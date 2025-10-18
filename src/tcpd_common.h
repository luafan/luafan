#ifndef TCPD_COMMON_H
#define TCPD_COMMON_H

#include "utlua.h"
#include <event2/bufferevent.h>

// Forward declarations
struct tcpd_config;
struct tcpd_ssl_context;
typedef struct tcpd_ssl_context tcpd_ssl_context_t;
struct tcpd_base_conn;

// Connection states
typedef enum {
    TCPD_CONN_DISCONNECTED = 0,
    TCPD_CONN_CONNECTING,
    TCPD_CONN_CONNECTED,
    TCPD_CONN_ERROR
} tcpd_conn_state_t;

// Connection types
typedef enum {
    TCPD_CONN_TYPE_CLIENT = 0,
    TCPD_CONN_TYPE_ACCEPT,
    TCPD_CONN_TYPE_SERVER
} tcpd_conn_type_t;

// TCP configuration structure
typedef struct tcpd_config {
    // Buffer settings
    int send_buffer_size;
    int receive_buffer_size;

    // TCP Keepalive settings
    int keepalive_enabled;
    int keepalive_idle;
    int keepalive_interval;
    int keepalive_count;

    // Timeout settings
    lua_Number read_timeout;
    lua_Number write_timeout;

    // Network interface
    int interface;

    // SSL settings
    int ssl_enabled;
    int ssl_verifyhost;
    int ssl_verifypeer;

    // Callback behavior settings
    int callback_self_first;  // When enabled, pass connection object as first parameter to all callbacks
} tcpd_config_t;

// Base connection structure - common fields for all connection types
typedef struct tcpd_base_conn {
    // Core connection data
    struct bufferevent *buf;
    tcpd_conn_state_t state;
    tcpd_conn_type_t type;

    // Lua state and callbacks
    lua_State *mainthread;
    int onReadRef;
    int onSendReadyRef;
    int onDisconnectedRef;
    int onConnectedRef;  // For client connections
    int selfRef;         // Self-reference for callback_self_first mode

    // Configuration
    tcpd_config_t config;

    // SSL context (if SSL is enabled)
    tcpd_ssl_context_t *ssl_ctx;

    // Connection info
    char *host;
    int port;
    char ip[INET6_ADDRSTRLEN];
} tcpd_base_conn_t;

// Extended structures using the base connection
typedef struct {
    tcpd_base_conn_t base;
    // Note: onConnectedRef is already defined in base structure

    // SSL connection data (integrated from tcpd_ssl_conn_t)
#if FAN_HAS_OPENSSL
    SSL *ssl;
    char *ssl_host;
    char *ssl_error_message;
#endif
} tcpd_client_conn_t;

// Function prototypes for common operations
int tcpd_config_init(tcpd_config_t *config);
int tcpd_config_set_defaults(tcpd_config_t *config);
int tcpd_config_from_lua_table(lua_State *L, int table_index, tcpd_config_t *config);

int tcpd_base_conn_init(tcpd_base_conn_t *conn, tcpd_conn_type_t type, lua_State *L);
void tcpd_base_conn_cleanup(tcpd_base_conn_t *conn);
int tcpd_base_conn_set_callbacks(tcpd_base_conn_t *conn, lua_State *L, int table_index);

// Error handling
typedef enum {
    TCPD_ERROR_NONE = 0,
    TCPD_ERROR_TIMEOUT,
    TCPD_ERROR_CONNECTION_RESET,
    TCPD_ERROR_DNS_FAILED,
    TCPD_ERROR_SSL_ERROR,
    TCPD_ERROR_EOF,
    TCPD_ERROR_UNKNOWN
} tcpd_error_type_t;

typedef struct {
    tcpd_error_type_t type;
    char *message;
    int system_error;
} tcpd_error_t;

const char* tcpd_error_to_string(tcpd_error_t *error);
void tcpd_error_cleanup(tcpd_error_t *error);

// Error creation functions
tcpd_error_t tcpd_error_from_socket_error(int error_code);
tcpd_error_t tcpd_error_timeout(short events);
tcpd_error_t tcpd_error_dns(int dns_error);
tcpd_error_t tcpd_error_eof(tcpd_conn_type_t conn_type);
tcpd_error_t tcpd_error_ssl(const char *ssl_error_msg);


// Additional config functions
int tcpd_config_apply_keepalive(const tcpd_config_t *config, evutil_socket_t fd);
int tcpd_config_apply_buffers(const tcpd_config_t *config, struct bufferevent *bev, evutil_socket_t fd);
int tcpd_config_apply_timeouts(const tcpd_config_t *config, struct bufferevent *bev);
int tcpd_config_apply_interface(const tcpd_config_t *config, evutil_socket_t fd);

// Event handling functions
void tcpd_common_readcb(struct bufferevent *bev, void *ctx);
void tcpd_common_writecb(struct bufferevent *bev, void *ctx);
void tcpd_common_eventcb(struct bufferevent *bev, short events, void *ctx);
void tcpd_shutdown_bufferevent(struct bufferevent *bev);
void tcpd_connection_cleanup_on_disconnect(tcpd_base_conn_t *conn);

// Helper function to push connection object to Lua stack
void tcpd_push_connection_object(lua_State *co, tcpd_base_conn_t *conn);

// SSL functions (forward declarations)
#if FAN_HAS_OPENSSL
struct tcpd_ssl_context;
void tcpd_ssl_init(void);
#endif

#endif // TCPD_COMMON_H
