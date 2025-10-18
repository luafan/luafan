
#include "utlua.h"
#include <stdarg.h>
#include <time.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <arpa/inet.h>  // For ntohs, htons

// Endianness conversion functions
#ifdef __linux__
#include <endian.h>     // For be64toh, htobe64
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
// iOS doesn't have sys/endian.h, use libkern/OSByteOrder.h
#include <libkern/OSByteOrder.h>
#define be64toh(x) OSSwapBigToHostInt64(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#else
// macOS has sys/endian.h
#include <sys/endian.h>
#endif
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/endian.h> // For BSD systems
#else
// Fallback implementation for other systems
#include <netinet/in.h>
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define be64toh(x) __builtin_bswap64(x)
#define htobe64(x) __builtin_bswap64(x)
#else
#define be64toh(x) (x)
#define htobe64(x) (x)
#endif
#endif

typedef struct {
    lua_State *mainthread;

    struct evhttp *httpd;
    struct evhttp_bound_socket *boundsocket;

    char *host;
    int port;

#if FAN_HAS_OPENSSL
    SSL_CTX *ctx;
#endif
    int onServiceRef;

    // Performance and security configuration
    int enable_keep_alive;
    int keep_alive_timeout;
    int max_keep_alive_requests;
    size_t max_body_size;
} LuaServer;

#define HTTP_POST_BODY_LIMIT 100 * 1024 * 1024
#define MAX_READ_BUFFER_SIZE 1024 * 1024  // 1MB max read buffer

#define REPLY_STATUS_NONE 0        // not reply yet.
#define REPLY_STATUS_REPLYED 1     // replied
#define REPLY_STATUS_REPLY_START 2 // processing reply chunk, but not ended.

// WebSocket constants
#define WEBSOCKET_MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WEBSOCKET_GUID_LEN 36
#define WEBSOCKET_KEY_LEN 24

// WebSocket opcodes
typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT = 0x1,
    WS_OPCODE_BINARY = 0x2,
    WS_OPCODE_CLOSE = 0x8,
    WS_OPCODE_PING = 0x9,
    WS_OPCODE_PONG = 0xA
} websocket_opcode_t;

// WebSocket connection state
typedef enum {
    WS_STATE_CONNECTING = 0,
    WS_STATE_OPEN = 1,
    WS_STATE_CLOSING = 2,
    WS_STATE_CLOSED = 3
} websocket_state_t;

// WebSocket frame structure
typedef struct {
    int fin;                    // Final fragment flag
    int rsv1, rsv2, rsv3;      // Reserved bits
    websocket_opcode_t opcode;  // Frame opcode
    int masked;                 // Mask flag
    uint64_t payload_len;       // Payload length
    uint32_t mask_key;          // Masking key (if masked)
    char *payload;              // Payload data
} websocket_frame_t;

// Performance metrics structure
typedef struct {
    unsigned long requests_total;          // Total requests processed
    unsigned long requests_active;         // Currently active requests
    unsigned long bytes_sent;              // Total bytes sent
    unsigned long bytes_received;          // Total bytes received
    unsigned long errors_total;            // Total error responses (4xx/5xx)
    unsigned long memory_allocated;        // Current allocated memory estimate
    unsigned long connections_total;       // Total connections accepted
    unsigned long keepalive_reused;        // Keep-alive connections reused
    time_t start_time;                     // Server start time

    // Request statistics by method
    unsigned long requests_get;
    unsigned long requests_post;
    unsigned long requests_put;
    unsigned long requests_delete;
    unsigned long requests_other;

    // Response statistics by status class
    unsigned long responses_2xx;
    unsigned long responses_3xx;
    unsigned long responses_4xx;
    unsigned long responses_5xx;
} httpd_metrics_t;

// Global metrics instance
static httpd_metrics_t g_metrics = {0};

// Enhanced error logging functions
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} log_level_t;

static void httpd_log(log_level_t level, const char* format, ...) {
    const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);

    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_names[level], message);
    fflush(stderr);
}

#define LOG_ERROR_FMT(fmt, ...) httpd_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_WARN_FMT(fmt, ...) httpd_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_INFO_FMT(fmt, ...) httpd_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_FMT(fmt, ...) httpd_log(LOG_DEBUG, fmt, ##__VA_ARGS__)

// Metrics update functions
static void metrics_init() {
    memset(&g_metrics, 0, sizeof(g_metrics));
    g_metrics.start_time = time(NULL);
}

static void metrics_update_request_start(const char* method) {
    g_metrics.requests_total++;
    g_metrics.requests_active++;

    if (method) {
        if (strcmp(method, "GET") == 0) {
            g_metrics.requests_get++;
        } else if (strcmp(method, "POST") == 0) {
            g_metrics.requests_post++;
        } else if (strcmp(method, "PUT") == 0) {
            g_metrics.requests_put++;
        } else if (strcmp(method, "DELETE") == 0) {
            g_metrics.requests_delete++;
        } else {
            g_metrics.requests_other++;
        }
    }
}

static void metrics_update_request_end(int status_code, size_t bytes_sent) {
    if (g_metrics.requests_active > 0) {
        g_metrics.requests_active--;
    }

    g_metrics.bytes_sent += bytes_sent;

    // Update response statistics by status class
    if (status_code >= 200 && status_code < 300) {
        g_metrics.responses_2xx++;
    } else if (status_code >= 300 && status_code < 400) {
        g_metrics.responses_3xx++;
    } else if (status_code >= 400 && status_code < 500) {
        g_metrics.responses_4xx++;
        g_metrics.errors_total++;
    } else if (status_code >= 500) {
        g_metrics.responses_5xx++;
        g_metrics.errors_total++;
    }
}

static void metrics_update_memory_alloc(size_t size) {
    g_metrics.memory_allocated += size;
}

static void metrics_update_memory_free(size_t size) {
    if (g_metrics.memory_allocated >= size) {
        g_metrics.memory_allocated -= size;
    }
}

static void metrics_update_connection() {
    g_metrics.connections_total++;
}

