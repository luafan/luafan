#include "tcpd_server.h"
#include "tcpd_ssl.h"
#include <net/if.h>

#ifdef __linux__
#include <limits.h>
#include <linux/netfilter_ipv4.h>
#endif

#define LUA_TCPD_SERVER_TYPE "<tcpd.bind %s %d>"
#define LUA_TCPD_ACCEPT_TYPE "<tcpd.accept %s %d>"

// Extended structures using the base connection
typedef struct {
    tcpd_base_conn_t base;
    int selfRef;  // Self-reference for accept connections
} tcpd_accept_conn_t;

// Forward declarations
static void tcpd_accept_cleanup_on_disconnect(tcpd_accept_conn_t *accept);

// Server connection listener callback
void tcpd_server_listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                            struct sockaddr *addr, int socklen, void *arg) {
    tcpd_server_t *server = (tcpd_server_t *)arg;

    if (server->onAcceptRef == LUA_NOREF) return;

    lua_State *mainthread = server->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, server->onAcceptRef);

    // Create accept connection object
    tcpd_accept_conn_t *accept = lua_newuserdata(co, sizeof(tcpd_accept_conn_t));
    memset(accept, 0, sizeof(tcpd_accept_conn_t));

    // Initialize base connection
    tcpd_base_conn_init(&accept->base, TCPD_CONN_TYPE_ACCEPT, mainthread);
    accept->base.config = server->config;  // Copy server config
    accept->selfRef = LUA_NOREF;

    luaL_getmetatable(co, LUA_TCPD_ACCEPT_TYPE);
    lua_setmetatable(co, -2);

    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev;

    // Create bufferevent (SSL or regular)
    if (server->ssl_ctx) {
        bev = tcpd_ssl_create_server_bufferevent(base, fd, server->ssl_ctx);
    } else {
        bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    }

    if (!bev) {
        // Handle error
        lua_pushnil(co);
        FAN_RESUME(co, mainthread, 1);
        POP_REF(mainthread);
        return;
    }

    accept->base.buf = bev;
    accept->base.state = TCPD_CONN_CONNECTED;

    // Set callbacks
    bufferevent_setcb(bev, tcpd_common_readcb, tcpd_common_writecb, tcpd_common_eventcb, &accept->base);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    // Apply configuration
    tcpd_config_apply_buffers(&server->config, bev, fd);
    tcpd_config_apply_keepalive(&server->config, fd);

    // Extract client address
    memset(accept->base.ip, 0, INET6_ADDRSTRLEN);
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
        inet_ntop(addr_in->sin_family, (void *)&(addr_in->sin_addr), accept->base.ip, INET_ADDRSTRLEN);
        accept->base.port = ntohs(addr_in->sin_port);
    } else {
        struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)addr;
        inet_ntop(addr_in->sin6_family, (void *)&(addr_in->sin6_addr), accept->base.ip, INET6_ADDRSTRLEN);
        accept->base.port = ntohs(addr_in->sin6_port);
    }

    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);
}

// Server rebind function implementation
void tcpd_server_rebind(lua_State *L, tcpd_server_t *server) {
    if (!server) return;

    if (server->listener) {
        evconnlistener_free(server->listener);
        server->listener = NULL;
    }

    if (server->host) {
        char portbuf[6];
        evutil_snprintf(portbuf, sizeof(portbuf), "%d", server->port);

        struct evutil_addrinfo hints = {0};
        struct evutil_addrinfo *answer = NULL;
        hints.ai_family = server->ipv6 ? AF_INET6 : AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

        int err = evutil_getaddrinfo(server->host, portbuf, &hints, &answer);
        if (err < 0 || !answer) {
            if (L) {
                luaL_error(L, "invalid bind address %s:%d", server->host, server->port);
            }
            return;
        }

        server->listener = evconnlistener_new_bind(
            event_mgr_base(), tcpd_server_listener_cb, server,
            LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
            -1, answer->ai_addr, answer->ai_addrlen);
        evutil_freeaddrinfo(answer);
    } else {
        struct sockaddr *addr = NULL;
        size_t addr_size = 0;
        struct sockaddr_in sin;
        struct sockaddr_in6 sin6;

        memset(&sin, 0, sizeof(sin));
        memset(&sin6, 0, sizeof(sin6));

        if (!server->ipv6) {
            addr = (struct sockaddr *)&sin;
            addr_size = sizeof(sin);
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = htonl(0);
            sin.sin_port = htons(server->port);
        } else {
            addr = (struct sockaddr *)&sin6;
            addr_size = sizeof(sin6);
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons(server->port);
        }

        server->listener = evconnlistener_new_bind(
            event_mgr_base(), tcpd_server_listener_cb, server,
            LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
            -1, addr, (int)addr_size);
    }
}

