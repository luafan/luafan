#ifndef CONN_CONFIG_H
#define CONN_CONFIG_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <event2/util.h>

// Shared socket option helpers used by both TCP and UDP config modules.
// These return 0 on success, -1 on error. Callers decide whether to treat
// errors as fatal (TCP) or non-fatal (UDP).

static inline int conn_config_apply_sndbuf(evutil_socket_t fd, int size) {
    if (fd < 0 || size <= 0) return 0;
    return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0 ? 0 : -1;
}

static inline int conn_config_apply_rcvbuf(evutil_socket_t fd, int size) {
    if (fd < 0 || size <= 0) return 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0 ? 0 : -1;
}

static inline int conn_config_apply_bound_if(evutil_socket_t fd, int iface) {
    if (fd < 0 || iface <= 0) return 0;
#ifdef IP_BOUND_IF
    return setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &iface, sizeof(iface)) == 0 ? 0 : -1;
#else
    (void)fd;
    (void)iface;
    return 0;
#endif
}

#endif // CONN_CONFIG_H
