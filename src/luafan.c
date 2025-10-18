#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static struct event *mainevent;
static int main_ref;
static lua_State *mainState;

static void main_handler(const int fd, const short which, void *arg) {
    evtimer_del(mainevent);
    free(mainevent);

    lua_lock(mainState);
    lua_State *co = lua_newthread(mainState);
    PUSH_REF(mainState);
    lua_unlock(mainState);

    lua_rawgeti(co, LUA_REGISTRYINDEX, main_ref);
    FAN_RESUME(co, NULL, 0);

    luaL_unref(co, LUA_REGISTRYINDEX, main_ref);
    main_ref = LUA_NOREF;

    POP_REF(mainState);
}

LUA_API int luafan_start(lua_State *L) {
    // Check if we are already in a loop (nested call detection)
    if (event_mgr_is_looping()) {
        // Already in loop - nested call detected
        // But we still need to execute the function if provided
        if (lua_gettop(L) > 0 && lua_isfunction(L, 1)) {
            // Make a copy of the function and call it directly
            lua_pushvalue(L, 1);  // Copy the function to the top
            int result = lua_pcall(L, 0, 0, 0);
            if (result != LUA_OK) {
                // Handle error
                const char *error_msg = lua_tostring(L, -1);
                fprintf(stderr, "Error in nested fan.loop function: %s\n", error_msg);
                lua_pop(L, 1); // Remove error message
            }
        }

        // Skip the actual loop since we're already looping
        return 0;
    }

    // Not in a loop yet, set up the function for execution and start the loop
    if (lua_gettop(L) > 0) {
        if (lua_isfunction(L, 1)) {
            lua_settop(L, 1);

            main_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            mainState = utlua_mainthread(L);

            struct timeval t = {0, 1};
            mainevent = malloc(sizeof(struct event));
            if (!mainevent) {
                fprintf(stderr, "Memory allocation failed for main event: %zu bytes\n", sizeof(struct event));
                luaL_error(L, "Memory allocation failure");
                return 0;
            }
            evtimer_set(mainevent, main_handler, NULL);
            event_mgr_init();
            event_base_set(event_mgr_base(), mainevent);
            evtimer_add(mainevent, &t);
        }
    }

    // Start the actual event loop
    event_mgr_loop();
    return 0;
}

LUA_API int luafan_stop(lua_State *L) {
    event_mgr_break();
    return 0;
}

// -- luafan_sleep start --
struct sleep_args {
    lua_State *mainthread;
    int _ref_;
    struct event clockevent;
};

static void clock_handler(const int fd, const short which, void *arg) {
    struct sleep_args *args = (struct sleep_args *)arg;

    lua_State *L = NULL;
    REF_STATE_GET(args, L);

    evtimer_del(&args->clockevent);

    FAN_RESUME(L, NULL, 0);

    REF_STATE_CLEAR(args);
    free(args);
}

LUA_API int luafan_sleep(lua_State *L) {
    lua_Number sec = luaL_checknumber(L, 1);
    struct event_base *base = event_mgr_base();

    struct sleep_args *args = malloc(sizeof(struct sleep_args));
    if (!args) {
        fprintf(stderr, "Memory allocation failed for sleep args: %zu bytes\n", sizeof(struct sleep_args));
        luaL_error(L, "Memory allocation failure");
        return 0;
    }
    memset(args, 0, sizeof(struct sleep_args));

    REF_STATE_SET(args, L);

    struct timeval t = {0};
    d2tv(sec, &t);
    evtimer_set(&args->clockevent, clock_handler, args);
    event_base_set(base, &args->clockevent);
    evtimer_add(&args->clockevent, &t);

    return lua_yield(L, 0);
}
// -- luafan_sleep end --

// -- start hex2data data2hex --
static unsigned char strToChar(char a, char b) {
    char encoder[3] = {'\0', '\0', '\0'};
    encoder[0] = a;
    encoder[1] = b;
    return (char)strtol(encoder, NULL, 16);
}

