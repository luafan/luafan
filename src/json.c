/*
 * json.c — C implementation of webase/ljson.lua (rxi-compatible strict JSON).
 *
 * Production: require "json" (package.preload).
 * Pure-Lua twin for regression: require "ljson" (webase/ljson.lua).
 *
 * Same public API as the pure-Lua module:
 *   encode / decode / array / object / null / enable_null
 *   is_present / is_nonempty_string / _version
 *
 * Strict empty-table rule: bare {} must error (use json.array() or json.object()).
 * Decode attaches array/object metatables so re-encode is unambiguous.
 */

#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define JSON_VERSION "0.1.2"

#define JSON_ARRAY_MT_KEY "json.array_mt"
#define JSON_OBJECT_MT_KEY "json.object_mt"
#define JSON_NULL_KEY "json.null_sentinel"
#define JSON_MOD_KEY "json.module"

/* ---- growable string buffer ------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

static void strbuf_init(strbuf_t *b) {
    b->cap = 256;
    b->len = 0;
    b->data = (char *)malloc(b->cap);
    if (b->data)
        b->data[0] = '\0';
}

static void strbuf_free(strbuf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int strbuf_reserve(strbuf_t *b, size_t need) {
    if (b->len + need + 1 <= b->cap)
        return 1;
    size_t ncap = b->cap ? b->cap : 256;
    while (b->len + need + 1 > ncap) {
        if (ncap > (SIZE_MAX / 2))
            return 0;
        ncap *= 2;
    }
    char *p = (char *)realloc(b->data, ncap);
    if (!p)
        return 0;
    b->data = p;
    b->cap = ncap;
    return 1;
}

static int strbuf_append(strbuf_t *b, const char *s, size_t n) {
    if (!b->data || !strbuf_reserve(b, n))
        return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int strbuf_append_char(strbuf_t *b, char c) {
    return strbuf_append(b, &c, 1);
}

static int strbuf_append_str(strbuf_t *b, const char *s) {
    return strbuf_append(b, s, strlen(s));
}

/* ---- registry helpers ------------------------------------------------------ */

static void push_array_mt(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, JSON_ARRAY_MT_KEY);
}

static void push_object_mt(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, JSON_OBJECT_MT_KEY);
}

static void push_null_sentinel(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, JSON_NULL_KEY);
}

static int mt_is_array(lua_State *L, int idx) {
    if (!lua_getmetatable(L, idx))
        return 0;
    push_array_mt(L);
    int ok = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return ok;
}

static int mt_is_object(lua_State *L, int idx) {
    if (!lua_getmetatable(L, idx))
        return 0;
    push_object_mt(L);
    int ok = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return ok;
}

static int table_is_empty(lua_State *L, int idx) {
    lua_pushnil(L);
    if (lua_next(L, idx)) {
        lua_pop(L, 2);
        return 0;
    }
    return 1;
}

/* First useful Lua call site outside this C module (mirrors webase/ljson.lua). */
static void caller_site(lua_State *L, char *buf, size_t buflen) {
    lua_Debug ar;
    char fallback[256];
    fallback[0] = '\0';

    for (int level = 1; level <= 32; level++) {
        if (!lua_getstack(L, level, &ar))
            break;
        if (!lua_getinfo(L, "Sl", &ar))
            continue;
        if (ar.what && ar.what[0] == 'C')
            continue;

        const char *label_src = ar.short_src ? ar.short_src : "?";
        int is_chunk = (ar.source && ar.source[0] == '=');
        if (is_chunk)
            label_src = ar.source + 1;

        if (is_chunk && ar.currentline > 0) {
            snprintf(buf, buflen, "%s:%d", label_src, ar.currentline);
            return;
        }
        if (!fallback[0]) {
            if (ar.currentline > 0)
                snprintf(fallback, sizeof(fallback), "%s:%d", label_src, ar.currentline);
            else
                snprintf(fallback, sizeof(fallback), "%s", label_src);
        }
    }
    if (fallback[0])
        snprintf(buf, buflen, "%s", fallback);
    else
        snprintf(buf, buflen, "?");
}

/* Raise without C location prefix (Lua error level 0). */
static int json_error(lua_State *L, const char *msg) {
    lua_pushstring(L, msg);
    return lua_error(L);
}

