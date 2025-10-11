#include "tcpd_ssl.h"
#include <string.h>
#include <stdlib.h>

#if FAN_HAS_OPENSSL

static int ssl_conn_index = 0;

// Initialize SSL library (call once at startup)
void tcpd_ssl_init(void) {
    static int initialized = 0;
    if (!initialized) {
        ssl_conn_index = SSL_get_ex_new_index(0, "tcpd_ssl_conn_index", NULL, NULL, NULL);
        initialized = 1;
    }
}

// Create a new SSL context
tcpd_ssl_context_t* tcpd_ssl_context_create(void) {
    tcpd_ssl_context_t *ctx = malloc(sizeof(tcpd_ssl_context_t));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(tcpd_ssl_context_t));
    ctx->retain_count = 1;
    ctx->verify_peer = 1;
    ctx->verify_host = 1;

    return ctx;
}

// Configure SSL context from Lua table
int tcpd_ssl_context_configure(tcpd_ssl_context_t *ctx, lua_State *L, int table_index) {
    if (!ctx || !L) return -1;

    // Extract SSL configuration from Lua table
    lua_getfield(L, table_index, "cert");
    if (lua_isstring(L, -1)) {
        const char *cert = lua_tostring(L, -1);
        ctx->cert_file = strdup(cert);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "key");
    if (lua_isstring(L, -1)) {
        const char *key = lua_tostring(L, -1);
        ctx->key_file = strdup(key);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "cainfo");
    if (lua_isstring(L, -1)) {
        const char *cainfo = lua_tostring(L, -1);
        ctx->ca_info = strdup(cainfo);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "capath");
    if (lua_isstring(L, -1)) {
        const char *capath = lua_tostring(L, -1);
        ctx->ca_path = strdup(capath);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "pkcs12.path");
    if (lua_isstring(L, -1)) {
        const char *p12path = lua_tostring(L, -1);
        ctx->pkcs12_path = strdup(p12path);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "pkcs12.password");
    if (lua_isstring(L, -1)) {
        const char *p12password = lua_tostring(L, -1);
        ctx->pkcs12_password = strdup(p12password);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "ssl_verifyhost");
    if (lua_isnumber(L, -1)) {
        ctx->verify_host = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "ssl_verifypeer");
    if (lua_isnumber(L, -1)) {
        ctx->verify_peer = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    // Create SSL_CTX
#if OPENSSL_VERSION_NUMBER < 0x1010000fL
    ctx->ssl_ctx = SSL_CTX_new(SSLv23_method());
#else
    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
#endif

    if (!ctx->ssl_ctx) {
        return -1;
    }

    // Configure SSL_CTX
    if (ctx->ca_info || ctx->ca_path) {
        tcpd_ssl_setup_ca_verification(ctx->ssl_ctx, ctx->ca_info, ctx->ca_path);
    } else {
        // Use default cert.pem if no CA specified
        tcpd_ssl_setup_ca_verification(ctx->ssl_ctx, "cert.pem", NULL);
    }

    // Set up certificate and key if specified
    if (ctx->cert_file && ctx->key_file) {
        tcpd_ssl_load_cert_and_key(ctx->ssl_ctx, ctx->cert_file, ctx->key_file);
    }

    // Load PKCS12 if specified
    if (ctx->pkcs12_path) {
        tcpd_ssl_load_pkcs12(ctx->ssl_ctx, ctx->pkcs12_path, ctx->pkcs12_password);
    }

    // Set SSL options
#ifdef SSL_MODE_RELEASE_BUFFERS
    SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
#endif
    SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_NO_COMPRESSION);

    // Set verification mode
#if OPENSSL_VERSION_NUMBER < 0x1010000fL
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_cert_verify_callback(ctx->ssl_ctx, tcpd_ssl_cert_verify_callback, NULL);
#else
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, tcpd_ssl_verify_callback);
#endif

    return 0;
}