static void metrics_update_keepalive_reuse() {
    g_metrics.keepalive_reused++;
}

typedef struct {
    char *name;
    enum evhttp_cmd_type cmd;
} MethodMap;

static const MethodMap methodMap[] = {{"GET", EVHTTP_REQ_GET},       {"POST", EVHTTP_REQ_POST},
                                      {"HEAD", EVHTTP_REQ_HEAD},     {"PUT", EVHTTP_REQ_PUT},
                                      {"DELETE", EVHTTP_REQ_DELETE}, {"OPTIONS", EVHTTP_REQ_OPTIONS},
                                      {"TRACE", EVHTTP_REQ_TRACE},   {"CONNECT", EVHTTP_REQ_CONNECT},
                                      {"PATCH", EVHTTP_REQ_PATCH},   {NULL, EVHTTP_REQ_GET}};

typedef struct {
    struct evhttp_request *req;

    int reply_status;

    // WebSocket support
    int is_websocket;
    websocket_state_t ws_state;
    struct bufferevent *ws_bev;
} Request;

#define LUA_EVHTTP_REQUEST_TYPE "EVHTTP_REQUEST_TYPE"
#define LUA_EVHTTP_SERVER_TYPE "EVHTTP_SERVER_TYPE"

static Request *request_from_table(lua_State *L, int idx) {
    lua_rawgeti(L, idx, 1);

    Request *request = (Request *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    return request;
}

static void newtable_from_req(lua_State *L, struct evhttp_request *req) {
    lua_newtable(L);
    Request *request = (Request *)lua_newuserdata(L, sizeof(Request));
    request->reply_status = REPLY_STATUS_NONE;
    request->req = req;
    request->is_websocket = 0;
    request->ws_state = WS_STATE_CONNECTING;
    request->ws_bev = NULL;
    lua_rawseti(L, -2, 1);
}

// WebSocket utility functions
static int is_websocket_upgrade_request(struct evhttp_request *req) {
    const char *upgrade = evhttp_find_header(req->input_headers, "Upgrade");
    const char *connection = evhttp_find_header(req->input_headers, "Connection");
    const char *ws_key = evhttp_find_header(req->input_headers, "Sec-WebSocket-Key");
    const char *ws_version = evhttp_find_header(req->input_headers, "Sec-WebSocket-Version");

    return (upgrade && strcasecmp(upgrade, "websocket") == 0 &&
            connection && strcasestr(connection, "upgrade") != NULL &&
            ws_key && strlen(ws_key) > 0 &&
            ws_version && strcmp(ws_version, "13") == 0);
}

static char* generate_websocket_accept_key(const char* client_key) {
    if (!client_key) return NULL;

    // Concatenate client key with WebSocket GUID
    char combined[WEBSOCKET_KEY_LEN + WEBSOCKET_GUID_LEN + 1];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WEBSOCKET_MAGIC_STRING);

    // Calculate SHA-1 hash
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), hash);

    // Base64 encode the hash
    BIO *bio_mem = BIO_new(BIO_s_mem());
    BIO *bio_b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(bio_b64, bio_mem);

    BIO_write(bio_b64, hash, SHA_DIGEST_LENGTH);
    BIO_flush(bio_b64);

    BUF_MEM *buf_mem = NULL;
    BIO_get_mem_ptr(bio_b64, &buf_mem);

    char *accept_key = malloc(buf_mem->length + 1);
    if (accept_key) {
        memcpy(accept_key, buf_mem->data, buf_mem->length);
        accept_key[buf_mem->length] = '\0';
    }

    BIO_free_all(bio_b64);
    return accept_key;
}

// WebSocket frame parsing and generation functions
static void websocket_mask_unmask(char *data, uint64_t len, uint32_t mask_key) {
    uint8_t *mask_bytes = (uint8_t *)&mask_key;
    for (uint64_t i = 0; i < len; i++) {
        data[i] ^= mask_bytes[i % 4];
    }
}

static int websocket_parse_frame(struct evbuffer *input, websocket_frame_t *frame) {
    size_t available = evbuffer_get_length(input);
    if (available < 2) {
        return 0; // Need at least 2 bytes for basic header
    }

    // Read first two bytes
    uint8_t header[2];
    evbuffer_copyout(input, header, 2);

    frame->fin = (header[0] & 0x80) != 0;
    frame->rsv1 = (header[0] & 0x40) != 0;
    frame->rsv2 = (header[0] & 0x20) != 0;
    frame->rsv3 = (header[0] & 0x10) != 0;
    frame->opcode = (websocket_opcode_t)(header[0] & 0x0F);
    frame->masked = (header[1] & 0x80) != 0;

    uint64_t payload_len = header[1] & 0x7F;
    size_t header_size = 2;

    // Determine actual payload length
    if (payload_len == 126) {
        if (available < 4) return 0;
        uint16_t len16;
        evbuffer_copyout(input, &len16, sizeof(len16));
        frame->payload_len = ntohs(len16);
        header_size += 2;
    } else if (payload_len == 127) {
        if (available < 10) return 0;
        uint64_t len64;
        evbuffer_copyout(input, &len64, sizeof(len64));
        frame->payload_len = be64toh(len64);
        header_size += 8;
    } else {
        frame->payload_len = payload_len;
    }

    // Add mask size if masked
    if (frame->masked) {
        header_size += 4;
    }

    // Check if we have the complete frame
    if (available < header_size + frame->payload_len) {
        return 0; // Incomplete frame
    }

    // Remove header from buffer
    evbuffer_drain(input, header_size - 2); // We already read 2 bytes

    // Read extended length if needed
    if (payload_len == 126) {
        uint16_t len16;
        evbuffer_remove(input, &len16, sizeof(len16));
    } else if (payload_len == 127) {
        uint64_t len64;
        evbuffer_remove(input, &len64, sizeof(len64));
    }

    // Read mask key if masked
    if (frame->masked) {
        evbuffer_remove(input, &frame->mask_key, 4);
    }

    // Read payload
    if (frame->payload_len > 0) {
        frame->payload = malloc(frame->payload_len);
        if (!frame->payload) {
            return -1; // Memory allocation failed
        }
        evbuffer_remove(input, frame->payload, frame->payload_len);

        // Unmask payload if needed
        if (frame->masked) {
            websocket_mask_unmask(frame->payload, frame->payload_len, frame->mask_key);
        }
    } else {
        frame->payload = NULL;
    }

    return 1; // Successfully parsed
}

