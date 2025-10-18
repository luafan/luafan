#include "udpd_common.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>

// Helper function to push UDP connection object to Lua stack based on stored reference
void udpd_push_connection_object(lua_State *co, udpd_base_conn_t *conn) {
    if (!co || !conn) return;

    // Use the selfRef from connection if available
    if (conn->selfRef != LUA_NOREF) {
        lua_rawgeti(co, LUA_REGISTRYINDEX, conn->selfRef);
        return;
    }

    // For connections without selfRef, push nil
    lua_pushnil(co);
}

// Common read callback for UDP connections
void udpd_common_readcb(evutil_socket_t fd, short what, void *ctx) {
    udpd_base_conn_t *conn = (udpd_base_conn_t *)ctx;

    if (!conn || conn->onReadRef == LUA_NOREF) {
        return;
    }

    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[UDPD_MAX_PACKET_SIZE];

    // Receive UDP packet
    ssize_t len = recvfrom(fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&client_addr, &client_len);

    if (len < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // Handle read error
            udpd_handle_read_error(conn, errno);
        }
        return;
    }

    if (len == 0) {
        // UDP doesn't have connection close concept, but handle gracefully
        return;
    }

    // Process received data
    udpd_process_received_data(conn, buffer, len, &client_addr, client_len);
}

// Common write callback for UDP connections
void udpd_common_writecb(evutil_socket_t fd, short what, void *ctx) {
    udpd_base_conn_t *conn = (udpd_base_conn_t *)ctx;

    if (!conn || conn->onSendReadyRef == LUA_NOREF) {
        return;
    }

    // Call Lua send ready callback
    lua_State *mainthread = conn->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onSendReadyRef);

    int argc = 0;
    // Check if callback_self_first is enabled
    if (conn->config.base.callback_self_first) {
        udpd_push_connection_object(co, conn);
        argc = 1;
    }

    FAN_RESUME(co, mainthread, argc);
    POP_REF(mainthread);
}

// Process received UDP data
void udpd_process_received_data(udpd_base_conn_t *conn, const char *data, size_t len,
                               const struct sockaddr_storage *from_addr, socklen_t from_len) {
    if (!conn || !data || len == 0) return;

    lua_State *mainthread = conn->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    // Push read callback function
    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onReadRef);

    int argc = 0;
    // Check if callback_self_first is enabled
    if (conn->config.base.callback_self_first) {
        udpd_push_connection_object(co, conn);
        argc++;
    }

    // Push received data
    lua_pushlstring(co, data, len);
    argc++;

    // Create destination object for sender
    udpd_dest_t *dest = lua_newuserdata(co, sizeof(udpd_dest_t));
    luaL_getmetatable(co, LUA_UDPD_DEST_TYPE);
    lua_setmetatable(co, -2);

    // Initialize destination with sender's address
    memcpy(&dest->addr, from_addr, from_len);
    dest->addrlen = from_len;
    dest->host = NULL;  // Will be resolved on demand
    dest->port = udpd_dest_get_port(dest);
    argc++;

    // Resume coroutine with data and sender info (and self if enabled)
    FAN_RESUME(co, mainthread, argc);
    POP_REF(mainthread);
}

// Handle read errors
void udpd_handle_read_error(udpd_base_conn_t *conn, int error_code) {
    if (!conn) return;

    // Convert system error to structured error
    tcpd_error_t error = udpd_error_from_socket_error(error_code);

    // Log error (if logging is enabled)
    LOGE("UDP read error on fd %d: %s", conn->socket_fd, tcpd_error_to_string(&error));

    // Update connection state
    conn->state = UDPD_CONN_ERROR;

    // Disable read events to prevent error loop
    if (conn->read_ev) {
        event_del(conn->read_ev);
    }

    // Cleanup error
    tcpd_error_cleanup(&error);
}

