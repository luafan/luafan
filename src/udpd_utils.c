#include "udpd_common.h"
#include <fcntl.h>
#include <errno.h>
#include <net/if.h>

// Set socket to non-blocking mode
int udpd_socket_set_nonblock(evutil_socket_t fd) {
    if (fd < 0) return -1;

#ifdef O_NONBLOCK
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;

    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#else
    return evutil_make_socket_nonblocking(fd);
#endif
}

// Set socket reuse address option
int udpd_socket_set_reuse_addr(evutil_socket_t fd) {
    if (fd < 0) return -1;

    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

// Set socket broadcast option
int udpd_socket_set_broadcast(evutil_socket_t fd) {
    if (fd < 0) return -1;

    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
}

// Bind socket to specific network interface
int udpd_socket_bind_interface(evutil_socket_t fd, int interface) {
    if (fd < 0 || interface <= 0) return -1;

#ifdef IP_BOUND_IF
    return setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &interface, sizeof(interface));
#else
    // Interface binding not supported on this platform
    return 0;
#endif
}

// Create error from socket error code
tcpd_error_t udpd_error_from_socket_error(int error_code) {
    // Reuse TCP error handling since socket errors are similar
    return tcpd_error_from_socket_error(error_code);
}

// Create error from DNS error code
tcpd_error_t udpd_error_from_dns_error(int dns_error) {
    return tcpd_error_dns(dns_error);
}

// Create bind failed error
tcpd_error_t udpd_error_bind_failed(const char *host, int port) {
    tcpd_error_t error;
    error.type = TCPD_ERROR_CONNECTION_RESET;  // Reuse existing error type
    error.system_error = errno;

    // Create descriptive message
    size_t msg_len = strlen(host) + 64;
    error.message = malloc(msg_len);
    if (error.message) {
        snprintf(error.message, msg_len, "Failed to bind UDP socket to %s:%d", host, port);
    }

    return error;
}

// Get socket port (for bound sockets)
int udpd_get_socket_port(evutil_socket_t fd) {
    if (fd < 0) return 0;

    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        return 0;
    }

    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
        return ntohs(addr_in->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&addr;
        return ntohs(addr_in6->sin6_port);
    }

    return 0;
}

// Extract network interface from Lua table
int udpd_extract_interface_from_lua(lua_State *L, int table_index) {
    lua_getfield(L, table_index, "interface");
    int interface = 0;

    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *interface_name = lua_tostring(L, -1);
        interface = if_nametoindex(interface_name);
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        interface = (int)lua_tointeger(L, -1);
    }

    lua_pop(L, 1);
    return interface;
}

// Helper function to format UDP connection info
char* udpd_format_connection_info(const udpd_base_conn_t *conn) {
    if (!conn) return NULL;

    const char *host = conn->host ? conn->host : "unknown";
    const char *bind_host = conn->bind_host ? conn->bind_host : "any";

    size_t len = strlen(host) + strlen(bind_host) + 128;
    char *info = malloc(len);

    if (info) {
        snprintf(info, len,
                "<UDP: target=%s:%d, bind=%s:%d, fd=%d, state=%d>",
                host, conn->port, bind_host, conn->bind_port,
                conn->socket_fd, conn->state);
    }

    return info;
}

// Validate UDP packet size
int udpd_validate_packet_size(size_t size) {
    return (size > 0 && size <= UDPD_MAX_PACKET_SIZE);
}

// Check if address family is supported
int udpd_is_address_family_supported(int family) {
    return (family == AF_INET || family == AF_INET6);
}

// Convert sockaddr to string representation
char* udpd_sockaddr_to_string(const struct sockaddr *addr, socklen_t addrlen) {
    if (!addr) return NULL;

    char host[INET6_ADDRSTRLEN];
    int port = 0;
    const char *result = NULL;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)addr;
        result = inet_ntop(AF_INET, &addr_in->sin_addr, host, sizeof(host));
        port = ntohs(addr_in->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)addr;
        result = inet_ntop(AF_INET6, &addr_in6->sin6_addr, host, sizeof(host));
        port = ntohs(addr_in6->sin6_port);
    }

    if (!result) return NULL;

    size_t len = strlen(host) + 16;
    char *str = malloc(len);
    if (str) {
        snprintf(str, len, "%s:%d", host, port);
    }

    return str;
}

// Check if string is a valid IP address
int udpd_is_ip_address(const char *str) {
    if (!str) return 0;

    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;

    // Try IPv4 first
    if (inet_pton(AF_INET, str, &(sa4.sin_addr)) == 1) {
        return 1;
    }

    // Try IPv6
    if (inet_pton(AF_INET6, str, &(sa6.sin6_addr)) == 1) {
        return 1;
    }

    return 0;
}

// Create socket address from string and port
int udpd_create_address_from_string(const char *host, int port,
                                   struct sockaddr_storage *addr, socklen_t *addrlen) {
    if (!host || !addr || !addrlen || port <= 0) return -1;

    memset(addr, 0, sizeof(*addr));

    // Try IPv4 first
    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
    if (inet_pton(AF_INET, host, &addr4->sin_addr) == 1) {
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        *addrlen = sizeof(struct sockaddr_in);
        return 0;
    }

    // Try IPv6
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
    if (inet_pton(AF_INET6, host, &addr6->sin6_addr) == 1) {
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        *addrlen = sizeof(struct sockaddr_in6);
        return 0;
    }

    // Not a valid IP address
    return -1;
}