static struct evbuffer* websocket_create_frame(websocket_opcode_t opcode, const char *payload,
                                             uint64_t payload_len, int fin) {
    struct evbuffer *frame = evbuffer_new();
    if (!frame) return NULL;

    // First byte: FIN + RSV + Opcode
    uint8_t first_byte = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    evbuffer_add(frame, &first_byte, 1);

    // Second byte and length
    if (payload_len < 126) {
        uint8_t second_byte = (uint8_t)payload_len;
        evbuffer_add(frame, &second_byte, 1);
    } else if (payload_len <= 65535) {
        uint8_t second_byte = 126;
        evbuffer_add(frame, &second_byte, 1);
        uint16_t len16 = htons((uint16_t)payload_len);
        evbuffer_add(frame, &len16, 2);
    } else {
        uint8_t second_byte = 127;
        evbuffer_add(frame, &second_byte, 1);
        uint64_t len64 = htobe64(payload_len);
        evbuffer_add(frame, &len64, 8);
    }

    // Add payload
    if (payload && payload_len > 0) {
        evbuffer_add(frame, payload, payload_len);
    }

    return frame;
}

static void websocket_frame_free(websocket_frame_t *frame) {
    if (frame && frame->payload) {
        free(frame->payload);
        frame->payload = NULL;
    }
}

// Smart connection management based on HTTP version and configuration
static void set_connection_header(struct evhttp_request *req, LuaServer *server) {
    if (!server->enable_keep_alive) {
        evhttp_add_header(req->output_headers, "Connection", "close");
        return;
    }

    // Check HTTP version - assume HTTP/1.1 for now due to API compatibility
    int major = 1, minor = 1;
    // evhttp_request_get_version(req, &major, &minor); // Not available in this libevent version

    if (major > 1 || (major == 1 && minor >= 1)) {
        // HTTP/1.1 defaults to keep-alive
        const char *connection = evhttp_find_header(req->input_headers, "Connection");
        if (connection && strcasecmp(connection, "close") == 0) {
            evhttp_add_header(req->output_headers, "Connection", "close");
        } else {
            char keep_alive_header[64];
            snprintf(keep_alive_header, sizeof(keep_alive_header),
                    "timeout=%d, max=%d",
                    server->keep_alive_timeout,
                    server->max_keep_alive_requests);
            evhttp_add_header(req->output_headers, "Keep-Alive", keep_alive_header);
            evhttp_add_header(req->output_headers, "Connection", "keep-alive");
        }
    } else {
        // HTTP/1.0 requires explicit keep-alive
        const char *connection = evhttp_find_header(req->input_headers, "Connection");
        if (connection && strcasecmp(connection, "keep-alive") == 0) {
            char keep_alive_header[64];
            snprintf(keep_alive_header, sizeof(keep_alive_header),
                    "timeout=%d, max=%d",
                    server->keep_alive_timeout,
                    server->max_keep_alive_requests);
            evhttp_add_header(req->output_headers, "Keep-Alive", keep_alive_header);
            evhttp_add_header(req->output_headers, "Connection", "keep-alive");
        } else {
            evhttp_add_header(req->output_headers, "Connection", "close");
        }
    }
}

static void httpd_handler_cgi_bin(struct evhttp_request *req, LuaServer *server) {
    // Update connection metrics
    metrics_update_connection();

    // Get request method for metrics
    const char* method = NULL;
    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    const MethodMap *method_map;
    for (method_map = methodMap; method_map->name; method_map++) {
        if (cmd == method_map->cmd) {
            method = method_map->name;
            break;
        }
    }

    // Update request start metrics
    metrics_update_request_start(method);

    lua_State *mainthread = server->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, server->onServiceRef);

    newtable_from_req(co, req);

    luaL_getmetatable(co, LUA_EVHTTP_REQUEST_TYPE);
    lua_setmetatable(co, -2);

    lua_pushvalue(co, -1); // duplicate for req,resp

    FAN_RESUME(co, mainthread, 2);
    POP_REF(mainthread);
}

static void request_push_body(lua_State *L, int idx) {
    if (idx < 0) {
        idx = lua_gettop(L) + idx + 1;
    }
    lua_rawgetp(L, idx, "body");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);

        struct evhttp_request *req = request_from_table(L, idx)->req;
        struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
        if (bodybuf) {
            size_t len = evbuffer_get_length(bodybuf);

            // Get configured body size limit
            size_t body_limit = HTTP_POST_BODY_LIMIT;  // Default fallback
            lua_getfield(L, LUA_REGISTRYINDEX, "httpd_server");
            if (!lua_isnil(L, -1)) {
                LuaServer *server = (LuaServer *)lua_touserdata(L, -1);
                if (server) {
                    body_limit = server->max_body_size;
                }
            }
            lua_pop(L, 1);

            if (len > 0 && len < body_limit) {
                char *data = calloc(1, len + 1);
                if (!data) {
                    // Critical fix: Handle memory allocation failure
                    LOG_ERROR_FMT("Memory allocation failed for request body: %zu bytes", len + 1);
                    lua_pushnil(L);
                    return;
                }

                size_t total_read = 0;
                char *ptrdata = data;
                while (total_read < len) {
                    int read = evbuffer_remove(bodybuf, ptrdata, len - total_read);
                    if (read <= 0) break;
                    ptrdata += read;
                    total_read += read;
                }

                lua_pushlstring(L, data, total_read);
                free(data);

#if (LUA_VERSION_NUM >= 502)
                lua_pushvalue(L, -1);
                lua_rawsetp(L, idx, "body");
#else
                lua_pushliteral(L, "body");
                lua_pushvalue(L, -2);
                lua_rawset(L, idx);
#endif
                return;
            }
        }

        lua_pushnil(L);
    }
}

