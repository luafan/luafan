#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>

// Create destination from socket address
udpd_dest_t* udpd_dest_create(const struct sockaddr *addr, socklen_t addrlen) {
    if (!addr || addrlen == 0) return NULL;

    udpd_dest_t *dest = malloc(sizeof(udpd_dest_t));
    if (!dest) return NULL;

    memset(dest, 0, sizeof(udpd_dest_t));

    // Copy address
    if (addrlen <= sizeof(dest->addr)) {
        memcpy(&dest->addr, addr, addrlen);
        dest->addrlen = addrlen;
    } else {
        free(dest);
        return NULL;
    }

    // Extract port
    dest->port = udpd_dest_get_port(dest);
    dest->host = NULL;  // Will be set if needed

    return dest;
}

// Create destination from host string and port
udpd_dest_t* udpd_dest_create_from_string(const char *host, int port) {
    if (!host || port <= 0) return NULL;

    udpd_dest_t *dest = malloc(sizeof(udpd_dest_t));
    if (!dest) return NULL;

    memset(dest, 0, sizeof(udpd_dest_t));

    // Store host and port
    dest->host = strdup(host);
    if (!dest->host) {
        free(dest);
        return NULL;
    }
    dest->port = port;

    // Try to resolve address
    if (udpd_create_address_from_string(host, port, &dest->addr, &dest->addrlen) < 0) {
        // Resolution failed - clean up and return NULL
        free(dest->host);
        free(dest);
        return NULL;
    }

    return dest;
}

// Clean up destination
void udpd_dest_cleanup(udpd_dest_t *dest) {
    if (!dest) return;

    if (dest->host) {
        free(dest->host);
        dest->host = NULL;
    }

    free(dest);
}

// Compare two destinations for equality
int udpd_dest_equal(const udpd_dest_t *dest1, const udpd_dest_t *dest2) {
    if (!dest1 || !dest2) return 0;

    // Compare address lengths first
    if (dest1->addrlen != dest2->addrlen) return 0;

    // Compare addresses
    return memcmp(&dest1->addr, &dest2->addr, dest1->addrlen) == 0;
}

// Get host string from destination
const char* udpd_dest_get_host_string(const udpd_dest_t *dest) {
    if (!dest) return NULL;

    // Return cached hostname if available
    if (dest->host) return dest->host;

    // Convert address to string
    static char host_buf[INET6_ADDRSTRLEN];
    const char *result = NULL;

    if (dest->addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&dest->addr;
        result = inet_ntop(AF_INET, &addr_in->sin_addr, host_buf, sizeof(host_buf));
    } else if (dest->addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)&dest->addr;
        result = inet_ntop(AF_INET6, &addr_in6->sin6_addr, host_buf, sizeof(host_buf));
    }

    return result;
}

// Get port from destination
int udpd_dest_get_port(const udpd_dest_t *dest) {
    if (!dest) return 0;

    if (dest->addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&dest->addr;
        return ntohs(addr_in->sin_port);
    } else if (dest->addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)&dest->addr;
        return ntohs(addr_in6->sin6_port);
    }

    return 0;
}

// Create destination string representation
char* udpd_dest_to_string(const udpd_dest_t *dest) {
    if (!dest) return NULL;

    const char *host = udpd_dest_get_host_string(dest);
    if (!host) return NULL;

    int port = udpd_dest_get_port(dest);

    // Allocate buffer for "host:port" format
    size_t len = strlen(host) + 16;  // host + ":" + port + null terminator
    char *result = malloc(len);
    if (!result) return NULL;

    snprintf(result, len, "%s:%d", host, port);
    return result;
}

// Copy destination
udpd_dest_t* udpd_dest_copy(const udpd_dest_t *src) {
    if (!src) return NULL;

    udpd_dest_t *dest = malloc(sizeof(udpd_dest_t));
    if (!dest) return NULL;

    // Copy structure
    memcpy(dest, src, sizeof(udpd_dest_t));

    // Copy hostname if present
    if (src->host) {
        dest->host = strdup(src->host);
        if (!dest->host) {
            free(dest);
            return NULL;
        }
    } else {
        dest->host = NULL;
    }

    return dest;
}

// Lua interface functions for destination objects

// Lua: dest:getHost()
LUA_API int lua_udpd_dest_get_host(lua_State *L) {
    udpd_dest_t *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    const char *host = udpd_dest_get_host_string(dest);

    if (host) {
        lua_pushstring(L, host);
        return 1;
    }

    return 0;
}

// Lua: dest:getPort()
LUA_API int lua_udpd_dest_get_port(lua_State *L) {
    udpd_dest_t *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    int port = udpd_dest_get_port(dest);

    lua_pushinteger(L, port);
    return 1;
}

