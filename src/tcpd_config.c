#include "tcpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <net/if.h>  // For if_nametoindex function

// Initialize configuration with zero values
int tcpd_config_init(tcpd_config_t *config) {
    if (!config) return -1;

    memset(config, 0, sizeof(tcpd_config_t));
    return 0;
}

// Set default configuration values
int tcpd_config_set_defaults(tcpd_config_t *config) {
    if (!config) return -1;

    // Buffer settings - 0 means use system defaults
    config->send_buffer_size = 0;
    config->receive_buffer_size = 0;

    // TCP Keepalive settings
    config->keepalive_enabled = 0;
    config->keepalive_idle = 7200;      // 2 hours default
    config->keepalive_interval = 75;    // 75 seconds default
    config->keepalive_count = 9;        // 9 probes default

    // Timeout settings - 0 means no timeout
    config->read_timeout = 0;
    config->write_timeout = 0;

    // Network interface - 0 means any interface
    config->interface = 0;

    // SSL settings
    config->ssl_enabled = 0;
    config->ssl_verifyhost = 1;
    config->ssl_verifypeer = 1;

    return 0;
}

// Extract configuration from Lua table
int tcpd_config_from_lua_table(lua_State *L, int table_index, tcpd_config_t *config) {
    if (!L || !config) return -1;

    // Initialize with defaults first
    tcpd_config_set_defaults(config);

    // Buffer settings
    lua_getfield(L, table_index, "send_buffer_size");
    if (lua_isnumber(L, -1)) {
        config->send_buffer_size = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "receive_buffer_size");
    if (lua_isnumber(L, -1)) {
        config->receive_buffer_size = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    // TCP Keepalive settings
    lua_getfield(L, table_index, "keepalive");
    config->keepalive_enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (config->keepalive_enabled) {
        lua_getfield(L, table_index, "keepalive_idle");
        if (lua_isnumber(L, -1)) {
            config->keepalive_idle = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, table_index, "keepalive_interval");
        if (lua_isnumber(L, -1)) {
            config->keepalive_interval = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, table_index, "keepalive_count");
        if (lua_isnumber(L, -1)) {
            config->keepalive_count = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    // Timeout settings
    lua_getfield(L, table_index, "read_timeout");
    if (lua_isnumber(L, -1)) {
        config->read_timeout = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "write_timeout");
    if (lua_isnumber(L, -1)) {
        config->write_timeout = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);

    // Network interface
    lua_getfield(L, table_index, "interface");
    if (lua_isstring(L, -1)) {
        const char *interface = lua_tostring(L, -1);
        config->interface = if_nametoindex(interface);
    }
    lua_pop(L, 1);

    // SSL settings
    lua_getfield(L, table_index, "ssl");
    config->ssl_enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (config->ssl_enabled) {
        lua_getfield(L, table_index, "ssl_verifyhost");
        if (lua_isnumber(L, -1)) {
            config->ssl_verifyhost = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, table_index, "ssl_verifypeer");
        if (lua_isnumber(L, -1)) {
            config->ssl_verifypeer = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    return 0;
}

// Helper function to set TCP keepalive options on a socket
int tcpd_config_apply_keepalive(const tcpd_config_t *config, evutil_socket_t fd) {
    if (!config || !config->keepalive_enabled) {
        return 0;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) != 0) {
        return -1;
    }

#ifdef TCP_KEEPIDLE
    if (config->keepalive_idle > 0) {
        if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &config->keepalive_idle, sizeof(config->keepalive_idle)) != 0) {
            return -1;
        }
    }
#endif

#ifdef TCP_KEEPINTVL
    if (config->keepalive_interval > 0) {
        if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &config->keepalive_interval, sizeof(config->keepalive_interval)) != 0) {
            return -1;
        }
    }
#endif

#ifdef TCP_KEEPCNT
    if (config->keepalive_count > 0) {
        if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &config->keepalive_count, sizeof(config->keepalive_count)) != 0) {
            return -1;
        }
    }
#endif

    return 0;
}

// Apply buffer settings to a bufferevent and socket
int tcpd_config_apply_buffers(const tcpd_config_t *config, struct bufferevent *bev, evutil_socket_t fd) {
    if (!config || !bev) return -1;

    if (config->send_buffer_size > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &config->send_buffer_size, sizeof(config->send_buffer_size)) != 0) {
            return -1;
        }
    }

    if (config->receive_buffer_size > 0) {
        bufferevent_setwatermark(bev, EV_READ, 0, config->receive_buffer_size);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &config->receive_buffer_size, sizeof(config->receive_buffer_size)) != 0) {
            return -1;
        }
    }

    return 0;
}

// Apply timeout settings to a bufferevent
int tcpd_config_apply_timeouts(const tcpd_config_t *config, struct bufferevent *bev) {
    if (!config || !bev) return -1;

    struct timeval read_tv, write_tv;
    struct timeval *read_tvp = NULL, *write_tvp = NULL;

    if (config->read_timeout > 0) {
        read_tv.tv_sec = (long)config->read_timeout;
        read_tv.tv_usec = (long)((config->read_timeout - read_tv.tv_sec) * 1000000);
        read_tvp = &read_tv;
    }

    if (config->write_timeout > 0) {
        write_tv.tv_sec = (long)config->write_timeout;
        write_tv.tv_usec = (long)((config->write_timeout - write_tv.tv_sec) * 1000000);
        write_tvp = &write_tv;
    }

    if (read_tvp || write_tvp) {
        bufferevent_set_timeouts(bev, read_tvp, write_tvp);
    }

    return 0;
}

// Apply network interface binding
int tcpd_config_apply_interface(const tcpd_config_t *config, evutil_socket_t fd) {
    if (!config || config->interface == 0) return 0;

#ifdef IP_BOUND_IF
    if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &config->interface, sizeof(config->interface)) != 0) {
        return -1;
    }
#endif

    return 0;
}