LUA_API int lua_evhttp_request_available(lua_State *L) {
    struct evhttp_request *req = request_from_table(L, 1)->req;
    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
    if (bodybuf) {
        size_t len = evbuffer_get_length(bodybuf);
        lua_pushinteger(L, len);
    } else {
        lua_pushinteger(L, 0);
    }

    return 1;
}

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

LUA_API int lua_evhttp_request_read(lua_State *L) {
    struct evhttp_request *req = request_from_table(L, 1)->req;
    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
    size_t evbuffer_length = evbuffer_get_length(bodybuf);

    if (evbuffer_length) {
        lua_Integer buff_len = luaL_optinteger(L, 2, MIN(READ_BUFF_LEN, evbuffer_length));

        // Critical fix: Validate buffer length and check allocation
        if (buff_len <= 0 || buff_len > MAX_READ_BUFFER_SIZE) {
            lua_pushnil(L);
            return 1;
        }

        char *data = malloc(buff_len);
        if (!data) {
            // Critical fix: Handle memory allocation failure
            LOG_ERROR_FMT("Memory allocation failed for read buffer: %ld bytes", (long)buff_len);
            lua_pushnil(L);
            return 1;
        }

        int read = evbuffer_remove(bodybuf, data, buff_len);
        if (read > 0) {
            lua_pushlstring(L, data, read);
        } else {
            lua_pushnil(L);
        }
        free(data);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

LUA_API int lua_evhttp_request_reply(lua_State *L) {
    Request *request = request_from_table(L, 1);
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            evhttp_send_reply_end(request->req);
            request->reply_status = REPLY_STATUS_REPLYED;
            lua_settop(L, 1);
            return 1;
        default:
            break;
    }

    int responseCode = (int)lua_tointeger(L, 2);
    const char *responseMessage = lua_tostring(L, 3);

    size_t responseBuffLen = 0;
    const char *responseBuff = lua_tolstring(L, 4, &responseBuffLen);

    // Get server context for smart connection management
    LuaServer *server = NULL;
    lua_getfield(L, LUA_REGISTRYINDEX, "httpd_server");
    if (!lua_isnil(L, -1)) {
        server = (LuaServer *)lua_touserdata(L, -1);
    }
    lua_pop(L, 1);

    if (server) {
        set_connection_header(request->req, server);
    } else {
        // Fallback to close if no server context
        evhttp_add_header(request->req->output_headers, "Connection", "close");
    }

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        LOG_ERROR_FMT("Failed to create response buffer for request");
        return luaL_error(L, "Failed to create response buffer");
    }

    if (responseBuff && responseBuffLen > 0) {
        if (evbuffer_add(buf, responseBuff, responseBuffLen) < 0) {
            evbuffer_free(buf);
            LOG_ERROR_FMT("Failed to add %zu bytes to response buffer", responseBuffLen);
            return luaL_error(L, "Failed to add data to response buffer");
        }
    }

    evhttp_send_reply(request->req, responseCode, responseMessage, buf);
    evbuffer_free(buf);

    request->reply_status = REPLY_STATUS_REPLYED;

    // Update response metrics
    metrics_update_request_end(responseCode, responseBuffLen);

    return 0;
}

LUA_API int lua_evhttp_request_reply_addheader(lua_State *L) {
    Request *request = request_from_table(L, 1);
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            return luaL_error(L, "reply has started already.");
        default:
            break;
    }

    const char *key = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);

    evhttp_add_header(request->req->output_headers, key, value);

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_start(lua_State *L) {
    Request *request = request_from_table(L, 1);
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            return luaL_error(L, "reply has started already.");
        default:
            break;
    }

    int responseCode = (int)lua_tointeger(L, 2);
    const char *responseMessage = lua_tostring(L, 3);
    evhttp_send_reply_start(request->req, responseCode, responseMessage);
    request->reply_status = REPLY_STATUS_REPLY_START;

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_chunk(lua_State *L) {
    Request *request = request_from_table(L, 1);
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_NONE:
            return luaL_error(L, "reply has not started yet.");
        default:
            break;
    }

    int top = lua_gettop(L);
    if (top > 1) {
        struct evbuffer *buf = evbuffer_new();
        if (!buf) {
            return luaL_error(L, "Failed to create chunk buffer");
        }

        int i = 2;
        for (; i <= top; i++) {
            size_t responseBuffLen = 0;
            const char *responseBuff = lua_tolstring(L, i, &responseBuffLen);
            if (responseBuff && responseBuffLen > 0) {
                if (evbuffer_add(buf, responseBuff, responseBuffLen) < 0) {
                    evbuffer_free(buf);
                    return luaL_error(L, "Failed to add data to chunk buffer");
                }
            }
        }
        evhttp_send_reply_chunk(request->req, buf);
        evbuffer_free(buf);
    }

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_end(lua_State *L) {
    Request *request = request_from_table(L, 1);
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_NONE:
            return luaL_error(L, "reply has not started yet.");
        default:
            break;
    }

    evhttp_send_reply_end(request->req);
    request->reply_status = REPLY_STATUS_REPLYED;

    lua_settop(L, 1);
    return 1;
}

// WebSocket API functions
LUA_API int lua_evhttp_request_is_websocket_upgrade(lua_State *L) {
    Request *request = request_from_table(L, 1);
    struct evhttp_request *req = request->req;

    int is_upgrade = is_websocket_upgrade_request(req);
    lua_pushboolean(L, is_upgrade);
    return 1;
}

