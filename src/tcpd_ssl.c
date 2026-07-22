#include "tcpd_ssl.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

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

// SSL context metatable name
#define LUA_TCPD_SSL_CONTEXT_TYPE "<tcpd.ssl_context>"

// SSL context garbage collection
static int tcpd_ssl_context_gc(lua_State *L) {
    tcpd_ssl_context_t *ctx = luaL_checkudata(L, 1, LUA_TCPD_SSL_CONTEXT_TYPE);

    // Clean up SSL context
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
    }

    // Free allocated strings
    free(ctx->cache_key);
    free(ctx->cert_file);
    free(ctx->key_file);
    free(ctx->ca_info);
    free(ctx->ca_path);
    free(ctx->pkcs12_path);
    free(ctx->pkcs12_password);

    // Clear the strings to avoid double-free
    ctx->cache_key = NULL;
    ctx->cert_file = NULL;
    ctx->key_file = NULL;
    ctx->ca_info = NULL;
    ctx->ca_path = NULL;
    ctx->pkcs12_path = NULL;
    ctx->pkcs12_password = NULL;

    return 0;
}

// Create a new SSL context
tcpd_ssl_context_t* tcpd_ssl_context_create(lua_State *L) {
    if (!L) return NULL;

    tcpd_ssl_context_t *ctx = lua_newuserdata(L, sizeof(tcpd_ssl_context_t));
    memset(ctx, 0, sizeof(tcpd_ssl_context_t));
    atomic_init(&ctx->retain_count, 1);
    ctx->verify_peer = 1;
    ctx->verify_host = 1;

    // Set metatable for proper garbage collection
    luaL_getmetatable(L, LUA_TCPD_SSL_CONTEXT_TYPE);
    lua_setmetatable(L, -2);

    return ctx;
}

