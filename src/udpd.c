#include "udpd_common.h"
#include <net/if.h>

// Refactored UDP module using modular components
// This file now focuses on Lua interface and high-level coordination

// Extended connection structure (inherits from base)
typedef struct {
    udpd_base_conn_t base;
    // Add any connection-specific extensions here if needed
} udpd_conn_t;

// Lua garbage collection for UDP connections
LUA_API int lua_udpd_conn_gc(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    udpd_base_conn_cleanup(&conn->base);
    return 0;
}

// Create new UDP connection
LUA_API int udpd_new(lua_State *L) {
    event_mgr_init();

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    // Create connection object
    udpd_conn_t *conn = lua_newuserdata(L, sizeof(udpd_conn_t));
    memset(conn, 0, sizeof(udpd_conn_t));

    // Set metatable
    luaL_getmetatable(L, LUA_UDPD_CONNECTION_TYPE);
    lua_setmetatable(L, -2);

    // Initialize base connection
    udpd_base_conn_init(&conn->base, UDPD_CONN_TYPE_CLIENT, utlua_mainthread(L));

    // Extract configuration from Lua table
    udpd_config_from_lua_table(L, 1, &conn->base.config);

    // Set callbacks
    udpd_base_conn_set_callbacks(&conn->base, L, 1);

    // Extract network parameters
    lua_getfield(L, 1, "host");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *host = lua_tostring(L, -1);
        conn->base.host = strdup(host);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "port");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        conn->base.port = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "bind_host");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *bind_host = lua_tostring(L, -1);
        conn->base.bind_host = strdup(bind_host);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "bind_port");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        conn->base.bind_port = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    // Extract interface
    conn->base.interface = udpd_extract_interface_from_lua(L, 1);
    conn->base.config.base.interface = conn->base.interface;

    // Set up Lua state reference for async operations
    REF_STATE_SET((&conn->base), L);

    // Store self-reference if callback_self_first is enabled
    if (conn->base.config.base.callback_self_first) {
        lua_pushvalue(L, -1);  // Push the connection object again
        conn->base.selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        conn->base.selfRef = LUA_NOREF;
    }

    // If we have a target host, check if it's an IP address first
    if (conn->base.host && conn->base.port > 0) {
        if (udpd_is_ip_address(conn->base.host)) {
            // Direct IP address - create address directly
            if (udpd_create_address_from_string(conn->base.host, conn->base.port,
                                              &conn->base.addr, &conn->base.addrlen) < 0) {
                lua_pushnil(L);
                lua_pushstring(L, "Failed to create address from IP");
                return 2;
            }

            // Create socket and set up connection
            if (udpd_base_conn_create_socket(&conn->base) < 0) {
                lua_pushnil(L);
                lua_pushstring(L, "Failed to create UDP socket");
                return 2;
            }

            if (conn->base.bind_host && udpd_base_conn_bind(&conn->base) < 0) {
                lua_pushnil(L);
                lua_pushstring(L, "Failed to bind UDP socket");
                return 2;
            }

            if (udpd_base_conn_setup_events(&conn->base) < 0) {
                lua_pushnil(L);
                lua_pushstring(L, "Failed to setup events");
                return 2;
            }

            conn->base.state = UDPD_CONN_READY;
            return 1;  // Return connection object directly
        } else {
            // Hostname - start asynchronous DNS resolution
            int result = udpd_dns_resolve_for_connection(&conn->base);
            if (result < 0) {
                // DNS resolution setup failed
                lua_pushnil(L);
                lua_pushstring(L, "Failed to start DNS resolution");
                return 2;
            }

            // Store self-reference for async callback
            conn->base.selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
            return lua_yield(L, 0);
        }
    } else {
        // No target host - just set up for binding
        if (udpd_base_conn_create_socket(&conn->base) < 0) {
            lua_pushnil(L);
            lua_pushstring(L, "Failed to create UDP socket");
            return 2;
        }

        if (udpd_base_conn_bind(&conn->base) < 0) {
            lua_pushnil(L);
            lua_pushstring(L, "Failed to bind UDP socket");
            return 2;
        }

        if (udpd_base_conn_setup_events(&conn->base) < 0) {
            lua_pushnil(L);
            lua_pushstring(L, "Failed to setup events");
            return 2;
        }

        return 1;  // Return connection object
    }
}