LUA_API int lua_evhttp_request_websocket_accept(lua_State *L) {
    Request *request = request_from_table(L, 1);
    struct evhttp_request *req = request->req;

    if (request->reply_status != REPLY_STATUS_NONE) {
        return luaL_error(L, "Response already started");
    }

    if (!is_websocket_upgrade_request(req)) {
        return luaL_error(L, "Not a valid WebSocket upgrade request");
    }

    const char *ws_key = evhttp_find_header(req->input_headers, "Sec-WebSocket-Key");
    if (!ws_key) {
        return luaL_error(L, "Missing Sec-WebSocket-Key header");
    }

    char *accept_key = generate_websocket_accept_key(ws_key);
    if (!accept_key) {
        return luaL_error(L, "Failed to generate WebSocket accept key");
    }

    // Send WebSocket handshake response
    struct evbuffer *response = evbuffer_new();
    if (!response) {
        free(accept_key);
        return luaL_error(L, "Failed to create response buffer");
    }

    evbuffer_add_printf(response,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_key);

    // Send the handshake response manually
    struct evhttp_connection *evcon = evhttp_request_get_connection(req);
    struct bufferevent *bev = evhttp_connection_get_bufferevent(evcon);

    if (bev) {
        bufferevent_write_buffer(bev, response);

        // Mark as WebSocket connection
        request->is_websocket = 1;
        request->ws_state = WS_STATE_OPEN;
        request->ws_bev = bev;
        request->reply_status = REPLY_STATUS_REPLYED;

        LOG_INFO_FMT("WebSocket connection established for request");
    }

    evbuffer_free(response);
    free(accept_key);

    lua_pushboolean(L, bev != NULL);
    return 1;
}

// WebSocket data sending function
LUA_API int lua_evhttp_request_websocket_send(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket || request->ws_state != WS_STATE_OPEN) {
        return luaL_error(L, "WebSocket connection not open");
    }

    if (!request->ws_bev) {
        return luaL_error(L, "WebSocket connection not available");
    }

    // Get message data and optional opcode
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 2, &data_len);
    int opcode = luaL_optinteger(L, 3, WS_OPCODE_TEXT);
    int fin = luaL_optinteger(L, 4, 1);  // Default to final frame

    // Validate opcode
    if (opcode < 0 || opcode > 0xF) {
        return luaL_error(L, "Invalid WebSocket opcode: %d", opcode);
    }

    // Create WebSocket frame
    struct evbuffer *frame = websocket_create_frame((websocket_opcode_t)opcode, data, data_len, fin);
    if (!frame) {
        return luaL_error(L, "Failed to create WebSocket frame");
    }

    // Send frame
    int result = bufferevent_write_buffer(request->ws_bev, frame);
    evbuffer_free(frame);

    lua_pushboolean(L, result == 0);
    return 1;
}

// WebSocket ping function
LUA_API int lua_evhttp_request_websocket_ping(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket || request->ws_state != WS_STATE_OPEN) {
        return luaL_error(L, "WebSocket connection not open");
    }

    if (!request->ws_bev) {
        return luaL_error(L, "WebSocket connection not available");
    }

    // Optional ping payload
    size_t payload_len = 0;
    const char *payload = lua_tolstring(L, 2, &payload_len);

    // Ping payload must be <= 125 bytes
    if (payload_len > 125) {
        return luaL_error(L, "Ping payload too large (max 125 bytes)");
    }

    struct evbuffer *frame = websocket_create_frame(WS_OPCODE_PING, payload, payload_len, 1);
    if (!frame) {
        return luaL_error(L, "Failed to create ping frame");
    }

    int result = bufferevent_write_buffer(request->ws_bev, frame);
    evbuffer_free(frame);

    lua_pushboolean(L, result == 0);
    return 1;
}

// WebSocket pong function
LUA_API int lua_evhttp_request_websocket_pong(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket || request->ws_state != WS_STATE_OPEN) {
        return luaL_error(L, "WebSocket connection not open");
    }

    if (!request->ws_bev) {
        return luaL_error(L, "WebSocket connection not available");
    }

    // Optional pong payload (usually echo of ping payload)
    size_t payload_len = 0;
    const char *payload = lua_tolstring(L, 2, &payload_len);

    // Pong payload must be <= 125 bytes
    if (payload_len > 125) {
        return luaL_error(L, "Pong payload too large (max 125 bytes)");
    }

    struct evbuffer *frame = websocket_create_frame(WS_OPCODE_PONG, payload, payload_len, 1);
    if (!frame) {
        return luaL_error(L, "Failed to create pong frame");
    }

    int result = bufferevent_write_buffer(request->ws_bev, frame);
    evbuffer_free(frame);

    lua_pushboolean(L, result == 0);
    return 1;
}

// WebSocket close function
LUA_API int lua_evhttp_request_websocket_close(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket) {
        return luaL_error(L, "Not a WebSocket connection");
    }

    if (request->ws_state == WS_STATE_CLOSED) {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (!request->ws_bev) {
        request->ws_state = WS_STATE_CLOSED;
        lua_pushboolean(L, 1);
        return 1;
    }

    // Optional close code and reason
    int close_code = luaL_optinteger(L, 2, 1000); // Normal closure
    size_t reason_len = 0;
    const char *reason = lua_tolstring(L, 3, &reason_len);

    // Create close payload
    char close_payload[127]; // Max close frame payload
    size_t close_payload_len = 0;

    if (close_code >= 1000 && close_code <= 4999) {
        // Add close code as 2-byte big-endian
        close_payload[0] = (close_code >> 8) & 0xFF;
        close_payload[1] = close_code & 0xFF;
        close_payload_len = 2;

        // Add reason if provided
        if (reason && reason_len > 0) {
            size_t max_reason = sizeof(close_payload) - 2;
            if (reason_len > max_reason) reason_len = max_reason;
            memcpy(close_payload + 2, reason, reason_len);
            close_payload_len += reason_len;
        }
    }

    struct evbuffer *frame = websocket_create_frame(WS_OPCODE_CLOSE,
                                                   close_payload_len > 0 ? close_payload : NULL,
                                                   close_payload_len, 1);
    if (frame) {
        bufferevent_write_buffer(request->ws_bev, frame);
        evbuffer_free(frame);
    }

    request->ws_state = WS_STATE_CLOSING;

    lua_pushboolean(L, 1);
    return 1;
}

