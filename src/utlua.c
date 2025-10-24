#include "utlua.h"

int GLOBAL_VERBOSE = 0;

#if (LUA_VERSION_NUM < 502)
static int mainthread_ref = LUA_NOREF;

void utlua_set_mainthread(lua_State *L) {
    if (mainthread_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, mainthread_ref);
    }

    int ismain_thread = lua_pushthread(L);
    if (!ismain_thread) {
        printf("ismain_thread = false\n");
    }
    mainthread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

#endif

lua_State *utlua_mainthread(lua_State *L) {
    lua_lock(L);
#if (LUA_VERSION_NUM >= 502)
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
#else
    lua_rawgeti(L, LUA_REGISTRYINDEX, mainthread_ref);
#endif
    lua_State *mt = lua_tothread(L, -1);
    lua_pop(L, 1);
    lua_unlock(L);
    return mt;
}

int _utlua_resume(lua_State *co, lua_State *from, int count) {
    lua_lock(co);

#if (LUA_VERSION_NUM >= 504)
    int nresults;
    int status = lua_resume(co, from, count, &nresults);
    lua_pop(co, nresults);
#elif (LUA_VERSION_NUM >= 502)
    int status = lua_resume(co, from, count);
#else
#if (FAN_HAS_LUAJIT == 0)
    if (from) {
        lua_setlevel(from, co);
    } else {
        lua_setlevel(utlua_mainthread(co), co);
    }
#endif
    int status = lua_resume(co, count);
#endif

    // printf("resume status = %d\n", status);

    if (status > LUA_YIELD) {
        fprintf(stderr, "Error: %s\n", lua_tostring(co, -1));
    }

    lua_unlock(co);

    return status;
}

FAN_RESUME_TPYE FAN_RESUME = &_utlua_resume;

void utlua_set_resume(FAN_RESUME_TPYE resume) {
    FAN_RESUME = resume;
}

void d2tv(double x, struct timeval *tv) {
    tv->tv_sec = x;
    tv->tv_usec = (x - (double)tv->tv_sec) * 1000.0 * 1000.0 + 0.5;
}

int regress_get_socket_port(evutil_socket_t fd) {
    struct sockaddr_storage ss;
    ev_socklen_t socklen = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &socklen) != 0)
        return -1;
    if (ss.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    else if (ss.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    else
        return -1;
}

void regress_get_socket_host(evutil_socket_t fd, char *host) {
    struct sockaddr_storage ss;
    ev_socklen_t socklen = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &socklen) != 0)
        return;

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&ss;
        inet_ntop(addr_in->sin_family, (void *)&(addr_in->sin_addr), host, INET_ADDRSTRLEN);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)&ss;
        inet_ntop(addr_in->sin6_family, (void *)&(addr_in->sin6_addr), host, INET6_ADDRSTRLEN);
    }
}

#if FAN_HAS_OPENSSL

void die_most_horribly_from_openssl_error(const char *func) {
    fprintf(stderr, "%s failed:\n", func);
    ERR_print_errors_fp(stderr);
    exit(EXIT_FAILURE);
}

void server_setup_certs(SSL_CTX *ctx, const char *certificate_chain, const char *private_key) {
#if DEBUG
    printf("Loading certificate chain from '%s'\n"
           "and private key from '%s'\n",
           certificate_chain, private_key);
#endif

    if (1 != SSL_CTX_use_certificate_chain_file(ctx, certificate_chain)) {
        die_most_horribly_from_openssl_error("SSL_CTX_use_certificate_chain_file");
    }

    if (1 != SSL_CTX_use_PrivateKey_file(ctx, private_key, SSL_FILETYPE_PEM))
        die_most_horribly_from_openssl_error("SSL_CTX_use_PrivateKey_file");

    if (1 != SSL_CTX_check_private_key(ctx))
        die_most_horribly_from_openssl_error("SSL_CTX_check_private_key");
}
#endif

// Shared weak table functions for TCP/UDP connection self-references
void utlua_store_self_in_weak_table(lua_State *L, void *conn_ptr, int self_index) {
    // Get or create weak table for connections (shared by TCP and UDP)
    lua_pushliteral(L, "LUAFAN_WEAK_REFS");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        // Create weak table
        lua_newtable(L);
        // Set weak metatable
        lua_newtable(L);
        lua_pushliteral(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        // Store in registry
        lua_pushliteral(L, "LUAFAN_WEAK_REFS");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    // Store connection in weak table with pointer as key
    lua_pushlightuserdata(L, conn_ptr);
    lua_pushvalue(L, self_index);
    lua_rawset(L, -3);
    lua_pop(L, 1); // pop weak table
}

void utlua_push_self_from_weak_table(lua_State *L, void *conn_ptr) {
    // Try to get connection from weak table using conn pointer as key
    lua_pushliteral(L, "LUAFAN_WEAK_REFS");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1)) {
        // Get connection from weak table using pointer as key
        lua_pushlightuserdata(L, conn_ptr);
        lua_rawget(L, -2);
        lua_remove(L, -2); // remove weak table
        if (!lua_isnil(L, -1)) {
            return; // Successfully retrieved connection
        }
        lua_pop(L, 1); // pop nil result
    } else {
        lua_pop(L, 1); // pop nil weak table
    }

    // If lookup failed or no weak table, push nil
    lua_pushnil(L);
}
