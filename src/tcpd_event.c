#include "tcpd_common.h"
#include "tcpd_ssl.h"  // For tcpd_ssl_context_release function
#include <event2/buffer.h>
#include <string.h>

#define BUFLEN 1024

// Forward declarations
static tcpd_error_t tcpd_analyze_event_error(struct bufferevent *bev, short events);
static void tcpd_call_lua_callback(lua_State *mainthread, int callback_ref, int argc);

// Helper function to push connection object to Lua stack from weak table
void tcpd_push_connection_object(lua_State *co, tcpd_base_conn_t *conn) {
    if (!co || !conn) {
        lua_pushnil(co);
        return;
    }

    // Use shared weak table function
    utlua_push_self_from_weak_table(co, conn);
}

// Common read callback for all connection types
void tcpd_common_readcb(struct bufferevent *bev, void *ctx) {
    tcpd_base_conn_t *conn = (tcpd_base_conn_t *)ctx;

    if (!conn || !conn->buf || conn->onReadRef == LUA_NOREF) {
        return;
    }

    char buf[BUFLEN];
    int n;
    BYTEARRAY ba = {0};
    bytearray_alloc(&ba, BUFLEN * 2);

    struct evbuffer *input = bufferevent_get_input(bev);
    while ((n = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
        bytearray_writebuffer(&ba, buf, n);
    }
    bytearray_read_ready(&ba);

    lua_State *mainthread = conn->mainthread;
    if (!mainthread) {
        bytearray_dealloc(&ba);
        return;
    }
    lua_lock(mainthread);
    // Recheck ref under lock — may have been cleared by another thread
    if (conn->onReadRef == LUA_NOREF) {
        lua_unlock(mainthread);
        bytearray_dealloc(&ba);
        return;
    }
    fan_cb_setup_t cbs = fan_cb_setup(mainthread, conn->onReadRef);
    if (!cbs.co) {
        lua_unlock(mainthread);
        bytearray_dealloc(&ba);
        return;
    }

    // Guard: verify the registry slot still holds a function
    if (!lua_isfunction(cbs.co, -1)) {
        LOGE("tcpd_common_readcb: onReadRef=%d resolved to %s, expected function\n",
             conn->onReadRef, luaL_typename(cbs.co, -1));
        lua_pop(cbs.co, 1);
        lua_unlock(mainthread);
        FAN_CB_CLEANUP(mainthread, cbs);
        bytearray_dealloc(&ba);
        return;
    }

    // Check if callback_self_first is enabled
    int argc = 1;
    if (conn->config.callback_self_first) {
        tcpd_push_connection_object(cbs.co, conn);
        lua_pushlstring(cbs.co, (const char *)ba.buffer, ba.total);
        argc = 2;
    } else {
        lua_pushlstring(cbs.co, (const char *)ba.buffer, ba.total);
        argc = 1;
    }

    lua_unlock(mainthread);
    FAN_RESUME(cbs.co, mainthread, argc);
    FAN_CB_CLEANUP(mainthread, cbs);

    bytearray_dealloc(&ba);
}

// Common write callback for all connection types
void tcpd_common_writecb(struct bufferevent *bev, void *ctx) {
    tcpd_base_conn_t *conn = (tcpd_base_conn_t *)ctx;

    if (!conn || !conn->buf || conn->onSendReadyRef == LUA_NOREF) {
        return;
    }

    // Only call the callback when output buffer is empty
    if (evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        lua_State *mainthread = conn->mainthread;
        if (!mainthread) return;
        lua_lock(mainthread);
        // Recheck ref under lock — may have been cleared by another thread
        if (conn->onSendReadyRef == LUA_NOREF) {
            lua_unlock(mainthread);
            return;
        }
        fan_cb_setup_t cbs = fan_cb_setup(mainthread, conn->onSendReadyRef);
        if (!cbs.co) {
            lua_unlock(mainthread);
            return;
        }

        // Guard: verify the registry slot still holds a function
        if (!lua_isfunction(cbs.co, -1)) {
            LOGE("tcpd_common_writecb: onSendReadyRef=%d resolved to %s, expected function\n",
                 conn->onSendReadyRef, luaL_typename(cbs.co, -1));
            lua_pop(cbs.co, 1);
            lua_unlock(mainthread);
            FAN_CB_CLEANUP(mainthread, cbs);
            return;
        }

        // Check if callback_self_first is enabled
        int argc = 0;
        if (conn->config.callback_self_first) {
            tcpd_push_connection_object(cbs.co, conn);
            argc = 1;
        }

        lua_unlock(mainthread);
        FAN_RESUME(cbs.co, mainthread, argc);
        FAN_CB_CLEANUP(mainthread, cbs);
    }
}

// Common event callback for all connection types
void tcpd_common_eventcb(struct bufferevent *bev, short events, void *ctx) {
    tcpd_base_conn_t *conn = (tcpd_base_conn_t *)ctx;

    if (!conn) return;

    // Handle connection established event (only for client connections)
    if (events & BEV_EVENT_CONNECTED && conn->type == TCPD_CONN_TYPE_CLIENT) {
        conn->state = TCPD_CONN_CONNECTED;

        if (conn->onConnectedRef != LUA_NOREF) {
            lua_State *mainthread = conn->mainthread;
            if (!mainthread) return;
            lua_lock(mainthread);
            // Recheck ref under lock — may have been cleared by another thread
            if (conn->onConnectedRef == LUA_NOREF) {
                lua_unlock(mainthread);
                return;
            }
            fan_cb_setup_t cbs = fan_cb_setup(mainthread, conn->onConnectedRef);
            if (!cbs.co) {
                lua_unlock(mainthread);
                return;
            }

            // Guard: verify the registry slot still holds a function
            if (!lua_isfunction(cbs.co, -1)) {
                LOGE("tcpd_common_eventcb(CONNECTED): onConnectedRef=%d resolved to %s, expected function\n",
                     conn->onConnectedRef, luaL_typename(cbs.co, -1));
                lua_pop(cbs.co, 1);
                lua_unlock(mainthread);
                FAN_CB_CLEANUP(mainthread, cbs);
                return;
            }

            // Check if callback_self_first is enabled
            int argc = 0;
            if (conn->config.callback_self_first) {
                tcpd_push_connection_object(cbs.co, conn);
                argc = 1;
            }

            lua_unlock(mainthread);
            FAN_RESUME(cbs.co, mainthread, argc);
            FAN_CB_CLEANUP(mainthread, cbs);
        }
        return;
    }

    // Handle disconnection events
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_TIMEOUT)) {
        // Critical: When receiving EOF, we must read all remaining data from input buffer BEFORE cleanup
        // The peer has sent FIN, but there may still be data in our receive buffer
        if ((events & BEV_EVENT_EOF) && conn->buf && conn->onReadRef != LUA_NOREF) {
            struct evbuffer *input = bufferevent_get_input(conn->buf);
            size_t pending = evbuffer_get_length(input);

            if (pending > 0) {
                // Read all remaining data from buffer
                char buf[BUFLEN];
                int n;
                BYTEARRAY ba = {0};
                bytearray_alloc(&ba, pending + BUFLEN);

                while ((n = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
                    bytearray_writebuffer(&ba, buf, n);
                }
                bytearray_read_ready(&ba);

                // Call onRead callback with remaining data
                lua_State *mainthread = conn->mainthread;
                if (!mainthread) {
                    bytearray_dealloc(&ba);
                    goto after_eof_read;
                }
                lua_lock(mainthread);
                // Recheck ref under lock — may have been cleared by another thread
                if (conn->onReadRef == LUA_NOREF) {
                    lua_unlock(mainthread);
                    bytearray_dealloc(&ba);
                    goto after_eof_read;
                }
                fan_cb_setup_t cbs = fan_cb_setup(mainthread, conn->onReadRef);
                if (!cbs.co) {
                    lua_unlock(mainthread);
                    bytearray_dealloc(&ba);
                    goto after_eof_read;
                }

                // Guard: verify the registry slot still holds a function
                if (!lua_isfunction(cbs.co, -1)) {
                    LOGE("tcpd_common_eventcb(EOF read): onReadRef=%d resolved to %s, expected function\n",
                         conn->onReadRef, luaL_typename(cbs.co, -1));
                    lua_pop(cbs.co, 1);
                    lua_unlock(mainthread);
                    FAN_CB_CLEANUP(mainthread, cbs);
                    bytearray_dealloc(&ba);
                    goto after_eof_read;
                }

                int argc = 1;
                if (conn->config.callback_self_first) {
                    tcpd_push_connection_object(cbs.co, conn);
                    lua_pushlstring(cbs.co, (const char *)ba.buffer, ba.total);
                    argc = 2;
                } else {
                    lua_pushlstring(cbs.co, (const char *)ba.buffer, ba.total);
                    argc = 1;
                }

                lua_unlock(mainthread);
                FAN_RESUME(cbs.co, mainthread, argc);
                FAN_CB_CLEANUP(mainthread, cbs);

                bytearray_dealloc(&ba);
            }
        }
after_eof_read:

        // Update connection state
        if (events & BEV_EVENT_ERROR) {
            conn->state = TCPD_CONN_ERROR;
        } else {
            conn->state = TCPD_CONN_DISCONNECTED;
        }

        // Analyze the error
        tcpd_error_t error = tcpd_analyze_event_error(bev, events);

        // Debug output
#if DEBUG
        if (events & BEV_EVENT_ERROR) {
            printf("BEV_EVENT_ERROR: %s\n", tcpd_error_to_string(&error));
        }
#endif

        // Clean up the bufferevent
        if (conn->buf) {
            tcpd_shutdown_bufferevent(conn->buf);
            conn->buf = NULL;
        }

        // Call the disconnected callback if set
        if (conn->onDisconnectedRef != LUA_NOREF) {
            lua_State *mainthread = conn->mainthread;
            if (!mainthread) goto after_disconnect_cb;
            lua_lock(mainthread);
            // Recheck ref under lock — may have been cleared by another thread
            if (conn->onDisconnectedRef == LUA_NOREF) {
                lua_unlock(mainthread);
                goto after_disconnect_cb;
            }
            fan_cb_setup_t cbs = fan_cb_setup(mainthread, conn->onDisconnectedRef);
            if (!cbs.co) {
                lua_unlock(mainthread);
                goto after_disconnect_cb;
            }

            // Guard: verify the registry slot still holds a function
            if (!lua_isfunction(cbs.co, -1)) {
                LOGE("tcpd_common_eventcb(DISCONNECT): onDisconnectedRef=%d resolved to %s, expected function\n",
                     conn->onDisconnectedRef, luaL_typename(cbs.co, -1));
                lua_pop(cbs.co, 1);
                lua_unlock(mainthread);
                FAN_CB_CLEANUP(mainthread, cbs);
                goto after_disconnect_cb;
            }

            // Check if callback_self_first is enabled
            int argc = 1;
            if (conn->config.callback_self_first) {
                tcpd_push_connection_object(cbs.co, conn);
                argc = 2;
            }

            // Push error message
            const char *error_msg = tcpd_error_to_string(&error);
            if (error_msg) {
                lua_pushstring(cbs.co, error_msg);
            } else {
                lua_pushnil(cbs.co);
            }

            // Clear the callback reference
            CLEAR_REF(mainthread, conn->onDisconnectedRef);

            lua_unlock(mainthread);
            FAN_RESUME(cbs.co, mainthread, argc);
            FAN_CB_CLEANUP(mainthread, cbs);
        }
after_disconnect_cb:

        // Clean up error
        tcpd_error_cleanup(&error);

        // Perform type-specific cleanup
        tcpd_connection_cleanup_on_disconnect(conn);
    }
}

