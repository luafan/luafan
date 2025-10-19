#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <event2/dns.h>

// Create DNS request structure
udpd_dns_request_t* udpd_dns_request_create(const char *hostname, int port) {
    if (!hostname || port <= 0 || port > 65535) return NULL;

    udpd_dns_request_t *request = malloc(sizeof(udpd_dns_request_t));
    if (!request) return NULL;

    memset(request, 0, sizeof(udpd_dns_request_t));

    request->hostname = strdup(hostname);
    if (!request->hostname) {
        free(request);
        return NULL;
    }

    request->port = port;
    request->mainthread = NULL;
    request->_ref_ = 0;
    request->yielded = 0;
    request->callback = NULL;
    request->callback_ctx = NULL;

    return request;
}

// Clean up DNS request
void udpd_dns_request_cleanup(udpd_dns_request_t *request) {
    if (!request) return;

    if (request->hostname) {
        free(request->hostname);
        request->hostname = NULL;
    }

    // Clear Lua reference if set
    if (request->mainthread && request->_ref_ > 0) {
        REF_STATE_CLEAR(request);
    }

    free(request);
}

// DNS resolution callback for connection establishment
void udpd_conn_dns_callback(int errcode, struct evutil_addrinfo *addr, void *ptr) {
    udpd_base_conn_t *conn = (udpd_base_conn_t *)ptr;
    lua_State *L = NULL;

    if (!conn) return;

    REF_STATE_GET(conn, L);

    if (errcode) {
        // DNS resolution failed
        conn->state = UDPD_CONN_ERROR;

        if (conn->selfRef != LUA_NOREF) {
            // Return error to Lua
            lua_pushnil(L);
            lua_pushfstring(L, "DNS resolution failed for '%s': %s",
                           conn->host, evutil_gai_strerror(errcode));

            FAN_RESUME(L, NULL, 2);
            CLEAR_REF(L, conn->selfRef);
        }
    } else {
        // DNS resolution successful
        memcpy(&conn->addr, addr->ai_addr, addr->ai_addrlen);
        conn->addrlen = addr->ai_addrlen;
        evutil_freeaddrinfo(addr);

        // Now create socket and set up connection
        int setup_success = 1;
        if (udpd_base_conn_create_socket(conn) < 0) {
            setup_success = 0;
        } else if (conn->bind_host && udpd_base_conn_bind(conn) < 0) {
            setup_success = 0;
        } else if (udpd_base_conn_setup_events(conn) < 0) {
            setup_success = 0;
        }

        if (setup_success) {
            conn->state = UDPD_CONN_READY;

            if (conn->selfRef != LUA_NOREF) {
                // Return success to Lua - push the connection object
                lua_rawgeti(L, LUA_REGISTRYINDEX, conn->selfRef);
                FAN_RESUME(L, NULL, 1);
                CLEAR_REF(L, conn->selfRef);
            }
        } else {
            conn->state = UDPD_CONN_ERROR;

            if (conn->selfRef != LUA_NOREF) {
                // Return error to Lua
                lua_pushnil(L);
                lua_pushstring(L, "Failed to set up UDP connection after DNS resolution");
                FAN_RESUME(L, NULL, 2);
                CLEAR_REF(L, conn->selfRef);
            }
        }
    }

    REF_STATE_CLEAR(conn);

    // Clean up DNS request
    if (conn->dns_request) {
        udpd_dns_request_cleanup(conn->dns_request);
        conn->dns_request = NULL;
    }
}