static int json_errorf(lua_State *L, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return json_error(L, buf);
}

/* Free encode buffer then raise (lua_error longjmps; avoid heap leak). */
static int encode_error(lua_State *L, strbuf_t *buf, const char *msg) {
    strbuf_free(buf);
    return json_error(L, msg);
}

static int encode_errorf(lua_State *L, strbuf_t *buf, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    return encode_error(L, buf, msg);
}

/* ---- encode ---------------------------------------------------------------- */

static void encode_value(lua_State *L, int idx, strbuf_t *buf, int stack_idx);

static void encode_string(lua_State *L, int idx, strbuf_t *buf) {
    size_t len;
    const char *s = lua_tolstring(L, idx, &len);
    if (!strbuf_append_char(buf, '"'))
        encode_error(L, buf, "out of memory");

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char uesc[8];
        switch (c) {
        case '\\':
            esc = "\\\\";
            break;
        case '"':
            esc = "\\\"";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        default:
            if (c < 0x20) {
                snprintf(uesc, sizeof(uesc), "\\u%04x", c);
                esc = uesc;
            }
            break;
        }
        if (esc) {
            if (!strbuf_append_str(buf, esc))
                encode_error(L, buf, "out of memory");
        } else {
            if (!strbuf_append_char(buf, (char)c))
                encode_error(L, buf, "out of memory");
        }
    }
    if (!strbuf_append_char(buf, '"'))
        encode_error(L, buf, "out of memory");
}

static void encode_number(lua_State *L, int idx, strbuf_t *buf) {
    lua_Number val = lua_tonumber(L, idx);
    if (val != val || val <= -HUGE_VAL || val >= HUGE_VAL) {
        const char *label = (val != val) ? "nan" : (val >= HUGE_VAL) ? "inf" : "-inf";
        encode_errorf(L, buf, "unexpected number value '%s'", label);
    }
    /* Match Lua string.format("%.14g"): IEEE -0 becomes "0", not "-0". */
    if (val == 0)
        val += 0; /* (-0) + (+0) => +0; avoids dead-store elision of val=0 */
    char tmp[64];
    /* Match webase/ljson.lua: string.format("%.14g", val) */
    int n = snprintf(tmp, sizeof(tmp), "%.14g", (double)val);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        encode_error(L, buf, "number format failed");
    if (!strbuf_append(buf, tmp, (size_t)n))
        encode_error(L, buf, "out of memory");
}

static void encode_table(lua_State *L, int idx, strbuf_t *buf, int stack_idx) {
    idx = lua_absindex(L, idx);

    /* Circular reference? stack is a Lua table map: t -> true */
    lua_pushvalue(L, idx);
    lua_rawget(L, stack_idx);
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        encode_error(L, buf, "circular reference");
    }
    lua_pop(L, 1);

    lua_pushvalue(L, idx);
    lua_pushboolean(L, 1);
    lua_rawset(L, stack_idx);

    int is_arr_mt = mt_is_array(L, idx);
    int is_obj_mt = mt_is_object(L, idx);

    lua_rawgeti(L, idx, 1);
    int has_index_1 = !lua_isnil(L, -1);
    lua_pop(L, 1);

    int empty = table_is_empty(L, idx);

    /* Same branch condition as webase/ljson.lua encode_table */
    if (has_index_1 || is_arr_mt || (!is_obj_mt && empty)) {
        if (!is_arr_mt && !is_obj_mt && empty) {
            char site[256];
            caller_site(L, site, sizeof(site));
            encode_errorf(L, buf,
                          "ambiguous empty table at %s: use json.array() for [] or "
                          "json.object() for {}",
                          site);
        }

        /* Count keys; require dense integer array */
        int n = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) != LUA_TNUMBER) {
                lua_pop(L, 2);
                encode_error(L, buf, "invalid table: mixed or invalid key types");
            }
            n++;
            lua_pop(L, 1); /* value */
        }
        int arr_len = (int)lua_rawlen(L, idx);
        if (n != arr_len) {
            encode_error(L, buf, "invalid table: sparse array");
        }

        if (!strbuf_append_char(buf, '['))
            encode_error(L, buf, "out of memory");
        for (int i = 1; i <= arr_len; i++) {
            if (i > 1 && !strbuf_append_char(buf, ','))
                encode_error(L, buf, "out of memory");
            lua_rawgeti(L, idx, i);
            encode_value(L, -1, buf, stack_idx);
            lua_pop(L, 1);
        }
        if (!strbuf_append_char(buf, ']'))
            encode_error(L, buf, "out of memory");
    } else {
        /* Object */
        if (!strbuf_append_char(buf, '{'))
            encode_error(L, buf, "out of memory");
        int first = 1;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            /* key at -2, value at -1 */
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 2);
                encode_error(L, buf, "invalid table: mixed or invalid key types");
            }
            if (!first && !strbuf_append_char(buf, ','))
                encode_error(L, buf, "out of memory");
            first = 0;
            encode_string(L, -2, buf);
            if (!strbuf_append_char(buf, ':'))
                encode_error(L, buf, "out of memory");
            encode_value(L, -1, buf, stack_idx);
            lua_pop(L, 1); /* value; keep key for next */
        }
        if (!strbuf_append_char(buf, '}'))
            encode_error(L, buf, "out of memory");
    }

    lua_pushvalue(L, idx);
    lua_pushnil(L);
    lua_rawset(L, stack_idx);
}