// Lua API wrapper for server rebind
LUA_API int lua_tcpd_server_rebind(lua_State *L) {
    tcpd_server_t *server = luaL_checkudata(L, 1, LUA_TCPD_SERVER_TYPE);
    tcpd_server_rebind(L, server);
    return 0;
}

// Server bind function
LUA_API int tcpd_bind(lua_State *L) {
    event_mgr_init();
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    tcpd_server_t *server = lua_newuserdata(L, sizeof(tcpd_server_t));
    memset(server, 0, sizeof(tcpd_server_t));
    luaL_getmetatable(L, LUA_TCPD_SERVER_TYPE);
    lua_setmetatable(L, -2);

    server->mainthread = utlua_mainthread(L);

    // Set callbacks
    SET_FUNC_REF_FROM_TABLE(L, server->onAcceptRef, 1, "onaccept");
    SET_FUNC_REF_FROM_TABLE(L, server->onSSLHostNameRef, 1, "onsslhostname");

    // Extract host and port
    DUP_STR_FROM_TABLE(L, server->host, 1, "host");
    SET_INT_FROM_TABLE(L, server->port, 1, "port");

    // Extract configuration
    tcpd_config_from_lua_table(L, 1, &server->config);

    // Set up SSL if enabled
    if (server->config.ssl_enabled) {
#if FAN_HAS_OPENSSL
        server->ssl_ctx = tcpd_ssl_context_create(L);
        if (server->ssl_ctx) {
            tcpd_ssl_context_configure(server->ssl_ctx, L, 1);
        }
#else
        luaL_error(L, "SSL is not supported in this build");
#endif
    }

    lua_getfield(L, 1, "ipv6");
    server->ipv6 = lua_toboolean(L, -1);
    lua_pop(L, 1);

    // Create listener
    tcpd_server_rebind(L, server);

    if (!server->listener) {
        return 0;
    }

    if (!server->port) {
        server->port = regress_get_socket_port(evconnlistener_get_fd(server->listener));
    }

    lua_pushinteger(L, server->port);
    return 2;
}

// Accept connection bind function
LUA_API int tcpd_accept_bind(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);

    lua_pushvalue(L, 1);
    accept->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // Set callbacks using common function
    tcpd_base_conn_set_callbacks(&accept->base, L, 2);

    lua_pushstring(L, accept->base.ip);
    lua_pushinteger(L, accept->base.port);

    return 2;
}

// Accept connection cleanup
static void tcpd_accept_cleanup_on_disconnect(tcpd_accept_conn_t *accept) {
    if (!accept) return;

    CLEAR_REF(accept->base.mainthread, accept->selfRef);
    tcpd_base_conn_cleanup(&accept->base);
}

// Accept send function
LUA_API int tcpd_accept_send(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    if (data && len > 0 && accept->base.buf) {
        bufferevent_write(accept->base.buf, data, len);
        size_t total = evbuffer_get_length(bufferevent_get_output(accept->base.buf));
        lua_pushinteger(L, total);
    } else {
        lua_pushinteger(L, -1);
    }

    return 1;
}

// Accept connection close function
LUA_API int tcpd_accept_close(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    // Perform complete cleanup including Lua registry references
    tcpd_accept_cleanup_on_disconnect(accept);

    return 0;
}

// Accept connection read pause function
LUA_API int tcpd_accept_read_pause(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    if (accept->base.buf) {
        bufferevent_disable(accept->base.buf, EV_READ);
    }

    return 0;
}

// Accept connection read resume function
LUA_API int tcpd_accept_read_resume(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    if (accept->base.buf) {
        bufferevent_enable(accept->base.buf, EV_READ);
    }

    return 0;
}