// Generate cache key for SSL context
static char* tcpd_ssl_generate_cache_key(tcpd_ssl_context_t *ctx) {
    BYTEARRAY ba = {0};
    bytearray_alloc(&ba, 1024);
    bytearray_writebuffer(&ba, "SSL_CTX:", 8);

    if (ctx->ca_info) {
        bytearray_writebuffer(&ba, ctx->ca_info, strlen(ctx->ca_info));
    }
    if (ctx->ca_path) {
        bytearray_writebuffer(&ba, ctx->ca_path, strlen(ctx->ca_path));
    }
    if (ctx->pkcs12_path) {
        bytearray_writebuffer(&ba, ctx->pkcs12_path, strlen(ctx->pkcs12_path));
    }
    if (ctx->pkcs12_password) {
        bytearray_writebuffer(&ba, ctx->pkcs12_password, strlen(ctx->pkcs12_password));
    }

    bytearray_write8(&ba, 0);
    bytearray_read_ready(&ba);

    char *key = strdup((const char *)ba.buffer);
    bytearray_dealloc(&ba);

    return key;
}

// Get or create SSL context (with caching)
tcpd_ssl_context_t* tcpd_ssl_context_get_or_create(const char *cache_key) {
    // This would typically use a global cache
    // For now, we'll create a new context each time
    return tcpd_ssl_context_create();
}

// Retain SSL context
void tcpd_ssl_context_retain(tcpd_ssl_context_t *ctx) {
    if (ctx) {
        ctx->retain_count++;
    }
}

// Release SSL context
void tcpd_ssl_context_release(tcpd_ssl_context_t *ctx) {
    if (!ctx) return;

    ctx->retain_count--;
    if (ctx->retain_count <= 0) {
        // Clean up SSL context
        if (ctx->ssl_ctx) {
            SSL_CTX_free(ctx->ssl_ctx);
        }

        // Free allocated strings
        free(ctx->cache_key);
        free(ctx->cert_file);
        free(ctx->key_file);
        free(ctx->ca_info);
        free(ctx->ca_path);
        free(ctx->pkcs12_path);
        free(ctx->pkcs12_password);

        free(ctx);
    }
}

// Initialize SSL connection
int tcpd_ssl_connection_init(tcpd_ssl_conn_t *ssl_conn, tcpd_ssl_context_t *ctx, const char *hostname) {
    if (!ssl_conn || !ctx || !ctx->ssl_ctx) return -1;

    memset(ssl_conn, 0, sizeof(tcpd_ssl_conn_t));

    ssl_conn->ssl = SSL_new(ctx->ssl_ctx);
    if (!ssl_conn->ssl) return -1;

    SSL_set_ex_data(ssl_conn->ssl, ssl_conn_index, ssl_conn);

    if (hostname) {
        ssl_conn->ssl_host = strdup(hostname);

#if OPENSSL_VERSION_NUMBER >= 0x1010000fL
        if (ctx->verify_host) {
            SSL_set_hostflags(ssl_conn->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (!SSL_set1_host(ssl_conn->ssl, hostname)) {
                printf("SSL_set1_host '%s' failed!\n", hostname);
            }
        }

        if (ctx->verify_peer) {
            SSL_set_verify(ssl_conn->ssl, SSL_VERIFY_PEER, NULL);
        } else {
            SSL_set_verify(ssl_conn->ssl, SSL_VERIFY_NONE, NULL);
        }
#endif

        SSL_set_tlsext_host_name(ssl_conn->ssl, hostname);
    }

    return 0;
}

// Clean up SSL connection
void tcpd_ssl_connection_cleanup(tcpd_ssl_conn_t *ssl_conn) {
    if (!ssl_conn) return;

    free(ssl_conn->ssl_host);
    free(ssl_conn->error_message);

    // SSL object is freed by bufferevent
    memset(ssl_conn, 0, sizeof(tcpd_ssl_conn_t));
}

// Get SSL error message
const char* tcpd_ssl_get_error(tcpd_ssl_conn_t *ssl_conn) {
    return ssl_conn ? ssl_conn->error_message : NULL;
}

// Set SSL error message
void tcpd_ssl_set_error(tcpd_ssl_conn_t *ssl_conn, const char *error) {
    if (!ssl_conn) return;

    free(ssl_conn->error_message);
    ssl_conn->error_message = error ? strdup(error) : NULL;
}

// Create client SSL bufferevent
struct bufferevent* tcpd_ssl_create_client_bufferevent(
    struct event_base *base,
    tcpd_ssl_context_t *ctx,
    const char *hostname) {

    if (!base || !ctx || !ctx->ssl_ctx) return NULL;

    tcpd_ssl_conn_t ssl_conn;
    if (tcpd_ssl_connection_init(&ssl_conn, ctx, hostname) != 0) {
        return NULL;
    }

    struct bufferevent *bev = bufferevent_openssl_socket_new(
        base, -1, ssl_conn.ssl, BUFFEREVENT_SSL_CONNECTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS
    );

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
    bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
#endif
#endif

    return bev;
}

// Create server SSL bufferevent
struct bufferevent* tcpd_ssl_create_server_bufferevent(
    struct event_base *base,
    evutil_socket_t fd,
    tcpd_ssl_context_t *ctx) {

    if (!base || !ctx || !ctx->ssl_ctx) return NULL;

    SSL *ssl = SSL_new(ctx->ssl_ctx);
    if (!ssl) return NULL;

    return bufferevent_openssl_socket_new(
        base, fd, ssl, BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS
    );
}

// Load certificate and key files
int tcpd_ssl_load_cert_and_key(SSL_CTX *ctx, const char *cert_file, const char *key_file) {
    if (!ctx || !cert_file || !key_file) return -1;

    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        return -1;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        return -1;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        return -1;
    }

    return 0;
}