static void encode_value(lua_State *L, int idx, strbuf_t *buf, int stack_idx) {
    idx = lua_absindex(L, idx);

    /* null sentinel */
    push_null_sentinel(L);
    if (lua_rawequal(L, idx, -1)) {
        lua_pop(L, 1);
        if (!strbuf_append_str(buf, "null"))
            encode_error(L, buf, "out of memory");
        return;
    }
    lua_pop(L, 1);

    int t = lua_type(L, idx);
    switch (t) {
    case LUA_TNIL:
        if (!strbuf_append_str(buf, "null"))
            encode_error(L, buf, "out of memory");
        break;
    case LUA_TBOOLEAN:
        if (!strbuf_append_str(buf, lua_toboolean(L, idx) ? "true" : "false"))
            encode_error(L, buf, "out of memory");
        break;
    case LUA_TNUMBER:
        encode_number(L, idx, buf);
        break;
    case LUA_TSTRING:
        encode_string(L, idx, buf);
        break;
    case LUA_TTABLE:
        encode_table(L, idx, buf, stack_idx);
        break;
    default:
        encode_errorf(L, buf, "unexpected type '%s'", lua_typename(L, t));
    }
}

static int json_encode(lua_State *L) {
    /* Ensure value at index 1 (encode() with no args → null). */
    if (lua_gettop(L) < 1)
        lua_pushnil(L);

    strbuf_t buf;
    strbuf_init(&buf);
    if (!buf.data)
        return json_error(L, "out of memory");

    /* stack map for circular refs (above the value) */
    lua_newtable(L);
    int stack_idx = lua_gettop(L);

    encode_value(L, 1, &buf, stack_idx);

    lua_pushlstring(L, buf.data, buf.len);
    strbuf_free(&buf);
    return 1;
}

/* ---- decode ---------------------------------------------------------------- */

typedef struct {
    const char *str;
    size_t len;
    size_t idx; /* 0-based */
} parse_t;

static void decode_error_at(lua_State *L, parse_t *p, size_t at, const char *msg) {
    int line = 1, col = 1;
    for (size_t i = 0; i < at && i < p->len; i++) {
        col++;
        if (p->str[i] == '\n') {
            line++;
            col = 1;
        }
    }
    char site[256];
    caller_site(L, site, sizeof(site));
    if (site[0] && !(site[0] == '?' && site[1] == '\0')) {
        json_errorf(L, "%s at line %d col %d (from %s)", msg, line, col, site);
    }
    json_errorf(L, "%s at line %d col %d", msg, line, col);
}

static int is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_delim(unsigned char c) {
    return is_space(c) || c == ']' || c == '}' || c == ',';
}

static size_t next_char(parse_t *p, size_t i, int (*pred)(unsigned char), int negate) {
    for (; i < p->len; i++) {
        int m = pred((unsigned char)p->str[i]);
        if (negate ? !m : m)
            return i;
    }
    return p->len;
}