// Configure SSL context from Lua table
int tcpd_ssl_context_configure(tcpd_ssl_context_t *ctx, lua_State *L, int table_index) {
    if (!ctx || !L) return -1;

    // Skip configuration if already configured (from cached context)
    if (ctx->configured) {
        return 0;
    }

    // Extract SSL configuration from Lua table
    lua_getfield(L, table_index, "cert");
    if (lua_isstring(L, -1)) {
        const char *cert = lua_tostring(L, -1);
        ctx->cert_file = strdup(cert);
        if (!ctx->cert_file) {
            lua_pop(L, 1);
            return luaL_error(L, "memory allocation failed for cert_file");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "key");
    if (lua_isstring(L, -1)) {
        const char *key = lua_tostring(L, -1);
        ctx->key_file = strdup(key);
        if (!ctx->key_file) {
            lua_pop(L, 1);
            return luaL_error(L, "memory allocation failed for key_file");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "cainfo");
    if (lua_isstring(L, -1)) {
        const char *cainfo = lua_tostring(L, -1);
        ctx->ca_info = strdup(cainfo);
        if (!ctx->ca_info) {
            lua_pop(L, 1);
            return luaL_error(L, "memory allocation failed for ca_info");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "capath");
    if (lua_isstring(L, -1)) {
        const char *capath = lua_tostring(L, -1);
        ctx->ca_path = strdup(capath);
        if (!ctx->ca_path) {
            lua_pop(L, 1);
            return luaL_error(L, "memory allocation failed for ca_path");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "pkcs12.path");
    if (lua_isstring(L, -1)) {
        const char *p12path = lua_tostring(L, -1);
        ctx->pkcs12_path = strdup(p12path);
        if (!ctx->pkcs12_path) {
            lua_pop(L, 1);
            return luaL_error(L, "memory allocation failed for pkcs12_path");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "pkcs12.password");
    if (lua_isstring(L, -1)) {
        const char *p12password = lua_tostring(L, -1);
        ctx->pkcs12_password = strdup(p12password);
        if (!ctx->pkcs12_password) {
            lua_pop(L, 1);
            return luaL_error(L, "memory allocation failed for pkcs12_password");
        }
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
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    ctx->ssl_ctx = SSL_CTX_new_ex(NULL, NULL, TLS_method());
#elif OPENSSL_VERSION_NUMBER >= 0x1010000fL
    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
#else
    ctx->ssl_ctx = SSL_CTX_new(SSLv23_method());
#endif

    if (!ctx->ssl_ctx) {
        return -1;
    }

    // Configure SSL_CTX trust store for peer verify.
    if (ctx->ca_info || ctx->ca_path) {
        tcpd_ssl_setup_ca_verification(ctx->ssl_ctx, ctx->ca_info, ctx->ca_path);
    } else {
        // No explicit cainfo/capath: OpenSSL defaults + common bundle paths.
        tcpd_ssl_setup_default_ca_verification(ctx->ssl_ctx);
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

    // Mark as configured to prevent re-configuration
    ctx->configured = 1;

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
    bytearray_write8(&ba, 0x1F);
    if (ctx->ca_path) {
        bytearray_writebuffer(&ba, ctx->ca_path, strlen(ctx->ca_path));
    }
    bytearray_write8(&ba, 0x1F);
    if (ctx->pkcs12_path) {
        bytearray_writebuffer(&ba, ctx->pkcs12_path, strlen(ctx->pkcs12_path));
    }
    bytearray_write8(&ba, 0x1F);
    if (ctx->pkcs12_password) {
        bytearray_writebuffer(&ba, ctx->pkcs12_password, strlen(ctx->pkcs12_password));
    }

    bytearray_write8(&ba, 0);
    bytearray_read_ready(&ba);

    char *key = strdup((const char *)ba.buffer);
    bytearray_dealloc(&ba);

    return key;
}

// Generate cache key from Lua table (based on tcpd.c.txt logic)
char* tcpd_ssl_generate_cache_key_from_table(lua_State *L, int table_index) {
    if (!L) return NULL;

    BYTEARRAY ba = {0};
    bytearray_alloc(&ba, 1024);
    bytearray_writebuffer(&ba, "SSL_CTX:", 8);

    // Add cainfo to cache key
    lua_getfield(L, table_index, "cainfo");
    const char *cainfo = lua_tostring(L, -1);
    if (cainfo) {
        bytearray_writebuffer(&ba, cainfo, strlen(cainfo));
    } else {
        // Shared default-CA path set (see tcpd_ssl_setup_default_ca_verification)
        bytearray_writebuffer(&ba, "default_ca", 10);
    }
    lua_pop(L, 1);

    // Field separator to prevent key collisions
    bytearray_write8(&ba, 0x1F);

    // Add capath to cache key
    lua_getfield(L, table_index, "capath");
    const char *capath = lua_tostring(L, -1);
    if (capath) {
        bytearray_writebuffer(&ba, capath, strlen(capath));
    }
    lua_pop(L, 1);

    bytearray_write8(&ba, 0x1F);

    // Add PKCS12 path to cache key
    lua_getfield(L, table_index, "pkcs12.path");
    const char *p12path = lua_tostring(L, -1);
    if (p12path) {
        bytearray_writebuffer(&ba, p12path, strlen(p12path));
    }
    lua_pop(L, 1);

    bytearray_write8(&ba, 0x1F);

    // Add PKCS12 password to cache key
    lua_getfield(L, table_index, "pkcs12.password");
    const char *p12password = lua_tostring(L, -1);
    if (p12password) {
        bytearray_writebuffer(&ba, p12password, strlen(p12password));
    }
    lua_pop(L, 1);

    bytearray_write8(&ba, 0);
    bytearray_read_ready(&ba);

    char *key = strdup((const char *)ba.buffer);
    bytearray_dealloc(&ba);

    return key;
}

// Get or create SSL context (with caching)
tcpd_ssl_context_t* tcpd_ssl_context_get_or_create(lua_State *L, const char *cache_key) {
    if (!L || !cache_key) return NULL;

    // Try to get from Lua registry cache
    lua_getfield(L, LUA_REGISTRYINDEX, cache_key);
    if (!lua_isnil(L, -1)) {
        // Found cached context
        tcpd_ssl_context_t *ctx = lua_touserdata(L, -1);
        if (ctx) {
            atomic_fetch_add(&ctx->retain_count, 1);
            lua_pop(L, 1);
            return ctx;
        }
    }
    lua_pop(L, 1);

    // Create new context using lua_newuserdata
    tcpd_ssl_context_t *ctx = tcpd_ssl_context_create(L);
    if (!ctx) return NULL;

    ctx->cache_key = strdup(cache_key);

    // Store in Lua registry (context is already a userdata on stack)
    lua_setfield(L, LUA_REGISTRYINDEX, cache_key);

    return ctx;
}

// Retain SSL context
void tcpd_ssl_context_retain(tcpd_ssl_context_t *ctx) {
    if (ctx) {
        atomic_fetch_add(&ctx->retain_count, 1);
    }
}

// Release SSL context (with cache cleanup)
void tcpd_ssl_context_release(tcpd_ssl_context_t *ctx, lua_State *L) {
    if (!ctx) return;

    // atomic_fetch_sub returns the previous value; the last releaser is the
    // thread that observed prev <= 1. Lua registry mutation must still happen
    // on a thread that owns L, which is guaranteed because release is called
    // from connection cleanup paths bound to the connection's mainthread.
    int prev = atomic_fetch_sub(&ctx->retain_count, 1);
    if (prev <= 1) {
        // Remove from Lua registry cache if cached
        if (ctx->cache_key && L) {
            lua_pushnil(L);
            lua_setfield(L, LUA_REGISTRYINDEX, ctx->cache_key);
        }
        // Note: The actual cleanup is handled by the __gc method
        // when Lua garbage collects the userdata
    }
}


// Set SSL error message for client connection
void tcpd_ssl_set_client_error(tcpd_client_conn_t *client, const char *error) {
    if (!client) return;

    free(client->ssl_error_message);
    client->ssl_error_message = error ? strdup(error) : NULL;
}

// Get SSL error message from client connection
const char* tcpd_ssl_get_client_error(tcpd_client_conn_t *client) {
    return client ? client->ssl_error_message : NULL;
}

// Create client SSL bufferevent
struct bufferevent* tcpd_ssl_create_client_bufferevent(
    struct event_base *base,
    tcpd_ssl_context_t *ctx,
    const char *hostname,
    tcpd_client_conn_t *client,
    int extra_bev_flags) {

    if (!base || !ctx || !ctx->ssl_ctx || !client) return NULL;

    // Initialize SSL fields directly in client structure
    client->ssl = SSL_new(ctx->ssl_ctx);
    if (!client->ssl) return NULL;

    SSL_set_ex_data(client->ssl, ssl_conn_index, client);

    if (hostname) {
        client->ssl_host = strdup(hostname);

#if OPENSSL_VERSION_NUMBER >= 0x1010000fL
        if (ctx->verify_host) {
            SSL_set_hostflags(client->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (!SSL_set1_host(client->ssl, hostname)) {
                printf("SSL_set1_host '%s' failed!\n", hostname);
            }
        }

        if (ctx->verify_peer) {
            SSL_set_verify(client->ssl, SSL_VERIFY_PEER, NULL);
        } else {
            SSL_set_verify(client->ssl, SSL_VERIFY_NONE, NULL);
        }
#endif

        SSL_set_tlsext_host_name(client->ssl, hostname);
    }

    struct bufferevent *bev = bufferevent_openssl_socket_new(
        base, -1, client->ssl, BUFFEREVENT_SSL_CONNECTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | extra_bev_flags
    );

    if (!bev) {
        // If bufferevent creation failed, clean up SSL object
        SSL_free(client->ssl);
        client->ssl = NULL;
        free(client->ssl_host);
        client->ssl_host = NULL;
        return NULL;
    }

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
    tcpd_ssl_context_t *ctx,
    int extra_bev_flags) {

    if (!base || !ctx || !ctx->ssl_ctx) return NULL;

    SSL *ssl = SSL_new(ctx->ssl_ctx);
    if (!ssl) return NULL;

    struct bufferevent *bev = bufferevent_openssl_socket_new(
        base, fd, ssl, BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | extra_bev_flags
    );

    if (!bev) {
        // If bufferevent creation failed, clean up SSL object
        SSL_free(ssl);
        return NULL;
    }

    return bev;
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
                X509 *ca_cert = sk_X509_value(ca, i);
                // add_extra_chain_cert takes ownership, so up-ref first
                X509_up_ref(ca_cert);
                SSL_CTX_add_extra_chain_cert(ctx, ca_cert);
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

// Setup CA verification from explicit file and/or directory.
int tcpd_ssl_setup_ca_verification(SSL_CTX *ctx, const char *ca_info, const char *ca_path) {
    if (!ctx) return -1;

    if (!SSL_CTX_load_verify_locations(ctx, ca_info, ca_path)) {
        printf("SSL_CTX_load_verify_locations failed: cainfo=%s capath=%s\n",
               ca_info ? ca_info : "NULL", ca_path ? ca_path : "NULL");
        return -1;
    }

    return 0;
}

// Default trust store when caller did not set cainfo/capath.
// Order: OpenSSL compiled-in defaults, then common system/app bundles.
// Returns 0 on success, -1 if nothing could be loaded.
int tcpd_ssl_setup_default_ca_verification(SSL_CTX *ctx) {
    if (!ctx) return -1;

    if (SSL_CTX_set_default_verify_paths(ctx) == 1) {
        return 0;
    }
    ERR_clear_error();

    // File bundles (cwd first for app-shipped cacert.pem next to binary/service).
    static const char *const ca_files[] = {
        "cacert.pem",
        "cert.pem",
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/usr/local/etc/openssl@3/cert.pem",
        "/opt/homebrew/etc/openssl@3/cert.pem",
        "/usr/local/etc/openssl/cert.pem",
        "/opt/homebrew/etc/openssl/cert.pem",
        NULL,
    };
    for (const char *const *p = ca_files; *p; ++p) {
        if (access(*p, R_OK) != 0) {
            continue;
        }
        if (SSL_CTX_load_verify_locations(ctx, *p, NULL) == 1) {
            return 0;
        }
        ERR_clear_error();
    }

    // Hashed cert directory (Debian/Ubuntu style).
    static const char *const ca_dirs[] = {
        "/etc/ssl/certs",
        "/etc/pki/tls/certs",
        NULL,
    };
    for (const char *const *p = ca_dirs; *p; ++p) {
        if (access(*p, R_OK | X_OK) != 0) {
            continue;
        }
        if (SSL_CTX_load_verify_locations(ctx, NULL, *p) == 1) {
            return 0;
        }
        ERR_clear_error();
    }

    printf("SSL default CA load failed (no OpenSSL defaults and no common cacert found)\n");
    return -1;
}

// SSL certificate verification callback (for older OpenSSL)
int tcpd_ssl_cert_verify_callback(X509_STORE_CTX *ctx, void *arg) {
    (void)arg;  // Unused parameter

    int preverify_ok = X509_verify_cert(ctx);
    if (!preverify_ok) {
        SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
        int err = X509_STORE_CTX_get_error(ctx);

        if (ssl) {
            tcpd_client_conn_t *client = SSL_get_ex_data(ssl, ssl_conn_index);
            if (client) {
                tcpd_ssl_set_client_error(client, X509_verify_cert_error_string(err));
            } else {
                // Server-side connection: log the error since no client structure is available
                LOGE("SSL verify error (server): %s", X509_verify_cert_error_string(err));
            }
        }
    }

    return preverify_ok;
}

// SSL verification callback (for newer OpenSSL)
int tcpd_ssl_verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
    if (!preverify_ok) {
        SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
        int err = X509_STORE_CTX_get_error(ctx);

        if (ssl) {
            tcpd_client_conn_t *client = SSL_get_ex_data(ssl, ssl_conn_index);
            if (client) {
                tcpd_ssl_set_client_error(client, X509_verify_cert_error_string(err));
            } else {
                // Server-side connection: log the error since no client structure is available
                LOGE("SSL verify error (server): %s", X509_verify_cert_error_string(err));
            }
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

// Register SSL context metatable
void tcpd_ssl_register_metatable(lua_State *L) {
    // Register SSL_CONTEXT_TYPE metatable
    luaL_newmetatable(L, LUA_TCPD_SSL_CONTEXT_TYPE);
    lua_pushcfunction(L, tcpd_ssl_context_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushstring(L, "ssl_context");
    lua_setfield(L, -2, "__typename");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

#endif // FAN_HAS_OPENSSL