// WebSocket state query function
LUA_API int lua_evhttp_request_websocket_state(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket) {
        lua_pushstring(L, "not_websocket");
        return 1;
    }

    const char *state_names[] = {"connecting", "open", "closing", "closed"};
    lua_pushstring(L, state_names[request->ws_state]);
    return 1;
}

// WebSocket data reading function (for handling incoming frames)
static void websocket_handle_incoming_frame(Request *request, websocket_frame_t *frame) {
    if (!request || !frame) return;

    switch (frame->opcode) {
        case WS_OPCODE_TEXT:
        case WS_OPCODE_BINARY:
            // Application data - would typically be passed to Lua callback
            LOG_INFO_FMT("WebSocket received %s frame: %llu bytes",
                        frame->opcode == WS_OPCODE_TEXT ? "text" : "binary",
                        (unsigned long long)frame->payload_len);
            break;

        case WS_OPCODE_CLOSE:
            LOG_INFO_FMT("WebSocket close frame received");
            request->ws_state = WS_STATE_CLOSED;
            if (request->ws_bev) {
                bufferevent_free(request->ws_bev);
                request->ws_bev = NULL;
            }
            break;

        case WS_OPCODE_PING:
            LOG_INFO_FMT("WebSocket ping received, sending pong");
            // Auto-respond with pong
            if (request->ws_bev && request->ws_state == WS_STATE_OPEN) {
                struct evbuffer *pong = websocket_create_frame(WS_OPCODE_PONG,
                                                              frame->payload,
                                                              frame->payload_len, 1);
                if (pong) {
                    bufferevent_write_buffer(request->ws_bev, pong);
                    evbuffer_free(pong);
                }
            }
            break;

        case WS_OPCODE_PONG:
            LOG_INFO_FMT("WebSocket pong received");
            // Could be used for keepalive logic
            break;

        case WS_OPCODE_CONTINUATION:
            LOG_INFO_FMT("WebSocket continuation frame received");
            // Handle fragmented messages
            break;

        default:
            LOG_WARN_FMT("WebSocket unknown opcode: 0x%x", frame->opcode);
            break;
    }
}

static const struct luaL_Reg evhttp_request_lib[] = {
    {"read", lua_evhttp_request_read},
    {"available", lua_evhttp_request_available},

    {"addheader", lua_evhttp_request_reply_addheader},

    {"reply", lua_evhttp_request_reply},
    {"reply_start", lua_evhttp_request_reply_start},
    {"reply_chunk", lua_evhttp_request_reply_chunk},
    {"reply_end", lua_evhttp_request_reply_end},

    // WebSocket support
    {"is_websocket_upgrade", lua_evhttp_request_is_websocket_upgrade},
    {"websocket_accept", lua_evhttp_request_websocket_accept},
    {"websocket_send", lua_evhttp_request_websocket_send},
    {"websocket_ping", lua_evhttp_request_websocket_ping},
    {"websocket_pong", lua_evhttp_request_websocket_pong},
    {"websocket_close", lua_evhttp_request_websocket_close},
    {"websocket_state", lua_evhttp_request_websocket_state},
    {NULL, NULL},
};

LUA_API int lua_evhttp_server_gc(lua_State *L) {
    LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
    CLEAR_REF(L, server->onServiceRef)

    if (server->httpd) {
        if (server->boundsocket) {
            evhttp_del_accept_socket(server->httpd, server->boundsocket);
            server->boundsocket = NULL;
        }

        if (server->httpd) {
            evhttp_free(server->httpd);
            server->httpd = NULL;
        }
    }

#if FAN_HAS_OPENSSL
    if (server->ctx) {
        SSL_CTX_free(server->ctx);
    }
#endif
    lua_pop(L, 1);

    return 0;
}

LUA_API int lua_evhttp_request_lookup(lua_State *L) {
    struct evhttp_request *req = request_from_table(L, 1)->req;
    const char *p = lua_tostring(L, 2);

    const luaL_Reg *lib;

    for (lib = evhttp_request_lib; lib->func; lib++) {
        if (strcmp(p, lib->name) == 0) {
            lua_pushcfunction(L, lib->func);
            return 1;
        }
    }

    if (strcmp(p, "path") == 0) {
        lua_pushstring(L, evhttp_uri_get_path(req->uri_elems));
        return 1;
    } else if (strcmp(p, "query") == 0) {
        lua_pushstring(L, evhttp_uri_get_query(req->uri_elems));
        return 1;
    } else if (strcmp(p, "method") == 0) {
        const MethodMap *method;
        enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
        for (method = methodMap; method->name; method++) {
            if (cmd == method->cmd) {
                lua_pushstring(L, method->name);
                return 1;
            }
        }
    } else if (strcmp(p, "headers") == 0) {
        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);

        lua_newtable(L);
        struct evkeyval *item;
        TAILQ_FOREACH(item, headers, next) {
            lua_pushstring(L, item->value);
            lua_setfield(L, -2, item->key);
        }
        return 1;
    } else if (strcmp(p, "params") == 0) {
        struct evkeyvalq params;
        evhttp_parse_query_str(evhttp_uri_get_query(req->uri_elems), &params);

        lua_newtable(L);
        struct evkeyval *item;
        TAILQ_FOREACH(item, &params, next) {
            lua_pushstring(L, item->value);
            lua_setfield(L, -2, item->key);
        }
        evhttp_clear_headers(&params);

        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
        const char *contentType = evhttp_find_header(headers, "Content-Type");
        if (contentType && strstr(contentType, "application/x-www-form-urlencoded") == contentType) {
            request_push_body(L, 1);
            if (lua_type(L, -1) == LUA_TSTRING) {
                const char *data = lua_tostring(L, -1);
                struct evkeyvalq params;
                evhttp_parse_query_str(data, &params);
                lua_pop(L, 1);

                TAILQ_FOREACH(item, &params, next) {
                    lua_pushstring(L, item->value);
                    lua_setfield(L, -2, item->key);
                }
                evhttp_clear_headers(&params);
            }
        }
        return 1;
    } else if (strcmp(p, "body") == 0) {
        request_push_body(L, 1);
        return 1;
    } else if (strcmp(p, "remoteip") == 0) {
        char *address = NULL;
        ev_uint16_t port = 0;
        evhttp_connection_get_peer(req->evcon, &address, &port);

        lua_pushstring(L, address);

        return 1;
    } else if (strcmp(p, "remoteport") == 0) {
        char *address = NULL;
        ev_uint16_t port = 0;
        evhttp_connection_get_peer(req->evcon, &address, &port);

        lua_pushinteger(L, port);

        return 1;
    }

    return 0;
}