// Load PKCS12 file
int tcpd_ssl_load_pkcs12(SSL_CTX *ctx, const char *p12_path, const char *password) {
    if (!ctx || !p12_path) return -1;

    FILE *fp = fopen(p12_path, "rb");
    if (!fp) return -1;

    PKCS12 *p12 = d2i_PKCS12_fp(fp, NULL);
    fclose(fp);

    if (!p12) return -1;

    EVP_PKEY *pkey = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *ca = NULL;

    int result = -1;
    if (PKCS12_parse(p12, password, &pkey, &cert, &ca)) {
        if (cert) {
            SSL_CTX_use_certificate(ctx, cert);
        }
        if (pkey) {
            SSL_CTX_use_PrivateKey(ctx, pkey);
        }
        if (ca) {
            for (int i = 0; i < sk_X509_num(ca); i++) {
                SSL_CTX_use_certificate(ctx, sk_X509_value(ca, i));
            }
        }
        result = 0;
    }

    // Cleanup
    PKCS12_free(p12);
    if (cert) X509_free(cert);
    if (pkey) EVP_PKEY_free(pkey);
    if (ca) sk_X509_pop_free(ca, X509_free);

    return result;
}

// Setup CA verification
int tcpd_ssl_setup_ca_verification(SSL_CTX *ctx, const char *ca_info, const char *ca_path) {
    if (!ctx) return -1;

    if (!SSL_CTX_load_verify_locations(ctx, ca_info, ca_path)) {
        printf("SSL_CTX_load_verify_locations failed: cainfo=%s capath=%s\n",
               ca_info ? ca_info : "NULL", ca_path ? ca_path : "NULL");
        return -1;
    }

    return 0;
}

// SSL certificate verification callback (for older OpenSSL)
int tcpd_ssl_cert_verify_callback(X509_STORE_CTX *ctx, void *arg) {
    (void)arg;  // Unused parameter

    int preverify_ok = X509_verify_cert(ctx);
    if (!preverify_ok) {
        SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
        if (ssl) {
            tcpd_ssl_conn_t *ssl_conn = SSL_get_ex_data(ssl, ssl_conn_index);
            int err = X509_STORE_CTX_get_error(ctx);
            if (ssl_conn) {
                tcpd_ssl_set_error(ssl_conn, X509_verify_cert_error_string(err));
            }
        }
    }

    return preverify_ok;
}

// SSL verification callback (for newer OpenSSL)
int tcpd_ssl_verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
    if (!preverify_ok) {
        SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
        tcpd_ssl_conn_t *ssl_conn = SSL_get_ex_data(ssl, ssl_conn_index);

        int err = X509_STORE_CTX_get_error(ctx);
        if (ssl_conn) {
            tcpd_ssl_set_error(ssl_conn, X509_verify_cert_error_string(err));
        }
    }

    return preverify_ok;
}

// Print SSL errors to stderr
void tcpd_ssl_print_errors(void) {
    ERR_print_errors_fp(stderr);
}

// Get SSL error as string
char* tcpd_ssl_get_error_string(void) {
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return NULL;

    ERR_print_errors(bio);

    char *data;
    long len = BIO_get_mem_data(bio, &data);
    char *result = len > 0 ? strndup(data, len) : NULL;

    BIO_free(bio);
    return result;
}

#endif // FAN_HAS_OPENSSL