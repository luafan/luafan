#include "tcpd_common.h"
#include "tcpd_ssl.h"  // For tcpd_ssl_context_release function
#include <event2/buffer.h>
#include <string.h>

#define BUFLEN 1024

// Forward declarations
static tcpd_error_t tcpd_analyze_event_error(struct bufferevent *bev, short events);
static void tcpd_call_lua_callback(lua_State *mainthread, int callback_ref, int argc);

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
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onReadRef);
    lua_pushlstring(co, (const char *)ba.buffer, ba.total);

    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);

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
        lua_lock(mainthread);
        lua_State *co = lua_newthread(mainthread);
        PUSH_REF(mainthread);
        lua_unlock(mainthread);

        lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onSendReadyRef);
        FAN_RESUME(co, mainthread, 0);
        POP_REF(mainthread);
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
            lua_lock(mainthread);
            lua_State *co = lua_newthread(mainthread);
            PUSH_REF(mainthread);
            lua_unlock(mainthread);

            lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onConnectedRef);
            FAN_RESUME(co, mainthread, 0);
            POP_REF(mainthread);
        }
        return;
    }

    // Handle disconnection events
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_TIMEOUT)) {
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
            lua_lock(mainthread);
            lua_State *co = lua_newthread(mainthread);
            PUSH_REF(mainthread);
            lua_unlock(mainthread);

            lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onDisconnectedRef);

            // Push error message
            const char *error_msg = tcpd_error_to_string(&error);
            if (error_msg) {
                lua_pushstring(co, error_msg);
            } else {
                lua_pushnil(co);
            }

            // Clear the callback reference
            CLEAR_REF(mainthread, conn->onDisconnectedRef);

            FAN_RESUME(co, mainthread, 1);
            POP_REF(mainthread);
        }

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

#if FAN_HAS_OPENSSL && OPENSSL_VERSION_NUMBER < 0x1010000fL
    SSL *ssl = bufferevent_openssl_get_ssl(bev);
    if (ssl) {
        SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
        if (SSL_shutdown(ssl) == 0) {
            SSL_shutdown(ssl);
        }
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
    if (conn->mainthread) {
        CLEAR_REF(conn->mainthread, conn->onReadRef);
        CLEAR_REF(conn->mainthread, conn->onSendReadyRef);
        CLEAR_REF(conn->mainthread, conn->onDisconnectedRef);
        CLEAR_REF(conn->mainthread, conn->onConnectedRef);
    }

    // Free host string
    if (conn->host) {
        free(conn->host);
        conn->host = NULL;
    }

    // Clean up SSL context (will be implemented in SSL module)
    if (conn->ssl_ctx) {
        tcpd_ssl_context_release(conn->ssl_ctx, conn->mainthread);
        conn->ssl_ctx = NULL;
    }

    conn->state = TCPD_CONN_DISCONNECTED;
}