static void codepoint_to_utf8(lua_State *L, parse_t *p, size_t err_at, unsigned int n,
                              char *out, size_t *out_len) {
    if (n <= 0x7f) {
        out[0] = (char)n;
        *out_len = 1;
    } else if (n <= 0x7ff) {
        out[0] = (char)(0xc0 | (n >> 6));
        out[1] = (char)(0x80 | (n & 0x3f));
        *out_len = 2;
    } else if (n <= 0xffff) {
        out[0] = (char)(0xe0 | (n >> 12));
        out[1] = (char)(0x80 | ((n >> 6) & 0x3f));
        out[2] = (char)(0x80 | (n & 0x3f));
        *out_len = 3;
    } else if (n <= 0x10ffff) {
        out[0] = (char)(0xf0 | (n >> 18));
        out[1] = (char)(0x80 | ((n >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((n >> 6) & 0x3f));
        out[3] = (char)(0x80 | (n & 0x3f));
        *out_len = 4;
    } else {
        decode_error_at(L, p, err_at, "invalid unicode codepoint");
    }
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned int hex4(const char *s) {
    unsigned int n = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        n <<= 4;
        if (c >= '0' && c <= '9')
            n |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            n |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            n |= (unsigned)(c - 'A' + 10);
    }
    return n;
}

/* Returns 1 if s points at a high-surrogate \uD800-\uDBFF form (4 hex digits). */
static int is_high_surrogate_hex(const char *s) {
    if (!is_hex(s[0]) || !is_hex(s[1]) || !is_hex(s[2]) || !is_hex(s[3]))
        return 0;
    char c0 = s[0];
    char c1 = s[1];
    if (!(c0 == 'd' || c0 == 'D'))
        return 0;
    if (!((c1 >= '8' && c1 <= '9') || (c1 >= 'a' && c1 <= 'b') || (c1 >= 'A' && c1 <= 'B')))
        return 0;
    return 1;
}

static void parse_value(lua_State *L, parse_t *p);

static void parse_string(lua_State *L, parse_t *p) {
    size_t i = p->idx;
    if (i >= p->len || p->str[i] != '"')
        decode_error_at(L, p, i, "expected string");
    i++; /* skip opening quote */

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    size_t k = i; /* start of unflushed raw span */

    while (i < p->len) {
        unsigned char x = (unsigned char)p->str[i];
        if (x < 32) {
            decode_error_at(L, p, i, "control character in string");
        } else if (x == '\\') {
            if (i > k)
                luaL_addlstring(&b, p->str + k, i - k);
            i++;
            if (i >= p->len)
                decode_error_at(L, p, p->idx, "expected closing quote for string");
            char c = p->str[i];
            if (c == 'u') {
                /* unicode escape: optional surrogate pair */
                size_t hex_start = i + 1;
                if (hex_start + 4 > p->len)
                    decode_error_at(L, p, i - 1, "invalid unicode escape in string");
                const char *hex = p->str + hex_start;
                size_t hex_len = 4;
                unsigned int cp;
                if (is_high_surrogate_hex(hex) && hex_start + 4 + 6 <= p->len &&
                    p->str[hex_start + 4] == '\\' && p->str[hex_start + 5] == 'u' &&
                    is_hex(p->str[hex_start + 6]) && is_hex(p->str[hex_start + 7]) &&
                    is_hex(p->str[hex_start + 8]) && is_hex(p->str[hex_start + 9])) {
                    unsigned int n1 = hex4(hex);
                    unsigned int n2 = hex4(p->str + hex_start + 6);
                    cp = (n1 - 0xd800) * 0x400 + (n2 - 0xdc00) + 0x10000;
                    hex_len = 10; /* XXXX\uYYYY */
                } else if (is_hex(hex[0]) && is_hex(hex[1]) && is_hex(hex[2]) && is_hex(hex[3])) {
                    cp = hex4(hex);
                    hex_len = 4;
                } else {
                    decode_error_at(L, p, i - 1, "invalid unicode escape in string");
                    return;
                }
                char utf8[4];
                size_t ulen = 0;
                codepoint_to_utf8(L, p, i - 1, cp, utf8, &ulen);
                luaL_addlstring(&b, utf8, ulen);
                i = hex_start + hex_len - 1; /* loop will i++ */
            } else {
                char out;
                switch (c) {
                case '\\':
                    out = '\\';
                    break;
                case '/':
                    out = '/';
                    break;
                case '"':
                    out = '"';
                    break;
                case 'b':
                    out = '\b';
                    break;
                case 'f':
                    out = '\f';
                    break;
                case 'n':
                    out = '\n';
                    break;
                case 'r':
                    out = '\r';
                    break;
                case 't':
                    out = '\t';
                    break;
                default: {
                    char emsg[64];
                    snprintf(emsg, sizeof(emsg), "invalid escape char '%c' in string", c);
                    decode_error_at(L, p, i - 1, emsg);
                    return;
                }
                }
                luaL_addchar(&b, out);
            }
            k = i + 1;
        } else if (x == '"') {
            if (i > k)
                luaL_addlstring(&b, p->str + k, i - k);
            luaL_pushresult(&b);
            p->idx = i + 1;
            return;
        }
        i++;
    }
    decode_error_at(L, p, p->idx, "expected closing quote for string");
}

static void parse_number(lua_State *L, parse_t *p) {
    size_t i = p->idx;
    size_t x = next_char(p, i, is_delim, 0);
    size_t nlen = x - i;
    if (nlen == 0 || nlen >= 128)
        decode_error_at(L, p, i, "invalid number");
    char tmp[128];
    memcpy(tmp, p->str + i, nlen);
    tmp[nlen] = '\0';

    char *end = NULL;
    double n = strtod(tmp, &end);
    if (end != tmp + nlen) {
        char emsg[160];
        snprintf(emsg, sizeof(emsg), "invalid number '%s'", tmp);
        decode_error_at(L, p, i, emsg);
    }
    /* Match Lua tonumber(): normalize IEEE -0 to +0. */
    if (n == 0.0)
        n += 0.0; /* (-0) + (+0) => +0 */
    lua_pushnumber(L, (lua_Number)n);
    p->idx = x;
}

static void parse_literal(lua_State *L, parse_t *p) {
    size_t i = p->idx;
    size_t x = next_char(p, i, is_delim, 0);
    size_t nlen = x - i;
    const char *w = p->str + i;

    if (nlen == 4 && memcmp(w, "true", 4) == 0) {
        lua_pushboolean(L, 1);
    } else if (nlen == 5 && memcmp(w, "false", 5) == 0) {
        lua_pushboolean(L, 0);
    } else if (nlen == 4 && memcmp(w, "null", 4) == 0) {
        /* json.null field on module (nil or sentinel) */
        lua_getfield(L, LUA_REGISTRYINDEX, JSON_MOD_KEY);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "null");
            lua_remove(L, -2);
        } else {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
    } else {
        char emsg[64];
        char word[32];
        size_t copy = nlen < 31 ? nlen : 31;
        memcpy(word, w, copy);
        word[copy] = '\0';
        snprintf(emsg, sizeof(emsg), "invalid literal '%s'", word);
        decode_error_at(L, p, i, emsg);
    }
    p->idx = x;
}

static void parse_array(lua_State *L, parse_t *p) {
    p->idx++; /* skip '[' */
    lua_newtable(L);
    push_array_mt(L);
    lua_setmetatable(L, -2);

    int n = 1;
    while (1) {
        p->idx = next_char(p, p->idx, is_space, 1);
        if (p->idx < p->len && p->str[p->idx] == ']') {
            p->idx++;
            break;
        }
        parse_value(L, p);
        lua_rawseti(L, -2, n++);
        p->idx = next_char(p, p->idx, is_space, 1);
        if (p->idx >= p->len)
            decode_error_at(L, p, p->idx, "expected ']' or ','");
        char chr = p->str[p->idx++];
        if (chr == ']')
            break;
        if (chr != ',')
            decode_error_at(L, p, p->idx, "expected ']' or ','");
    }
}

static void parse_object(lua_State *L, parse_t *p) {
    p->idx++; /* skip '{' */
    lua_newtable(L);
    push_object_mt(L);
    lua_setmetatable(L, -2);

    while (1) {
        p->idx = next_char(p, p->idx, is_space, 1);
        if (p->idx < p->len && p->str[p->idx] == '}') {
            p->idx++;
            break;
        }
        if (p->idx >= p->len || p->str[p->idx] != '"')
            decode_error_at(L, p, p->idx, "expected string for key");
        parse_string(L, p); /* key */
        p->idx = next_char(p, p->idx, is_space, 1);
        if (p->idx >= p->len || p->str[p->idx] != ':')
            decode_error_at(L, p, p->idx, "expected ':' after key");
        p->idx = next_char(p, p->idx + 1, is_space, 1);
        parse_value(L, p); /* value */
        lua_rawset(L, -3); /* t[key]=value */
        p->idx = next_char(p, p->idx, is_space, 1);
        if (p->idx >= p->len)
            decode_error_at(L, p, p->idx, "expected '}' or ','");
        char chr = p->str[p->idx++];
        if (chr == '}')
            break;
        if (chr != ',')
            decode_error_at(L, p, p->idx, "expected '}' or ','");
    }
}

static void parse_value(lua_State *L, parse_t *p) {
    if (p->idx >= p->len) {
        decode_error_at(L, p, p->idx, "unexpected character ''");
    }
    char chr = p->str[p->idx];
    switch (chr) {
    case '"':
        parse_string(L, p);
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '-':
        parse_number(L, p);
        break;
    case 't':
    case 'f':
    case 'n':
        parse_literal(L, p);
        break;
    case '[':
        parse_array(L, p);
        break;
    case '{':
        parse_object(L, p);
        break;
    default: {
        char emsg[48];
        snprintf(emsg, sizeof(emsg), "unexpected character '%c'", chr);
        decode_error_at(L, p, p->idx, emsg);
    }
    }
}

static int json_decode(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        return json_errorf(L, "expected argument of type string, got %s",
                           lua_typename(L, lua_type(L, 1)));
    }
    size_t len;
    const char *s = lua_tolstring(L, 1, &len);
    parse_t p;
    p.str = s;
    p.len = len;
    p.idx = next_char(&p, 0, is_space, 1);
    parse_value(L, &p);
    p.idx = next_char(&p, p.idx, is_space, 1);
    if (p.idx < p.len)
        decode_error_at(L, &p, p.idx, "trailing garbage");
    return 1;
}

