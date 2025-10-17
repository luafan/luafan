#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <net/if.h>

// Initialize UDP configuration with defaults
int udpd_config_init(udpd_config_t *config) {
    if (!config) return -1;

    memset(config, 0, sizeof(udpd_config_t));

    // Initialize base TCP configuration
    tcpd_config_init(&config->base);

    return udpd_config_set_defaults(config);
}

// Set default values for UDP configuration
int udpd_config_set_defaults(udpd_config_t *config) {
    if (!config) return -1;

    // Set base TCP defaults
    tcpd_config_set_defaults(&config->base);

    // UDP-specific defaults
    config->broadcast_enabled = 0;
    config->multicast_enabled = 0;
    config->multicast_group = NULL;
    config->multicast_ttl = 1;
    config->reuse_addr = 1;
    config->reuse_port = 0;

    // Set UDP-specific buffer defaults in base config if not already set
    if (config->base.send_buffer_size == 0) {
        config->base.send_buffer_size = UDPD_DEFAULT_BUFFER_SIZE;
    }
    if (config->base.receive_buffer_size == 0) {
        config->base.receive_buffer_size = UDPD_DEFAULT_BUFFER_SIZE;
    }

    return 0;
}

// Extract UDP configuration from Lua table
int udpd_config_from_lua_table(lua_State *L, int table_index, udpd_config_t *config) {
    if (!L || !config) return -1;

    // Initialize structure first
    memset(config, 0, sizeof(udpd_config_t));

    // Initialize base TCP configuration first
    tcpd_config_init(&config->base);

    // Extract base TCP configuration (reuse existing logic)
    tcpd_config_from_lua_table(L, table_index, &config->base);

    // Set UDP-specific defaults (non-buffer fields)
    config->broadcast_enabled = 0;
    config->multicast_enabled = 0;
    config->multicast_group = NULL;
    config->multicast_ttl = 1;
    config->reuse_addr = 1;
    config->reuse_port = 0;

    // Set UDP-specific buffer defaults only if not already set by Lua table
    if (config->base.send_buffer_size == 0) {
        config->base.send_buffer_size = UDPD_DEFAULT_BUFFER_SIZE;
    }
    if (config->base.receive_buffer_size == 0) {
        config->base.receive_buffer_size = UDPD_DEFAULT_BUFFER_SIZE;
    }

    // Extract UDP-specific configuration
    lua_getfield(L, table_index, "broadcast");
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        config->broadcast_enabled = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "multicast");
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        config->multicast_enabled = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "multicast_group");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *group = lua_tostring(L, -1);
        config->multicast_group = strdup(group);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "multicast_ttl");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        config->multicast_ttl = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "reuse_addr");
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        config->reuse_addr = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "reuse_port");
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        config->reuse_port = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    // Buffer sizes are handled by tcpd_config_from_lua_table() in base config
    // No separate UDP buffer handling needed - eliminates field conflicts

    return 0;
}

// Apply socket-level options
int udpd_config_apply_socket_options(const udpd_config_t *config, evutil_socket_t fd) {
    if (!config || fd < 0) return -1;

    // Apply reuse address
    if (config->reuse_addr) {
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            return -1;
        }
    }

    // Apply reuse port (if supported)
#ifdef SO_REUSEPORT
    if (config->reuse_port) {
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            return -1;
        }
    }
#endif

    // Apply broadcast option
    if (config->broadcast_enabled) {
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) < 0) {
            return -1;
        }
    }

    // Apply buffer sizes from base config
    if (config->base.receive_buffer_size > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                      &config->base.receive_buffer_size, sizeof(config->base.receive_buffer_size)) < 0) {
            // Non-fatal, continue
        }
    }

    if (config->base.send_buffer_size > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                      &config->base.send_buffer_size, sizeof(config->base.send_buffer_size)) < 0) {
            // Non-fatal, continue
        }
    }

    // Apply interface binding (similar to tcpd)
    if (config->base.interface > 0) {
#ifdef IP_BOUND_IF
        if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF,
                      &config->base.interface, sizeof(config->base.interface)) < 0) {
            // Non-fatal, continue
        }
#endif
    }

    return 0;
}

// Apply bind-specific options
int udpd_config_apply_bind_options(const udpd_config_t *config, evutil_socket_t fd) {
    if (!config || fd < 0) return -1;

    // Apply multicast options if enabled
    if (config->multicast_enabled && config->multicast_group) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));

        // Convert multicast group address
        if (inet_aton(config->multicast_group, &mreq.imr_multiaddr) == 0) {
            return -1;
        }

        mreq.imr_interface.s_addr = INADDR_ANY;

        // Join multicast group
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            return -1;
        }

        // Set multicast TTL
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                      &config->multicast_ttl, sizeof(config->multicast_ttl)) < 0) {
            return -1;
        }
    }

    return 0;
}

// Clean up configuration resources
void udpd_config_cleanup(udpd_config_t *config) {
    if (!config) return;

    // Clean up allocated strings
    if (config->multicast_group) {
        free(config->multicast_group);
        config->multicast_group = NULL;
    }

    // No cleanup needed for base config (it doesn't allocate)
    memset(config, 0, sizeof(udpd_config_t));
}

// Validate UDP configuration
int udpd_config_validate(const udpd_config_t *config) {
    if (!config) return -1;

    // Validate buffer sizes from base config
    if (config->base.receive_buffer_size <= 0 || config->base.receive_buffer_size > UDPD_MAX_PACKET_SIZE) {
        return -1;
    }

    if (config->base.send_buffer_size <= 0 || config->base.send_buffer_size > UDPD_MAX_PACKET_SIZE) {
        return -1;
    }

    // Validate multicast settings
    if (config->multicast_enabled) {
        if (!config->multicast_group) {
            return -1;
        }

        if (config->multicast_ttl < 0 || config->multicast_ttl > 255) {
            return -1;
        }

        // Validate multicast group address format
        struct in_addr addr;
        if (inet_aton(config->multicast_group, &addr) == 0) {
            return -1;
        }

        // Check if it's a valid multicast address (224.0.0.0 to 239.255.255.255)
        uint32_t ip = ntohl(addr.s_addr);
        if ((ip & 0xF0000000) != 0xE0000000) {
            return -1;
        }
    }

    return 0;
}

// Copy configuration
int udpd_config_copy(udpd_config_t *dest, const udpd_config_t *src) {
    if (!dest || !src) return -1;

    // Copy base configuration
    dest->base = src->base;

    // Copy UDP-specific fields (buffer sizes are in base config)
    dest->broadcast_enabled = src->broadcast_enabled;
    dest->multicast_enabled = src->multicast_enabled;
    dest->multicast_ttl = src->multicast_ttl;
    dest->reuse_addr = src->reuse_addr;
    dest->reuse_port = src->reuse_port;

    // Copy allocated strings
    if (src->multicast_group) {
        dest->multicast_group = strdup(src->multicast_group);
        if (!dest->multicast_group) {
            return -1;
        }
    } else {
        dest->multicast_group = NULL;
    }

    return 0;
}