// Initialize base connection
int udpd_base_conn_init(udpd_base_conn_t *conn, udpd_conn_type_t type, lua_State *L) {
    if (!conn || !L) return -1;

    memset(conn, 0, sizeof(udpd_base_conn_t));

    // Initialize basic fields
    conn->socket_fd = -1;
    conn->state = UDPD_CONN_DISCONNECTED;
    conn->type = type;
    conn->mainthread = L;
    conn->selfRef = LUA_NOREF;
    conn->onReadRef = LUA_NOREF;
    conn->onSendReadyRef = LUA_NOREF;

    // Initialize configuration
    udpd_config_init(&conn->config);

    // Initialize network fields
    conn->host = NULL;
    conn->bind_host = NULL;
    conn->port = 0;
    conn->bind_port = 0;
    conn->interface = 0;

    // Initialize address structures
    memset(&conn->addr, 0, sizeof(conn->addr));
    memset(&conn->bind_addr, 0, sizeof(conn->bind_addr));
    conn->addrlen = 0;
    conn->bind_addrlen = 0;

    // Initialize event pointers
    conn->read_ev = NULL;
    conn->write_ev = NULL;
    conn->dns_request = NULL;

    return 0;
}

// Clean up base connection
void udpd_base_conn_cleanup(udpd_base_conn_t *conn) {
    if (!conn) return;

    // Clear Lua references
    if (conn->mainthread) {
        CLEAR_REF(conn->mainthread, conn->selfRef);
        CLEAR_REF(conn->mainthread, conn->onReadRef);
        CLEAR_REF(conn->mainthread, conn->onSendReadyRef);

        // Clean up REF_STATE reference - this was the missing piece!
        REF_STATE_CLEAR(conn);
    }

    // Clean up allocated strings
    if (conn->host) {
        free(conn->host);
        conn->host = NULL;
    }
    if (conn->bind_host) {
        free(conn->bind_host);
        conn->bind_host = NULL;
    }

    // Clean up events
    if (conn->read_ev) {
        event_free(conn->read_ev);
        conn->read_ev = NULL;
    }
    if (conn->write_ev) {
        event_free(conn->write_ev);
        conn->write_ev = NULL;
    }

    // Close socket
    if (conn->socket_fd >= 0) {
        EVUTIL_CLOSESOCKET(conn->socket_fd);
        conn->socket_fd = -1;
    }

    // Clean up DNS request
    if (conn->dns_request) {
        udpd_dns_request_cleanup(conn->dns_request);
        conn->dns_request = NULL;
    }

    // Clean up configuration
    udpd_config_cleanup(&conn->config);

    // Update state
    conn->state = UDPD_CONN_DISCONNECTED;
}

// Set callbacks from Lua table
int udpd_base_conn_set_callbacks(udpd_base_conn_t *conn, lua_State *L, int table_index) {
    if (!conn || !L) return -1;

    // Set onread callback
    lua_getfield(L, table_index, "onread");
    if (lua_type(L, -1) == LUA_TFUNCTION) {
        conn->onReadRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        conn->onReadRef = LUA_NOREF;
    }

    // Set onsendready callback
    lua_getfield(L, table_index, "onsendready");
    if (lua_type(L, -1) == LUA_TFUNCTION) {
        conn->onSendReadyRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        conn->onSendReadyRef = LUA_NOREF;
    }

    return 0;
}

// Create and configure socket
int udpd_base_conn_create_socket(udpd_base_conn_t *conn) {
    if (!conn) return -1;

    // Create UDP socket
    evutil_socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }

    conn->socket_fd = fd;

    // Set non-blocking
    if (udpd_socket_set_nonblock(fd) < 0) {
        goto error;
    }

    // Apply socket options from configuration
    if (udpd_config_apply_socket_options(&conn->config, fd) < 0) {
        goto error;
    }

    conn->state = UDPD_CONN_BINDING;
    return 0;