// Analyze bufferevent error and create appropriate error structure
static tcpd_error_t tcpd_analyze_event_error(struct bufferevent *bev, short events) {
    tcpd_error_t error = {0};

    if (events & BEV_EVENT_TIMEOUT) {
        error.type = TCPD_ERROR_TIMEOUT;
        if (events & BEV_EVENT_READING) {
            error.message = strdup("read timeout");
        } else if (events & BEV_EVENT_WRITING) {
            error.message = strdup("write timeout");
        } else {
            error.message = strdup("unknown timeout");
        }
    } else if (events & BEV_EVENT_ERROR) {
        // Check for DNS errors first
        int dns_err = bufferevent_socket_get_dns_error(bev);
        if (dns_err) {
            error.type = TCPD_ERROR_DNS_FAILED;
            error.message = strdup(evutil_gai_strerror(dns_err));
            error.system_error = dns_err;
        } else if (EVUTIL_SOCKET_ERROR()) {
            // Socket error
            error.type = TCPD_ERROR_CONNECTION_RESET;
            error.message = strdup(evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
            error.system_error = EVUTIL_SOCKET_ERROR();
#if FAN_HAS_OPENSSL
        } else if (ERR_peek_error()) {
            // SSL error (no socket error, but OpenSSL error queue has entries)
            error.type = TCPD_ERROR_SSL_ERROR;
            char *ssl_err = tcpd_ssl_get_error_string();
            error.message = ssl_err ? ssl_err : strdup("SSL error");
#endif
        } else {
            error.type = TCPD_ERROR_UNKNOWN;
            error.message = strdup("unknown error");
        }
    } else if (events & BEV_EVENT_EOF) {
        error.type = TCPD_ERROR_EOF;
        // We can't access conn here since it's not passed to this function
        // The error message will be determined by the caller based on connection type
        error.message = strdup("connection closed");
    } else {
        error.type = TCPD_ERROR_UNKNOWN;
        error.message = strdup("unknown disconnection");
    }

    return error;
}

// Shutdown bufferevent safely (handles SSL)
void tcpd_shutdown_bufferevent(struct bufferevent *bev) {
    if (!bev) return;

    // Clear callbacks before freeing to prevent deferred callbacks from firing
    // after the owning connection struct has been freed (use-after-free).
    bufferevent_setcb(bev, NULL, NULL, NULL, NULL);
    bufferevent_disable(bev, EV_READ | EV_WRITE);

#if FAN_HAS_OPENSSL && OPENSSL_VERSION_NUMBER < 0x1010000fL
    SSL *ssl = bufferevent_openssl_get_ssl(bev);
    if (ssl) {
        SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
        if (SSL_shutdown(ssl) == 0) {
            SSL_shutdown(ssl);
        }
        // Mark as fully shut down so bufferevent_free won't attempt again
        SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
    }
#endif
    bufferevent_free(bev);
}

// Type-specific cleanup function (implemented in tcpd_refactored.c)
// This function is implemented in the refactored module to handle different connection types

// Initialize callbacks from Lua table
int tcpd_base_conn_set_callbacks(tcpd_base_conn_t *conn, lua_State *L, int table_index) {
    if (!conn || !L) return -1;

    // Initialize callback references
    conn->onReadRef = LUA_NOREF;
    conn->onSendReadyRef = LUA_NOREF;
    conn->onDisconnectedRef = LUA_NOREF;
    conn->onConnectedRef = LUA_NOREF;

    // Set callbacks from table
    SET_FUNC_REF_FROM_TABLE(L, conn->onReadRef, table_index, "onread");
    SET_FUNC_REF_FROM_TABLE(L, conn->onSendReadyRef, table_index, "onsendready");
    SET_FUNC_REF_FROM_TABLE(L, conn->onDisconnectedRef, table_index, "ondisconnected");
    SET_FUNC_REF_FROM_TABLE(L, conn->onConnectedRef, table_index, "onconnected");

    return 0;
}

// Initialize base connection
int tcpd_base_conn_init(tcpd_base_conn_t *conn, tcpd_conn_type_t type, lua_State *L) {
    if (!conn || !L) return -1;

    memset(conn, 0, sizeof(tcpd_base_conn_t));

    conn->type = type;
    conn->state = TCPD_CONN_DISCONNECTED;
    conn->mainthread = utlua_mainthread(L);
    conn->buf = NULL;
    conn->ssl_ctx = NULL;
    conn->host = NULL;
    conn->port = 0;

    // Initialize configuration with defaults
    tcpd_config_set_defaults(&conn->config);

    // Initialize callbacks
    conn->onReadRef = LUA_NOREF;
    conn->onSendReadyRef = LUA_NOREF;
    conn->onDisconnectedRef = LUA_NOREF;
    conn->onConnectedRef = LUA_NOREF;

    memset(conn->ip, 0, INET6_ADDRSTRLEN);

    return 0;
}

// Clean up base connection
void tcpd_base_conn_cleanup(tcpd_base_conn_t *conn) {
    if (!conn) return;

    // Close bufferevent if still open
    if (conn->buf) {
        tcpd_shutdown_bufferevent(conn->buf);
        conn->buf = NULL;
    }

    // Clear callback references
    lua_State *mt = conn->mainthread;
    if (mt) {
        CLEAR_REF(mt, conn->onReadRef);
        CLEAR_REF(mt, conn->onSendReadyRef);
        CLEAR_REF(mt, conn->onDisconnectedRef);
        CLEAR_REF(mt, conn->onConnectedRef);
    }

    // Free host string
    if (conn->host) {
        free(conn->host);
        conn->host = NULL;
    }

    // Clean up SSL context (will be implemented in SSL module)
    if (conn->ssl_ctx) {
        tcpd_ssl_context_release(conn->ssl_ctx, mt);
        conn->ssl_ctx = NULL;
    }

    conn->state = TCPD_CONN_DISCONNECTED;
    conn->mainthread = NULL;
}

// Helper function to format TCP connection info
char* tcpd_format_connection_info(const tcpd_base_conn_t *conn) {
    if (!conn) return NULL;

    const char *host = conn->host ? conn->host : "unknown";
    const char *local_ip = "unknown";
    int local_port = 0;
    evutil_socket_t fd = -1;

    // Get socket info if available
    if (conn->buf) {
        fd = bufferevent_getfd(conn->buf);
        if (fd >= 0) {
            struct sockaddr_storage ss;
            socklen_t len = sizeof(ss);
            if (getsockname(fd, (struct sockaddr*)&ss, &len) == 0) {
                if (ss.ss_family == AF_INET) {
                    struct sockaddr_in *addr = (struct sockaddr_in*)&ss;
                    inet_ntop(AF_INET, &addr->sin_addr, (char*)conn->ip, sizeof(conn->ip));
                    local_ip = conn->ip;
                    local_port = ntohs(addr->sin_port);
                } else if (ss.ss_family == AF_INET6) {
                    struct sockaddr_in6 *addr = (struct sockaddr_in6*)&ss;
                    inet_ntop(AF_INET6, &addr->sin6_addr, (char*)conn->ip, sizeof(conn->ip));
                    local_ip = conn->ip;
                    local_port = ntohs(addr->sin6_port);
                }
            }
        }
    }

    size_t len = strlen(host) + strlen(local_ip) + 128;
    char *info = malloc(len);

    if (info) {
        snprintf(info, len,
                "<TCP: target=%s:%d, local=%s:%d, fd=%d, state=%d>",
                host, conn->port, local_ip, local_port, fd, conn->state);
    }

    return info;
}