// Accept get socket name function
LUA_API int tcpd_accept_getsockname(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    if (accept->base.buf) {
        evutil_socket_t fd = bufferevent_getfd(accept->base.buf);
        if (fd >= 0) {
            struct sockaddr_storage ss;
            socklen_t len = sizeof(ss);

            if (getsockname(fd, (struct sockaddr*)&ss, &len) == 0) {
                char ip[INET6_ADDRSTRLEN];
                int port = 0;

                if (ss.ss_family == AF_INET) {
                    struct sockaddr_in *addr = (struct sockaddr_in*)&ss;
                    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                    port = ntohs(addr->sin_port);
                } else if (ss.ss_family == AF_INET6) {
                    struct sockaddr_in6 *addr = (struct sockaddr_in6*)&ss;
                    inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));
                    port = ntohs(addr->sin6_port);
                }

                lua_pushstring(L, ip);
                lua_pushinteger(L, port);
                return 2;
            }
        }
    }

    lua_pushnil(L);
    lua_pushnil(L);
    return 2;
}

// Accept get peer name function
LUA_API int tcpd_accept_getpeername(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    if (accept->base.buf) {
        evutil_socket_t fd = bufferevent_getfd(accept->base.buf);
        if (fd >= 0) {
            struct sockaddr_storage ss;
            socklen_t len = sizeof(ss);

            if (getpeername(fd, (struct sockaddr*)&ss, &len) == 0) {
                char ip[INET6_ADDRSTRLEN];
                int port = 0;

                if (ss.ss_family == AF_INET) {
                    struct sockaddr_in *addr = (struct sockaddr_in*)&ss;
                    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                    port = ntohs(addr->sin_port);
                } else if (ss.ss_family == AF_INET6) {
                    struct sockaddr_in6 *addr = (struct sockaddr_in6*)&ss;
                    inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));
                    port = ntohs(addr->sin6_port);
                }

                lua_pushstring(L, ip);
                lua_pushinteger(L, port);
                return 2;
            }
        }
    }

    lua_pushnil(L);
    lua_pushnil(L);
    return 2;
}

// Server garbage collection
static int tcpd_server_gc(lua_State *L) {
    tcpd_server_t *server = luaL_checkudata(L, 1, LUA_TCPD_SERVER_TYPE);

    // Clear Lua registry callback references
    if (server->mainthread) {
        CLEAR_REF(server->mainthread, server->onAcceptRef);
        CLEAR_REF(server->mainthread, server->onSSLHostNameRef);
    }

    // Clean up listener
    if (server->listener) {
        evconnlistener_free(server->listener);
        server->listener = NULL;
    }

    // Clean up host string
    if (server->host) {
        free(server->host);
        server->host = NULL;
    }

    // Clean up SSL context
    if (server->ssl_ctx) {
        tcpd_ssl_context_release(server->ssl_ctx, server->mainthread);
        server->ssl_ctx = NULL;
    }

    return 0;
}

// Accept connection garbage collection
static int tcpd_accept_conn_gc(lua_State *L) {
    tcpd_accept_conn_t *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

    // Clear the self-reference
    if (accept->base.mainthread) {
        CLEAR_REF(accept->base.mainthread, accept->selfRef);
    }

    // Perform base connection cleanup
    tcpd_base_conn_cleanup(&accept->base);

    return 0;
}

// Register server and accept metatables
void tcpd_server_register_metatables(lua_State *L) {
    // Register ACCEPT_TYPE metatable
    luaL_newmetatable(L, LUA_TCPD_ACCEPT_TYPE);
    lua_pushcfunction(L, tcpd_accept_send);
    lua_setfield(L, -2, "send");
    lua_pushcfunction(L, tcpd_accept_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, tcpd_accept_read_pause);
    lua_setfield(L, -2, "pause_read");
    lua_pushcfunction(L, tcpd_accept_read_resume);
    lua_setfield(L, -2, "resume_read");
    lua_pushcfunction(L, tcpd_accept_getsockname);
    lua_setfield(L, -2, "getsockname");
    lua_pushcfunction(L, tcpd_accept_getpeername);
    lua_setfield(L, -2, "getpeername");
    lua_pushcfunction(L, tcpd_accept_bind);
    lua_setfield(L, -2, "bind");
    lua_pushcfunction(L, tcpd_accept_conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushstring(L, "accept");
    lua_setfield(L, -2, "__typename");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Register SERVER_TYPE metatable
    luaL_newmetatable(L, LUA_TCPD_SERVER_TYPE);
    lua_pushcfunction(L, lua_tcpd_server_rebind);
    lua_setfield(L, -2, "rebind");
    lua_pushcfunction(L, tcpd_server_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushstring(L, "server");
    lua_setfield(L, -2, "__typename");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}