error:
    if (fd >= 0) {
        EVUTIL_CLOSESOCKET(fd);
        conn->socket_fd = -1;
    }
    conn->state = UDPD_CONN_ERROR;
    return -1;
}

// Bind socket to address
int udpd_base_conn_bind(udpd_base_conn_t *conn) {
    if (!conn || conn->socket_fd < 0) return -1;

    struct sockaddr *bind_addr = NULL;
    socklen_t bind_addrlen = 0;

    if (conn->bind_host) {
        // Resolve bind address
        char portbuf[6];
        evutil_snprintf(portbuf, sizeof(portbuf), "%d", conn->bind_port);

        struct evutil_addrinfo hints = {0};
        struct evutil_addrinfo *answer = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

        int err = evutil_getaddrinfo(conn->bind_host, portbuf, &hints, &answer);
        if (err < 0 || !answer) {
            return -1;
        }

        memcpy(&conn->bind_addr, answer->ai_addr, answer->ai_addrlen);
        conn->bind_addrlen = answer->ai_addrlen;
        evutil_freeaddrinfo(answer);

        bind_addr = (struct sockaddr *)&conn->bind_addr;
        bind_addrlen = conn->bind_addrlen;
    } else {
        // Bind to INADDR_ANY
        struct sockaddr_in *addr = (struct sockaddr_in *)&conn->bind_addr;
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons(conn->bind_port);
        addr->sin_addr.s_addr = htonl(INADDR_ANY);

        conn->bind_addrlen = sizeof(*addr);
        bind_addr = (struct sockaddr *)addr;
        bind_addrlen = conn->bind_addrlen;
    }

    // Perform bind
    if (bind(conn->socket_fd, bind_addr, bind_addrlen) < 0) {
        return -1;
    }

    // Get actual bound port if it was 0
    if (conn->bind_port == 0) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(conn->socket_fd, (struct sockaddr *)&addr, &len) == 0) {
            conn->bind_port = ntohs(addr.sin_port);
        }
    }

    // Apply bind-specific options
    udpd_config_apply_bind_options(&conn->config, conn->socket_fd);

    conn->state = UDPD_CONN_BOUND;
    return 0;
}

// Set up event handling
int udpd_base_conn_setup_events(udpd_base_conn_t *conn) {
    if (!conn || conn->socket_fd < 0) return -1;

    // Set up read event if callback is registered
    if (conn->onReadRef != LUA_NOREF) {
        conn->read_ev = event_new(event_mgr_base(), conn->socket_fd,
                                 EV_READ | EV_PERSIST, udpd_common_readcb, conn);
        if (!conn->read_ev) {
            return -1;
        }
        event_add(conn->read_ev, NULL);
    }

    // Set up write event if callback is registered
    if (conn->onSendReadyRef != LUA_NOREF) {
        conn->write_ev = event_new(event_mgr_base(), conn->socket_fd,
                                  EV_WRITE, udpd_common_writecb, conn);
        if (!conn->write_ev) {
            if (conn->read_ev) {
                event_free(conn->read_ev);
                conn->read_ev = NULL;
            }
            return -1;
        }
        // Don't add write event initially, only when requested
    }

    conn->state = UDPD_CONN_READY;
    return 0;
}

// Request send ready notification
int udpd_base_conn_request_send_ready(udpd_base_conn_t *conn) {
    if (!conn || !conn->write_ev) {
        return -1;
    }

    // Add write event to be notified when socket is ready for writing
    return event_add(conn->write_ev, NULL);
}

// Connection cleanup on disconnect
void udpd_connection_cleanup_on_disconnect(udpd_base_conn_t *conn) {
    if (!conn) return;

    // For UDP, we just clean up resources but don't change state dramatically
    // since UDP is connectionless

    // Remove events
    if (conn->read_ev) {
        event_del(conn->read_ev);
    }
    if (conn->write_ev) {
        event_del(conn->write_ev);
    }

    // Update state
    conn->state = UDPD_CONN_DISCONNECTED;
}