/* ---- helpers: array / object / null / is_* --------------------------------- */

static int json_array(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_newtable(L);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushvalue(L, 1);
    }
    push_array_mt(L);
    lua_setmetatable(L, -2);
    return 1;
}

static int json_object(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_newtable(L);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushvalue(L, 1);
    }
    push_object_mt(L);
    lua_setmetatable(L, -2);
    return 1;
}

static int json_enable_null(lua_State *L) {
    int enabled = lua_toboolean(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, JSON_MOD_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (enabled) {
        push_null_sentinel(L);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "null");
    lua_pop(L, 1);
    return 0;
}

/* True if value is present (not Lua nil / JSON null sentinel). */
static int json_is_present(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    push_null_sentinel(L);
    if (lua_rawequal(L, 1, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int json_is_nonempty_string(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t len;
    lua_tolstring(L, 1, &len);
    lua_pushboolean(L, len > 0);
    return 1;
}

/* ---- module open ----------------------------------------------------------- */

static const luaL_Reg json_funcs[] = {
    {"encode", json_encode},
    {"decode", json_decode},
    {"array", json_array},
    {"object", json_object},
    {"enable_null", json_enable_null},
    {"is_present", json_is_present},
    {"is_nonempty_string", json_is_nonempty_string},
    {NULL, NULL},
};

LUA_API int luaopen_json(lua_State *L) {
    /* Idempotent if package.preload / require loads more than once. */
    lua_getfield(L, LUA_REGISTRYINDEX, JSON_MOD_KEY);
    if (lua_istable(L, -1))
        return 1;
    lua_pop(L, 1);

    /* array / object metatables (identity only) */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, JSON_ARRAY_MT_KEY);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, JSON_OBJECT_MT_KEY);

    /* null sentinel: unique full userdata (same idea as fan.const) */
    lua_newuserdata(L, 0);
    lua_newtable(L);
    lua_pushstring(L, "const: json.null");
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, JSON_NULL_KEY);

    lua_newtable(L);
    luaL_register(L, NULL, json_funcs);

    lua_pushstring(L, JSON_VERSION);
    lua_setfield(L, -2, "_version");

    /* json.null defaults to nil (enable_null(true) switches to sentinel) */
    lua_pushnil(L);
    lua_setfield(L, -2, "null");

    /* keep module table for enable_null / decode null */
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, JSON_MOD_KEY);

    return 1;
}
