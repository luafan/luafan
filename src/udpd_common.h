#ifndef UDPD_COMMON_H
#define UDPD_COMMON_H

#include "utlua.h"
#include "tcpd_common.h"  // Reuse common TCP/UDP configurations
#include <event2/event.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Forward declarations
struct udpd_base_conn;
struct udpd_dest;
struct udpd_dns_request;

// UDP connection states
typedef enum {
    UDPD_CONN_DISCONNECTED = 0,
    UDPD_CONN_BINDING,
    UDPD_CONN_BOUND,
    UDPD_CONN_RESOLVING,
    UDPD_CONN_READY,
    UDPD_CONN_ERROR
} udpd_conn_state_t;

// UDP connection types
typedef enum {
    UDPD_CONN_TYPE_CLIENT = 0,
    UDPD_CONN_TYPE_SERVER,
    UDPD_CONN_TYPE_BROADCAST
} udpd_conn_type_t;

// UDP-specific configuration (extends tcpd_config_t)
typedef struct udpd_config {
    // Base TCP configuration (reuse buffer settings, timeouts, etc.)
    tcpd_config_t base;

    // UDP-specific settings
    int broadcast_enabled;
    int multicast_enabled;
    char *multicast_group;
    int multicast_ttl;
    int reuse_addr;
    int reuse_port;

    // Note: UDP uses base.send_buffer_size and base.receive_buffer_size
    // No separate UDP buffer sizes needed - eliminates redundancy
} udpd_config_t;

// Base UDP connection structure
typedef struct udpd_base_conn {
    // Core connection data
    evutil_socket_t socket_fd;
    udpd_conn_state_t state;
    udpd_conn_type_t type;

    // Lua state and callbacks
    lua_State *mainthread;
    int selfRef;
    int _ref_;  // Generic Lua reference for REF_STATE macros
    int onReadRef;
    int onSendReadyRef;

    // Configuration
    udpd_config_t config;

    // Network information
    char *host;
    char *bind_host;
    int port;
    int bind_port;
    int interface;

    // Address information
    struct sockaddr_storage addr;      // Target address
    socklen_t addrlen;
    struct sockaddr_storage bind_addr; // Bind address
    socklen_t bind_addrlen;

    // Event handling
    struct event *read_ev;
    struct event *write_ev;

    // DNS resolution
    struct udpd_dns_request *dns_request;
} udpd_base_conn_t;

// Destination address structure
typedef struct udpd_dest {
    struct sockaddr_storage addr;
    socklen_t addrlen;
    char *host;  // Optional hostname for reference
    int port;
} udpd_dest_t;

// DNS request structure
typedef struct udpd_dns_request {
    char *hostname;
    int port;
    lua_State *mainthread;
    int _ref_;
    int yielded;

    // Callback data
    void (*callback)(int errcode, struct evutil_addrinfo *addr, void *ctx);
    void *callback_ctx;
} udpd_dns_request_t;

// Configuration functions
int udpd_config_init(udpd_config_t *config);
int udpd_config_set_defaults(udpd_config_t *config);
int udpd_config_from_lua_table(lua_State *L, int table_index, udpd_config_t *config);
int udpd_config_apply_socket_options(const udpd_config_t *config, evutil_socket_t fd);
int udpd_config_apply_bind_options(const udpd_config_t *config, evutil_socket_t fd);
void udpd_config_cleanup(udpd_config_t *config);
int udpd_config_validate(const udpd_config_t *config);
int udpd_config_copy(udpd_config_t *dest, const udpd_config_t *src);

// Base connection functions
int udpd_base_conn_init(udpd_base_conn_t *conn, udpd_conn_type_t type, lua_State *L);
void udpd_base_conn_cleanup(udpd_base_conn_t *conn);
int udpd_base_conn_set_callbacks(udpd_base_conn_t *conn, lua_State *L, int table_index);
int udpd_base_conn_create_socket(udpd_base_conn_t *conn);
int udpd_base_conn_bind(udpd_base_conn_t *conn);
int udpd_base_conn_setup_events(udpd_base_conn_t *conn);
int udpd_base_conn_request_send_ready(udpd_base_conn_t *conn);