#if FAN_HAS_OPENSSL

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
static struct bufferevent *bevcb(struct event_base *base, void *arg) {
    struct bufferevent *r;
    SSL_CTX *ctx = (SSL_CTX *)arg;

    r = bufferevent_openssl_socket_new(base, -1, SSL_new(ctx), BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
    return r;
}
#endif
#endif

#endif

static void smoke_request_cb(struct evhttp_request *req, void *arg) {
    evhttp_send_reply(req, 200, "OK", NULL);
}

// Request statistics tracking
typedef struct {
    char path[256];
    char method[16];
    unsigned long count;
    unsigned long total_response_time_ms;
    unsigned long min_response_time_ms;
    unsigned long max_response_time_ms;
    time_t last_access;
} request_stat_t;

#define MAX_REQUEST_STATS 1000
static request_stat_t g_request_stats[MAX_REQUEST_STATS];
static int g_request_stats_count = 0;

static void update_request_statistics(const char* method, const char* path,
                                     unsigned long response_time_ms) {
    if (!method || !path) return;

    // Look for existing entry
    for (int i = 0; i < g_request_stats_count; i++) {
        if (strcmp(g_request_stats[i].method, method) == 0 &&
            strcmp(g_request_stats[i].path, path) == 0) {

            g_request_stats[i].count++;
            g_request_stats[i].total_response_time_ms += response_time_ms;
            g_request_stats[i].last_access = time(NULL);

            if (response_time_ms < g_request_stats[i].min_response_time_ms) {
                g_request_stats[i].min_response_time_ms = response_time_ms;
            }
            if (response_time_ms > g_request_stats[i].max_response_time_ms) {
                g_request_stats[i].max_response_time_ms = response_time_ms;
            }
            return;
        }
    }

    // Add new entry if we have space
    if (g_request_stats_count < MAX_REQUEST_STATS) {
        request_stat_t* stat = &g_request_stats[g_request_stats_count];
        strncpy(stat->path, path, sizeof(stat->path) - 1);
        stat->path[sizeof(stat->path) - 1] = '\0';
        strncpy(stat->method, method, sizeof(stat->method) - 1);
        stat->method[sizeof(stat->method) - 1] = '\0';
        stat->count = 1;
        stat->total_response_time_ms = response_time_ms;
        stat->min_response_time_ms = response_time_ms;
        stat->max_response_time_ms = response_time_ms;
        stat->last_access = time(NULL);
        g_request_stats_count++;
    }
}

// Metrics endpoint callback
static void metrics_request_cb(struct evhttp_request *req, void *arg) {
    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, 500, "Internal Server Error", NULL);
        return;
    }

    time_t now = time(NULL);
    time_t uptime = now - g_metrics.start_time;

    // Generate metrics in a simple text format
    evbuffer_add_printf(buf,
        "# HTTPD Server Metrics\n"
        "uptime_seconds %ld\n"
        "requests_total %lu\n"
        "requests_active %lu\n"
        "bytes_sent_total %lu\n"
        "bytes_received_total %lu\n"
        "errors_total %lu\n"
        "memory_allocated_bytes %lu\n"
        "connections_total %lu\n"
        "keepalive_reused_total %lu\n"
        "\n# Requests by method\n"
        "requests_get_total %lu\n"
        "requests_post_total %lu\n"
        "requests_put_total %lu\n"
        "requests_delete_total %lu\n"
        "requests_other_total %lu\n"
        "\n# Responses by status class\n"
        "responses_2xx_total %lu\n"
        "responses_3xx_total %lu\n"
        "responses_4xx_total %lu\n"
        "responses_5xx_total %lu\n",
        uptime,
        g_metrics.requests_total,
        g_metrics.requests_active,
        g_metrics.bytes_sent,
        g_metrics.bytes_received,
        g_metrics.errors_total,
        g_metrics.memory_allocated,
        g_metrics.connections_total,
        g_metrics.keepalive_reused,
        g_metrics.requests_get,
        g_metrics.requests_post,
        g_metrics.requests_put,
        g_metrics.requests_delete,
        g_metrics.requests_other,
        g_metrics.responses_2xx,
        g_metrics.responses_3xx,
        g_metrics.responses_4xx,
        g_metrics.responses_5xx
    );

    // Add detailed request statistics
    evbuffer_add_printf(buf, "\n# Request Statistics\n");
    for (int i = 0; i < g_request_stats_count; i++) {
        request_stat_t* stat = &g_request_stats[i];
        unsigned long avg_time = stat->count > 0 ?
            stat->total_response_time_ms / stat->count : 0;

        evbuffer_add_printf(buf,
            "request_stat{method=\"%s\",path=\"%s\"} "
            "count=%lu avg_ms=%lu min_ms=%lu max_ms=%lu last_access=%ld\n",
            stat->method, stat->path, stat->count, avg_time,
            stat->min_response_time_ms, stat->max_response_time_ms,
            stat->last_access);
    }

    evhttp_add_header(req->output_headers, "Content-Type", "text/plain; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

// Configuration validation functions
static int validate_httpd_config(LuaServer *server) {
    if (!server) return 0;

    // Validate timeout ranges
    if (server->keep_alive_timeout < 1 || server->keep_alive_timeout > 3600) {
        return 0; // 1 second to 1 hour
    }

    // Validate max requests per connection
    if (server->max_keep_alive_requests < 1 || server->max_keep_alive_requests > 10000) {
        return 0; // 1 to 10,000 requests
    }

    // Validate body size limit (1KB to 1GB)
    if (server->max_body_size < 1024 || server->max_body_size > 1073741824) {
        return 0;
    }

    // Validate port range
    if (server->port < 0 || server->port > 65535) {
        return 0;
    }

    return 1; // Valid configuration
}

void httpd_server_rebind(lua_State *L, LuaServer *server) {
    struct evhttp_bound_socket *boundsocket = evhttp_bind_socket_with_handle(server->httpd, server->host, server->port);

    server->boundsocket = boundsocket;
    if (boundsocket) {
        server->port = regress_get_socket_port(evhttp_bound_socket_get_fd(boundsocket));
    } else {
        server->port = 0;
    }
}

LUA_API int utd_bind(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    LuaServer *server = (LuaServer *)lua_newuserdata(L, sizeof(LuaServer));
    memset(server, 0, sizeof(LuaServer));
    luaL_getmetatable(L, LUA_EVHTTP_SERVER_TYPE);
    lua_setmetatable(L, -2);

    server->mainthread = utlua_mainthread(L);
    server->onServiceRef = LUA_NOREF;
    server->boundsocket = NULL;
    server->httpd = NULL;

    // Initialize configuration with defaults
    server->enable_keep_alive = 1;                    // Enable Keep-Alive by default
    server->keep_alive_timeout = 30;                  // 30 seconds timeout
    server->max_keep_alive_requests = 100;            // Max 100 requests per connection
    server->max_body_size = HTTP_POST_BODY_LIMIT;     // Use existing limit as default

    // Read configuration from Lua table if provided
    lua_getfield(L, 1, "enable_keep_alive");
    if (lua_isboolean(L, -1)) {
        server->enable_keep_alive = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "keep_alive_timeout");
    if (lua_isnumber(L, -1)) {
        server->keep_alive_timeout = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "max_keep_alive_requests");
    if (lua_isnumber(L, -1)) {
        server->max_keep_alive_requests = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "max_body_size");
    if (lua_isnumber(L, -1)) {
        server->max_body_size = (size_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    // Validate configuration before proceeding
    if (!validate_httpd_config(server)) {
        return luaL_error(L, "Invalid server configuration parameters");
    }

    struct evhttp *httpd = evhttp_new(event_mgr_base());

#if FAN_HAS_OPENSSL
    server->ctx = NULL;

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
    lua_getfield(L, 1, "cert");
    const char *cert = lua_tostring(L, -1);
    lua_getfield(L, 1, "key");
    const char *key = lua_tostring(L, -1);

    if (cert && key) {
#if OPENSSL_VERSION_NUMBER < 0x1010000fL
        SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
#else
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
#endif
        server->ctx = ctx;
        // Enhanced SSL security configuration
        SSL_CTX_set_options(ctx,
                            SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE |
                            SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |           // Disable weak protocols
                            SSL_OP_NO_COMPRESSION |                       // Prevent CRIME attacks
                            SSL_OP_CIPHER_SERVER_PREFERENCE);             // Server cipher preference

        // Set secure cipher suite
        SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS");

        EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!ecdh) {
            die_most_horribly_from_openssl_error("EC_KEY_new_by_curve_name");
        }

        if (1 != SSL_CTX_set_tmp_ecdh(ctx, ecdh)) {
            die_most_horribly_from_openssl_error("SSL_CTX_set_tmp_ecdh");
        }

        server_setup_certs(ctx, cert, key);

        evhttp_set_bevcb(httpd, bevcb, ctx);
    }

    lua_pop(L, 2);
#endif
#endif

#endif

    DUP_STR_FROM_TABLE(L, server->host, 1, "host")
    SET_INT_FROM_TABLE(L, server->port, 1, "port")

    server->httpd = httpd;

    httpd_server_rebind(L, server);

    if (!server->boundsocket) {
#if FAN_HAS_OPENSSL
        if (server->ctx) {
            SSL_CTX_free(server->ctx);
            server->ctx = NULL;
        }
#endif
        evhttp_free(httpd);
        server->httpd = NULL;
        return 0;
    }

    SET_FUNC_REF_FROM_TABLE(L, server->onServiceRef, 1, "onService")

    evhttp_set_timeout(httpd, server->keep_alive_timeout + 30);  // Timeout slightly longer than keep-alive
    evhttp_set_cb(httpd, "/smoketest", smoke_request_cb, NULL);
    evhttp_set_cb(httpd, "/metrics", metrics_request_cb, NULL);
    evhttp_set_gencb(httpd, (void (*)(struct evhttp_request *, void *))httpd_handler_cgi_bin, server);

    // Initialize metrics
    metrics_init();

    // Store server reference in registry for access in response functions
    lua_pushlightuserdata(L, server);
    lua_setfield(L, LUA_REGISTRYINDEX, "httpd_server");

    lua_newtable(L);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "serv");

    lua_pushinteger(L, server->port);
    lua_setfield(L, -2, "port");

    if (server->host) {
        lua_getfield(L, 1, "host");
    } else {
        lua_pushliteral(L, "0.0.0.0");
    }
    lua_setfield(L, -2, "host");

    return 1;
}

LUA_API int lua_evhttp_server_rebind(lua_State *L) {
    LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
    httpd_server_rebind(L, server);
    return 0;
}

static const luaL_Reg utdlib[] = {{"bind", utd_bind}, {NULL, NULL}};

LUA_API int luaopen_fan_httpd_core(lua_State *L) {
    luaL_newmetatable(L, LUA_EVHTTP_REQUEST_TYPE);

    lua_pushstring(L, "__index");
    lua_pushcfunction(L, &lua_evhttp_request_lookup);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    luaL_newmetatable(L, LUA_EVHTTP_SERVER_TYPE);

    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, &lua_evhttp_server_gc);
    lua_rawset(L, -3);

    lua_pushcfunction(L, &lua_evhttp_server_rebind);
    lua_setfield(L, -2, "rebind");

    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    lua_newtable(L);
    luaL_register(L, "httpd", utdlib);

    return 1;
}