// DNS resolution callback for destination creation
void udpd_dest_dns_callback(int errcode, struct evutil_addrinfo *addr, void *ptr) {
    udpd_dns_request_t *request = (udpd_dns_request_t *)ptr;
    lua_State *L = NULL;

    if (!request) return;

    REF_STATE_GET(request, L);

    if (errcode) {
        // DNS resolution failed
        lua_pushnil(L);
        lua_pushfstring(L, "DNS resolution failed for '%s': %s",
                       request->hostname, evutil_gai_strerror(errcode));

        if (request->yielded) {
            FAN_RESUME(L, NULL, 2);
        }
    } else {
        // DNS resolution successful - create destination object
        udpd_dest_t *dest = lua_newuserdata(L, sizeof(udpd_dest_t));
        luaL_getmetatable(L, LUA_UDPD_DEST_TYPE);
        lua_setmetatable(L, -2);

        // Initialize destination with resolved address
        memcpy(&dest->addr, addr->ai_addr, addr->ai_addrlen);
        dest->addrlen = addr->ai_addrlen;
        dest->host = strdup(request->hostname);
        dest->port = request->port;

        evutil_freeaddrinfo(addr);

        if (request->yielded) {
            FAN_RESUME(L, NULL, 1);
        }
    }

    REF_STATE_CLEAR(request);
    udpd_dns_request_cleanup(request);
}

// DNS resolution callback for multiple destination creation
void udpd_dests_dns_callback(int errcode, struct evutil_addrinfo *addr_list, void *ptr) {
    udpd_dns_request_t *request = (udpd_dns_request_t *)ptr;
    lua_State *L = NULL;

    if (!request) return;

    REF_STATE_GET(request, L);

    if (errcode) {
        // DNS resolution failed
        lua_pushnil(L);
        lua_pushfstring(L, "DNS resolution failed for '%s': %s",
                       request->hostname, evutil_gai_strerror(errcode));

        if (request->yielded) {
            FAN_RESUME(L, NULL, 2);
        }
    } else {
        // DNS resolution successful - create table with all destination objects
        lua_newtable(L);
        int table_index = 1;

        // Iterate through all resolved addresses
        struct evutil_addrinfo *addr = addr_list;
        while (addr) {
            // Create destination object for each address
            udpd_dest_t *dest = lua_newuserdata(L, sizeof(udpd_dest_t));
            luaL_getmetatable(L, LUA_UDPD_DEST_TYPE);
            lua_setmetatable(L, -2);

            // Initialize destination with resolved address
            memcpy(&dest->addr, addr->ai_addr, addr->ai_addrlen);
            dest->addrlen = addr->ai_addrlen;
            dest->host = strdup(request->hostname);
            dest->port = request->port;

            // Add to table
            lua_rawseti(L, -2, table_index++);

            addr = addr->ai_next;
        }

        evutil_freeaddrinfo(addr_list);

        if (request->yielded) {
            FAN_RESUME(L, NULL, 1);
        }
    }

    REF_STATE_CLEAR(request);
    udpd_dns_request_cleanup(request);
}

// Resolve hostname asynchronously for connection
int udpd_dns_resolve_for_connection(udpd_base_conn_t *conn) {
    if (!conn || !conn->host) return -1;

    // Create DNS request
    conn->dns_request = udpd_dns_request_create(conn->host, conn->port);
    if (!conn->dns_request) return -1;

    // Set up port string
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", conn->port);

    // Set up hints
    struct evutil_addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    // Start DNS resolution
    struct evdns_getaddrinfo_request *req =
        evdns_getaddrinfo(event_mgr_dnsbase(), conn->host, portbuf, &hints,
                         udpd_conn_dns_callback, conn);

    if (!req) {
        udpd_dns_request_cleanup(conn->dns_request);
        conn->dns_request = NULL;
        return -1;
    }

    conn->state = UDPD_CONN_RESOLVING;
    return 0;
}

