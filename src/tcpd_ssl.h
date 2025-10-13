#ifndef TCPD_SSL_H
#define TCPD_SSL_H

#include "tcpd_common.h"

#if FAN_HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>

// SSL context structure
typedef struct tcpd_ssl_context {
    SSL_CTX *ssl_ctx;
    char *cache_key;
    int retain_count;
    int configured;  // Flag to track if context is already configured

    // SSL configuration
    char *cert_file;
    char *key_file;
    char *ca_info;
    char *ca_path;
    char *pkcs12_path;
    char *pkcs12_password;

    // Verification settings
    int verify_peer;
    int verify_host;
} tcpd_ssl_context_t;


// SSL context management
tcpd_ssl_context_t* tcpd_ssl_context_create(lua_State *L);
int tcpd_ssl_context_configure(tcpd_ssl_context_t *ctx, lua_State *L, int table_index);
tcpd_ssl_context_t* tcpd_ssl_context_get_or_create(lua_State *L, const char *cache_key);
void tcpd_ssl_context_retain(tcpd_ssl_context_t *ctx);
void tcpd_ssl_context_release(tcpd_ssl_context_t *ctx, lua_State *L);

// Cache key generation
char* tcpd_ssl_generate_cache_key_from_table(lua_State *L, int table_index);


// Client connection SSL error handling
void tcpd_ssl_set_client_error(tcpd_client_conn_t *client, const char *error);
const char* tcpd_ssl_get_client_error(tcpd_client_conn_t *client);

// SSL bufferevent creation
struct bufferevent* tcpd_ssl_create_client_bufferevent(
    struct event_base *base,
    tcpd_ssl_context_t *ctx,
    const char *hostname,
    tcpd_client_conn_t *client
);

struct bufferevent* tcpd_ssl_create_server_bufferevent(
    struct event_base *base,
    evutil_socket_t fd,
    tcpd_ssl_context_t *ctx
);

// SSL certificate validation
typedef enum {
    TCPD_SSL_HOSTNAME_MATCH_FOUND = 0,
    TCPD_SSL_HOSTNAME_MATCH_NOT_FOUND,
    TCPD_SSL_HOSTNAME_NO_SAN_PRESENT,
    TCPD_SSL_HOSTNAME_MALFORMED_CERT,
    TCPD_SSL_HOSTNAME_ERROR
} tcpd_ssl_hostname_validation_result_t;

tcpd_ssl_hostname_validation_result_t tcpd_ssl_validate_hostname(const char *hostname, X509 *cert);

// SSL utility functions
int tcpd_ssl_load_cert_and_key(SSL_CTX *ctx, const char *cert_file, const char *key_file);
int tcpd_ssl_load_pkcs12(SSL_CTX *ctx, const char *p12_path, const char *password);
int tcpd_ssl_setup_ca_verification(SSL_CTX *ctx, const char *ca_info, const char *ca_path);

// SSL verification callbacks
int tcpd_ssl_verify_callback(int preverify_ok, X509_STORE_CTX *ctx);
int tcpd_ssl_cert_verify_callback(X509_STORE_CTX *ctx, void *arg);

// SSL error handling
void tcpd_ssl_print_errors(void);
char* tcpd_ssl_get_error_string(void);

// SSL context metatable registration
void tcpd_ssl_register_metatable(lua_State *L);

#else
// Stub definitions when SSL is not available
typedef struct tcpd_ssl_context {
    int dummy;
} tcpd_ssl_context_t;

// Stub functions
static inline tcpd_ssl_context_t* tcpd_ssl_context_create(lua_State *L) { (void)L; return NULL; }
static inline void tcpd_ssl_context_release(tcpd_ssl_context_t *ctx, lua_State *L) { (void)ctx; (void)L; }
static inline const char* tcpd_ssl_get_client_error(tcpd_client_conn_t *client) { (void)client; return "SSL not supported"; }
static inline void tcpd_ssl_register_metatable(lua_State *L) { (void)L; }

#endif // FAN_HAS_OPENSSL

#endif // TCPD_SSL_H