// Event handling functions
void udpd_common_readcb(evutil_socket_t fd, short what, void *ctx);
void udpd_common_writecb(evutil_socket_t fd, short what, void *ctx);
void udpd_connection_cleanup_on_disconnect(udpd_base_conn_t *conn);
void udpd_process_received_data(udpd_base_conn_t *conn, const char *data, size_t len,
                               const struct sockaddr_storage *from_addr, socklen_t from_len);
void udpd_handle_read_error(udpd_base_conn_t *conn, int error_code);

// Destination management functions
udpd_dest_t* udpd_dest_create(const struct sockaddr *addr, socklen_t addrlen);
udpd_dest_t* udpd_dest_create_from_string(const char *host, int port);
void udpd_dest_cleanup(udpd_dest_t *dest);
int udpd_dest_equal(const udpd_dest_t *dest1, const udpd_dest_t *dest2);
const char* udpd_dest_get_host_string(const udpd_dest_t *dest);
int udpd_dest_get_port(const udpd_dest_t *dest);
char* udpd_dest_to_string(const udpd_dest_t *dest);
udpd_dest_t* udpd_dest_copy(const udpd_dest_t *src);
int udpd_dest_is_multicast(const udpd_dest_t *dest);
int udpd_dest_is_broadcast(const udpd_dest_t *dest);
int udpd_dest_is_loopback(const udpd_dest_t *dest);
udpd_dest_t* udpd_dest_from_lua(lua_State *L, int index);
void udpd_dest_setup_metatable(lua_State *L);

// DNS resolution functions
udpd_dns_request_t* udpd_dns_request_create(const char *hostname, int port);
void udpd_dns_request_cleanup(udpd_dns_request_t *request);
int udpd_dns_resolve_async(udpd_dns_request_t *request,
                          void (*callback)(int, struct evutil_addrinfo*, void*),
                          void *ctx);
int udpd_dns_resolve_sync(const char *hostname, int port,
                         struct sockaddr_storage *addr, socklen_t *addrlen);
int udpd_dns_resolve_for_connection(udpd_base_conn_t *conn);
int udpd_dns_resolve_for_destination(const char *hostname, int port, lua_State *L);

// Utility functions
int udpd_socket_set_nonblock(evutil_socket_t fd);
int udpd_socket_set_reuse_addr(evutil_socket_t fd);
int udpd_socket_set_broadcast(evutil_socket_t fd);
int udpd_socket_bind_interface(evutil_socket_t fd, int interface);
int udpd_get_socket_port(evutil_socket_t fd);
int udpd_extract_interface_from_lua(lua_State *L, int table_index);
int udpd_validate_packet_size(size_t size);
int udpd_is_address_family_supported(int family);
int udpd_is_ip_address(const char *str);
char* udpd_format_connection_info(const udpd_base_conn_t *conn);
char* udpd_sockaddr_to_string(const struct sockaddr *addr, socklen_t addrlen);
int udpd_create_address_from_string(const char *host, int port,
                                   struct sockaddr_storage *addr, socklen_t *addrlen);

// Error handling (reuse tcpd error system)
tcpd_error_t udpd_error_from_socket_error(int error_code);
tcpd_error_t udpd_error_from_dns_error(int dns_error);
tcpd_error_t udpd_error_bind_failed(const char *host, int port);

// Constants
#define UDPD_DEFAULT_BUFFER_SIZE 2048
#define UDPD_MAX_PACKET_SIZE 65507  // Maximum UDP packet size
#define LUA_UDPD_CONNECTION_TYPE "UDPD_CONNECTION_TYPE"
#define LUA_UDPD_DEST_TYPE "LUA_UDPD_DEST_TYPE"

#endif // UDPD_COMMON_H