// Lua: dest:getIP()
LUA_API int lua_udpd_dest_get_ip(lua_State *L) {
    udpd_dest_t *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);

    if (!dest) {
        return 0;
    }

    char ip_buf[INET6_ADDRSTRLEN];
    const char *result = NULL;

    if (dest->addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&dest->addr;
        result = inet_ntop(AF_INET, &addr_in->sin_addr, ip_buf, sizeof(ip_buf));
    } else if (dest->addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)&dest->addr;
        result = inet_ntop(AF_INET6, &addr_in6->sin6_addr, ip_buf, sizeof(ip_buf));
    }

    if (result) {
        lua_pushstring(L, result);
        return 1;
    }

    return 0;
}

// Lua: tostring(dest)
LUA_API int lua_udpd_dest_tostring(lua_State *L) {
    udpd_dest_t *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    char *str = udpd_dest_to_string(dest);

    if (str) {
        lua_pushstring(L, str);
        free(str);
        return 1;
    }

    return 0;
}

// Lua: dest1 == dest2
LUA_API int lua_udpd_dest_eq(lua_State *L) {
    udpd_dest_t *dest1 = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    udpd_dest_t *dest2 = luaL_checkudata(L, 2, LUA_UDPD_DEST_TYPE);

    int equal = udpd_dest_equal(dest1, dest2);
    lua_pushboolean(L, equal);
    return 1;
}

// Lua: gc for destination objects
LUA_API int lua_udpd_dest_gc(lua_State *L) {
    udpd_dest_t *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);

    if (dest->host) {
        free(dest->host);
        dest->host = NULL;
    }

    return 0;
}

// Set up destination metatable
void udpd_dest_setup_metatable(lua_State *L) {
    // Create metatable for destination objects
    luaL_newmetatable(L, LUA_UDPD_DEST_TYPE);

    // Set methods
    lua_pushcfunction(L, lua_udpd_dest_get_host);
    lua_setfield(L, -2, "getHost");

    lua_pushcfunction(L, lua_udpd_dest_get_port);
    lua_setfield(L, -2, "getPort");

    lua_pushcfunction(L, lua_udpd_dest_get_ip);
    lua_setfield(L, -2, "getIP");

    lua_pushcfunction(L, lua_udpd_dest_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, lua_udpd_dest_eq);
    lua_setfield(L, -2, "__eq");

    lua_pushcfunction(L, lua_udpd_dest_gc);
    lua_setfield(L, -2, "__gc");

    // Set __index to self
    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);  // Remove metatable from stack
}

// Helper function: Create Lua destination object
void udpd_dest_push_to_lua(lua_State *L, const struct sockaddr *addr, socklen_t addrlen) {
    // Create userdata
    udpd_dest_t *dest = lua_newuserdata(L, sizeof(udpd_dest_t));
    memset(dest, 0, sizeof(udpd_dest_t));

    // Set metatable
    luaL_getmetatable(L, LUA_UDPD_DEST_TYPE);
    lua_setmetatable(L, -2);

    // Initialize destination
    if (addrlen <= sizeof(dest->addr)) {
        memcpy(&dest->addr, addr, addrlen);
        dest->addrlen = addrlen;
        dest->port = udpd_dest_get_port(dest);
        dest->host = NULL;
    }
}

// Helper function: Get destination from Lua stack
udpd_dest_t* udpd_dest_from_lua(lua_State *L, int index) {
    return luaL_checkudata(L, index, LUA_UDPD_DEST_TYPE);
}

// Utility: Check if destination is multicast
int udpd_dest_is_multicast(const udpd_dest_t *dest) {
    if (!dest || dest->addr.ss_family != AF_INET) return 0;

    const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&dest->addr;
    uint32_t ip = ntohl(addr_in->sin_addr.s_addr);

    // Check if IP is in multicast range (224.0.0.0 to 239.255.255.255)
    return (ip & 0xF0000000) == 0xE0000000;
}

// Utility: Check if destination is broadcast
int udpd_dest_is_broadcast(const udpd_dest_t *dest) {
    if (!dest || dest->addr.ss_family != AF_INET) return 0;

    const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&dest->addr;
    return addr_in->sin_addr.s_addr == INADDR_BROADCAST;
}

// Utility: Check if destination is loopback
int udpd_dest_is_loopback(const udpd_dest_t *dest) {
    if (!dest) return 0;

    if (dest->addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&dest->addr;
        uint32_t ip = ntohl(addr_in->sin_addr.s_addr);
        return (ip & 0xFF000000) == 0x7F000000;  // 127.x.x.x
    } else if (dest->addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)&dest->addr;
        return IN6_IS_ADDR_LOOPBACK(&addr_in6->sin6_addr);
    }

    return 0;
}