// Resolve hostname asynchronously for destination creation
int udpd_dns_resolve_for_destination(const char *hostname, int port, lua_State *L) {
    if (!hostname || !L) return -1;

    // Create DNS request
    udpd_dns_request_t *request = udpd_dns_request_create(hostname, port);
    if (!request) return -1;

    // Set up Lua state reference
    REF_STATE_SET(request, L);
    request->yielded = 0;

    // Set up port string
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", port);

    // Set up hints
    struct evutil_addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    // Start DNS resolution
    struct evdns_getaddrinfo_request *req =
        evdns_getaddrinfo(event_mgr_dnsbase(), hostname, portbuf, &hints,
                         udpd_dest_dns_callback, request);

    if (!req) {
        udpd_dns_request_cleanup(request);
        return -1;
    }

    // Check if resolution completed synchronously
    if (lua_gettop(L) > 2) {
        // Synchronous completion - don't yield
        return lua_gettop(L) - 2;
    } else {
        // Asynchronous - yield coroutine
        request->yielded = 1;
        return lua_yield(L, 0);
    }
}

// Resolve hostname asynchronously for multiple destination creation
int udpd_dns_resolve_for_destinations(const char *hostname, int port, lua_State *L) {
    if (!hostname || !L) return -1;

    // Create DNS request
    udpd_dns_request_t *request = udpd_dns_request_create(hostname, port);
    if (!request) return -1;

    // Set up Lua state reference
    REF_STATE_SET(request, L);
    request->yielded = 0;

    // Set up port string
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", port);

    // Set up hints for multiple addresses
    struct evutil_addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;  // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    // Start DNS resolution for multiple addresses
    struct evdns_getaddrinfo_request *req =
        evdns_getaddrinfo(event_mgr_dnsbase(), hostname, portbuf, &hints,
                         udpd_dests_dns_callback, request);

    if (!req) {
        udpd_dns_request_cleanup(request);
        return -1;
    }

    // Check if resolution completed synchronously
    if (lua_gettop(L) > 2) {
        // Synchronous completion - don't yield
        return lua_gettop(L) - 2;
    } else {
        // Asynchronous - yield coroutine
        request->yielded = 1;
        return lua_yield(L, 0);
    }
}

// Generic DNS resolution with custom callback
int udpd_dns_resolve_async(udpd_dns_request_t *request,
                          void (*callback)(int, struct evutil_addrinfo*, void*),
                          void *ctx) {
    if (!request || !callback) return -1;

    request->callback = callback;
    request->callback_ctx = ctx;

    // Set up port string
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", request->port);

    // Set up hints
    struct evutil_addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    // Start DNS resolution
    struct evdns_getaddrinfo_request *req =
        evdns_getaddrinfo(event_mgr_dnsbase(), request->hostname, portbuf, &hints,
                         callback, ctx);

    return req ? 0 : -1;
}

// Synchronous DNS resolution (blocking)
int udpd_dns_resolve_sync(const char *hostname, int port,
                         struct sockaddr_storage *addr, socklen_t *addrlen) {
    if (!hostname || !addr || !addrlen) return -1;

    // Set up port string
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", port);

    // Set up hints
    struct evutil_addrinfo hints = {0};
    struct evutil_addrinfo *answer = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    // Perform synchronous resolution
    int err = evutil_getaddrinfo(hostname, portbuf, &hints, &answer);
    if (err < 0 || !answer) {
        return -1;
    }

    // Copy result
    if (answer->ai_addr && answer->ai_addrlen <= sizeof(*addr)) {
        memcpy(addr, answer->ai_addr, answer->ai_addrlen);
        *addrlen = answer->ai_addrlen;
    } else {
        evutil_freeaddrinfo(answer);
        return -1;
    }

    evutil_freeaddrinfo(answer);
    return 0;
}


// Cancel pending DNS resolution
void udpd_dns_cancel_resolution(udpd_base_conn_t *conn) {
    if (!conn || !conn->dns_request) return;

    // Note: libevent doesn't provide a direct way to cancel DNS requests
    // We just clean up our state and the callback will be ignored
    udpd_dns_request_cleanup(conn->dns_request);
    conn->dns_request = NULL;

    conn->state = UDPD_CONN_DISCONNECTED;
}