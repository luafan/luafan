#include "tcpd_common.h"
#include "tcpd_ssl.h"
#include "tcpd_server.h"
#include <net/if.h>

#ifdef __linux__
#include <limits.h>
#include <linux/netfilter_ipv4.h>
#endif

#define LUA_TCPD_CONNECTION_TYPE "<tcpd.connect>"

// Forward declarations
static void tcpd_client_cleanup_on_disconnect(tcpd_client_conn_t *client);

// Override the base cleanup function for client connections
void tcpd_connection_cleanup_on_disconnect(tcpd_base_conn_t *conn) {
    if (!conn) return;

    switch (conn->type) {
        case TCPD_CONN_TYPE_CLIENT: {
            tcpd_client_conn_t *client = (tcpd_client_conn_t *)conn;
            tcpd_client_cleanup_on_disconnect(client);
            break;
        }
        default:
            // Basic cleanup
            if (conn->mainthread) {
                CLEAR_REF(conn->mainthread, conn->onReadRef);
                CLEAR_REF(conn->mainthread, conn->onSendReadyRef);
            }
            break;
    }
}


// Client connect function
LUA_API int tcpd_connect(lua_State *L) {
    event_mgr_init();
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    tcpd_client_conn_t *client = lua_newuserdata(L, sizeof(tcpd_client_conn_t));
    memset(client, 0, sizeof(tcpd_client_conn_t));
    luaL_getmetatable(L, LUA_TCPD_CONNECTION_TYPE);
    lua_setmetatable(L, -2);

    // Initialize base connection
    tcpd_base_conn_init(&client->base, TCPD_CONN_TYPE_CLIENT, utlua_mainthread(L));

    // Extract configuration
    tcpd_config_from_lua_table(L, 1, &client->base.config);

    // Set callbacks (including onConnectedRef)
    tcpd_base_conn_set_callbacks(&client->base, L, 1);

    // Extract host and port
    DUP_STR_FROM_TABLE(L, client->base.host, 1, "host");
    SET_INT_FROM_TABLE(L, client->base.port, 1, "port");

    // Set up SSL if enabled
    if (client->base.config.ssl_enabled) {
#if FAN_HAS_OPENSSL
        tcpd_ssl_init();

        // Generate cache key based on SSL configuration
        char *cache_key = tcpd_ssl_generate_cache_key_from_table(L, 1);
        if (cache_key) {
            client->base.ssl_ctx = tcpd_ssl_context_get_or_create(L, cache_key);
            if (client->base.ssl_ctx) {
                tcpd_ssl_context_configure(client->base.ssl_ctx, L, 1);

                // Get SSL hostname override
                lua_getfield(L, 1, "ssl_host");
                const char *ssl_host = lua_tostring(L, -1);
                lua_pop(L, 1);

                client->base.buf = tcpd_ssl_create_client_bufferevent(
                    event_mgr_base(), client->base.ssl_ctx,
                    ssl_host ? ssl_host : client->base.host, client);
            }
            free(cache_key);
        }
#else
        luaL_error(L, "SSL is not supported in this build");
#endif
    } else {
        client->base.buf = bufferevent_socket_new(
            event_mgr_base(), -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    }

    if (!client->base.buf) {
        return 1;
    }

    // Connect to host
    int rc = bufferevent_socket_connect_hostname(
        client->base.buf, event_mgr_dnsbase(), AF_UNSPEC,
        client->base.host, client->base.port);

    if (rc < 0) {
        LOGE("could not connect to %s:%d %s", client->base.host, client->base.port,
             evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
        tcpd_shutdown_bufferevent(client->base.buf);
        client->base.buf = NULL;
        return 1;
    }

    // Apply configuration
    evutil_socket_t fd = bufferevent_getfd(client->base.buf);
    tcpd_config_apply_buffers(&client->base.config, client->base.buf, fd);
    tcpd_config_apply_keepalive(&client->base.config, fd);
    tcpd_config_apply_interface(&client->base.config, fd);

    client->base.state = TCPD_CONN_CONNECTING;

    // Set callbacks and enable events
    bufferevent_setcb(client->base.buf, tcpd_common_readcb, tcpd_common_writecb, tcpd_common_eventcb, &client->base);
    bufferevent_enable(client->base.buf, EV_WRITE | EV_READ);

    // If callback_self_first is enabled, store a self-reference to avoid circular references
    if (client->base.config.callback_self_first) {
        lua_pushvalue(L, -1);  // Push the client connection object
        client->base.selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    return 1;
}

// Client cleanup
static void tcpd_client_cleanup_on_disconnect(tcpd_client_conn_t *client) {
    if (!client) return;

#if FAN_HAS_OPENSSL
    // Clean up SSL fields
    free(client->ssl_host);
    free(client->ssl_error_message);
    client->ssl_host = NULL;
    client->ssl_error_message = NULL;
    // SSL object is freed by bufferevent
    client->ssl = NULL;

    // Release SSL context with retain count management
    if (client->base.ssl_ctx) {
        tcpd_ssl_context_release(client->base.ssl_ctx, client->base.mainthread);
        client->base.ssl_ctx = NULL;
    }
#endif
    // Base cleanup handles all references including onConnectedRef
    tcpd_base_conn_cleanup(&client->base);
}

// Connection send function
LUA_API int tcpd_conn_send(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    if (data && len > 0 && client->base.buf) {
        tcpd_config_apply_timeouts(&client->base.config, client->base.buf);
        bufferevent_write(client->base.buf, data, len);

        size_t total = evbuffer_get_length(bufferevent_get_output(client->base.buf));
        lua_pushinteger(L, total);
    } else {
        lua_pushinteger(L, -1);
    }

    return 1;
}

// Connection close function
LUA_API int tcpd_conn_close(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);

    // Perform complete cleanup including Lua registry references
    tcpd_client_cleanup_on_disconnect(client);

    return 0;
}

// Connection read pause function
LUA_API int tcpd_conn_read_pause(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);

    if (client->base.buf) {
        bufferevent_disable(client->base.buf, EV_READ);
    }

    return 0;
}

// Connection read resume function
LUA_API int tcpd_conn_read_resume(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);

    if (client->base.buf) {
        bufferevent_enable(client->base.buf, EV_READ);
    }

    return 0;
}