LUA_API int hex2data(lua_State *L) {
    if (!lua_isstring(L, 1)) {
        return 0;
    }
    size_t length = 0;
    const char *bytes = lua_tolstring(L, 1, &length);

    char *r = (char *)malloc(length / 2 + 1);
    if (!r) {
        fprintf(stderr, "Memory allocation failed for hex2data: %zu bytes\n", length / 2 + 1);
        luaL_error(L, "Memory allocation failure");
        return 0;
    }
    char *index = r;

    while ((*bytes) && (*(bytes + 1))) {
        *index = strToChar(*bytes, *(bytes + 1));
        index++;
        bytes += 2;
    }
    *index = '\0';

    lua_pushlstring(L, r, length / 2);

    free(r);

    return 1;
}

LUA_API int data2hex(lua_State *L) {
    if (!lua_isstring(L, 1)) {
        return 0;
    }
    static const char hexdigits[] = "0123456789ABCDEF";
    size_t len = 0;
    const char *data = lua_tolstring(L, 1, &len);
    const size_t numBytes = len;
    const char *bytes = data;
    char *strbuf = (char *)malloc(numBytes * 2 + 1);
    if (!strbuf) {
        fprintf(stderr, "Memory allocation failed for data2hex: %zu bytes\n", numBytes * 2 + 1);
        luaL_error(L, "Memory allocation failure");
        return 0;
    }
    char *hex = strbuf;
    int i = 0;
    for (i = 0; i < numBytes; ++i) {
        const unsigned char c = *(bytes++);
        *hex++ = hexdigits[(c >> 4) & 0xF];
        *hex++ = hexdigits[(c)&0xF];
    }
    *hex = 0;

    lua_pushlstring(L, strbuf, numBytes * 2);

    free(strbuf);

    return 1;
}
// -- end hex2data data2hex --

LUA_API int luafan_gettime(lua_State *L) {
    struct timeval v;
    gettimeofday(&v, NULL);

    lua_pushinteger(L, v.tv_sec);
    lua_pushinteger(L, v.tv_usec);

    return 2;
}

LUA_API int luafan_fork(lua_State *L);
LUA_API int luafan_getpid(lua_State *L);
LUA_API int luafan_getdtablesize(lua_State *L);
LUA_API int luafan_setpgid(lua_State *L);
LUA_API int luafan_open(lua_State *L);
LUA_API int luafan_close(lua_State *L);
LUA_API int luafan_setsid(lua_State *L);
LUA_API int luafan_setprogname(lua_State *L);
LUA_API int luafan_getpgid(lua_State *L);

LUA_API int luafan_getaffinity(lua_State *L);
LUA_API int luafan_setaffinity(lua_State *L);
LUA_API int luafan_getcpucount(lua_State *L);
LUA_API int luafan_getinterfaces(lua_State *L);

LUA_API int luafan_kill(lua_State *L);
LUA_API int luafan_waitpid(lua_State *L);

LUA_API int luafan_gettop(lua_State *L) {
    lua_pushinteger(L, lua_gettop(utlua_mainthread(L)));
    return 1;
}

static const struct luaL_Reg fanlib[] = {
    {"loop", luafan_start},
    {"loopbreak", luafan_stop},

    {"sleep", luafan_sleep},
    {"gettime", luafan_gettime},
    {"gettop", luafan_gettop},

    {"data2hex", data2hex},
    {"hex2data", hex2data},

    {"fork", luafan_fork},
    {"getpid", luafan_getpid},
    {"waitpid", luafan_waitpid},
    {"kill", luafan_kill},
    {"setpgid", luafan_setpgid},
    {"getpgid", luafan_getpgid},
    {"setsid", luafan_setsid},
    {"getdtablesize", luafan_getdtablesize},
    {"open", luafan_open},
    {"close", luafan_close},
    {"setprogname", luafan_setprogname},
#ifndef DISABLE_AFFINIY
    {"setaffinity", luafan_setaffinity},
    {"getaffinity", luafan_getaffinity},
    {"getcpucount", luafan_getcpucount},
#endif
    {"getinterfaces", luafan_getinterfaces},

    {NULL, NULL},
};

LUA_API int luaopen_fan(lua_State *L) {

#if (LUA_VERSION_NUM < 502)
    utlua_set_mainthread(L);
#endif

#if DEBUG_THREAD_TRACKING
    // Initialize thread tracker
    thread_tracker_init();
    // Register Lua functions
    thread_tracker_register_lua_functions(L);
#endif

    lua_newtable(L);
    luaL_register(L, "fan", fanlib);
    return 1;
}