// Create destination object from host:port string
LUA_API int udpd_conn_make_dest(lua_State *L) {
    event_mgr_init();

    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    lua_settop(L, 2);

    // Try synchronous resolution first (for IP addresses)
    if (udpd_is_ip_address(host)) {
        udpd_dest_t *dest = udpd_dest_create_from_string(host, port);
        if (dest) {
            // Push destination to Lua stack
            udpd_dest_t *lua_dest = lua_newuserdata(L, sizeof(udpd_dest_t));
            memcpy(lua_dest, dest, sizeof(udpd_dest_t));
            lua_dest->host = dest->host ? strdup(dest->host) : NULL;

            luaL_getmetatable(L, LUA_UDPD_DEST_TYPE);
            lua_setmetatable(L, -2);

            udpd_dest_cleanup(dest);  // Clean up temporary dest
            return 1;
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "Invalid IP address");
            return 2;
        }
    }

    // Asynchronous DNS resolution for hostnames
    return udpd_dns_resolve_for_destination(host, port, L);
}

// Rebind UDP connection
LUA_API int lua_udpd_conn_rebind(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);

    // Close existing socket and events
    if (conn->base.read_ev) {
        event_free(conn->base.read_ev);
        conn->base.read_ev = NULL;
    }
    if (conn->base.write_ev) {
        event_free(conn->base.write_ev);
        conn->base.write_ev = NULL;
    }
    if (conn->base.socket_fd >= 0) {
        EVUTIL_CLOSESOCKET(conn->base.socket_fd);
        conn->base.socket_fd = -1;
    }

    // Recreate and rebind
    if (udpd_base_conn_create_socket(&conn->base) < 0 ||
        udpd_base_conn_bind(&conn->base) < 0 ||
        udpd_base_conn_setup_events(&conn->base) < 0) {
        return 0;
    }

    return 0;
}

// Send UDP data
LUA_API int udpd_conn_send(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    if (!udpd_validate_packet_size(len)) {
        lua_pushnil(L);
        lua_pushstring(L, "Packet size exceeds UDP maximum");
        return 2;
    }

    if (conn->base.socket_fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Socket not created");
        return 2;
    }

    ssize_t sent = 0;
    if (lua_gettop(L) > 2) {
        // Send to specific destination
        udpd_dest_t *dest = udpd_dest_from_lua(L, 3);
        sent = sendto(conn->base.socket_fd, data, len, 0,
                     (struct sockaddr *)&dest->addr, dest->addrlen);
    } else {
        // Send to default destination (from connection setup)
        if (conn->base.addrlen == 0) {
            lua_pushnil(L);
            lua_pushstring(L, "No destination address available");
            return 2;
        }
        sent = sendto(conn->base.socket_fd, data, len, 0,
                     (struct sockaddr *)&conn->base.addr, conn->base.addrlen);
    }

    if (sent < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }

    lua_pushinteger(L, sent);
    return 1;
}

// Request send ready notification
LUA_API int udpd_conn_send_request(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);

    if (conn->base.onSendReadyRef == LUA_NOREF) {
        luaL_error(L, "onsendready callback not defined");
    }

    if (udpd_base_conn_request_send_ready(&conn->base) < 0) {
        luaL_error(L, "Failed to request send ready notification");
    }

    return 0;
}

// Get connection string representation
LUA_API int udpd_conn_tostring(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    char *info = udpd_format_connection_info(&conn->base);

    if (info) {
        lua_pushstring(L, info);
        free(info);
        return 1;
    }

    lua_pushstring(L, "<UDP connection>");
    return 1;
}

// Get bound port
LUA_API int udpd_conn_get_port(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    int port = udpd_get_socket_port(conn->base.socket_fd);
    lua_pushinteger(L, port);
    return 1;
}

// Close UDP connection
LUA_API int udpd_conn_close(lua_State *L) {
    udpd_conn_t *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);

    // Perform complete cleanup including Lua registry references
    udpd_base_conn_cleanup(&conn->base);
    return 0;
}

// Module registration
static const luaL_Reg udpdlib[] = {
    {"new", udpd_new},
    {"make_dest", udpd_conn_make_dest},
    {NULL, NULL}
};

// Module initialization
LUA_API int luaopen_fan_udpd(lua_State *L) {
    // Register UDP connection metatable
    luaL_newmetatable(L, LUA_UDPD_CONNECTION_TYPE);

    lua_pushcfunction(L, udpd_conn_send);
    lua_setfield(L, -2, "send");

    lua_pushcfunction(L, udpd_conn_send_request);
    lua_setfield(L, -2, "send_req");

    lua_pushcfunction(L, udpd_conn_close);
    lua_setfield(L, -2, "close");

    lua_pushcfunction(L, lua_udpd_conn_rebind);
    lua_setfield(L, -2, "rebind");

    lua_pushcfunction(L, udpd_conn_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, udpd_conn_get_port);
    lua_setfield(L, -2, "getPort");

    lua_pushcfunction(L, lua_udpd_conn_gc);
    lua_setfield(L, -2, "__gc");

    // Set __index to self
    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    // Set up destination metatable
    udpd_dest_setup_metatable(L);

    // Create and return module table
    lua_newtable(L);
    luaL_register(L, NULL, udpdlib);

    return 1;
}