// Get socket name function
LUA_API int tcpd_conn_getsockname(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);

    if (client->base.buf) {
        evutil_socket_t fd = bufferevent_getfd(client->base.buf);
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

// Get peer name function
LUA_API int tcpd_conn_getpeername(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);

    if (client->base.buf) {
        evutil_socket_t fd = bufferevent_getfd(client->base.buf);
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


// Garbage collection functions for preventing Lua registry leaks

// Client connection garbage collection
static int tcpd_client_conn_gc(lua_State *L) {
    tcpd_client_conn_t *client = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);

    // Perform base connection cleanup (handles all references)
    tcpd_base_conn_cleanup(&client->base);

    return 0;
}


// Module initialization
static const luaL_Reg tcpdlib[] = {
    {"connect", tcpd_connect},
    {NULL, NULL}
};

LUA_API int luaopen_fan_tcpd(lua_State *L) {
#if FAN_HAS_OPENSSL
    tcpd_ssl_init();
    tcpd_ssl_register_metatable(L);
#endif

    // Register CONNECTION_TYPE metatable
    luaL_newmetatable(L, LUA_TCPD_CONNECTION_TYPE);
    lua_pushcfunction(L, tcpd_conn_send);
    lua_setfield(L, -2, "send");
    lua_pushcfunction(L, tcpd_conn_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, tcpd_conn_read_pause);
    lua_setfield(L, -2, "pause_read");
    lua_pushcfunction(L, tcpd_conn_read_resume);
    lua_setfield(L, -2, "resume_read");
    lua_pushcfunction(L, tcpd_conn_getsockname);
    lua_setfield(L, -2, "getsockname");
    lua_pushcfunction(L, tcpd_conn_getpeername);
    lua_setfield(L, -2, "getpeername");
    lua_pushcfunction(L, tcpd_client_conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushstring(L, "connection");
    lua_setfield(L, -2, "__typename");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Register server and accept metatables
    tcpd_server_register_metatables(L);

    // Create the main tcpd module table
    lua_newtable(L);
    luaL_register(L, NULL, tcpdlib);

    // Add bind function from tcpd_server.c
    lua_pushcfunction(L, tcpd_bind);
    lua_setfield(L, -2, "bind");

    return 1;
}
