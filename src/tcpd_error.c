#include "tcpd_common.h"
#include <string.h>
#include <stdlib.h>

// Error type to string mapping
static const char* error_type_strings[] = {
    [TCPD_ERROR_NONE] = "no error",
    [TCPD_ERROR_TIMEOUT] = "timeout",
    [TCPD_ERROR_CONNECTION_RESET] = "connection reset by peer",
    [TCPD_ERROR_DNS_FAILED] = "DNS resolution failed",
    [TCPD_ERROR_SSL_ERROR] = "SSL error",
    [TCPD_ERROR_EOF] = "connection closed",
    [TCPD_ERROR_UNKNOWN] = "unknown error"
};

// Convert error to human-readable string
const char* tcpd_error_to_string(tcpd_error_t *error) {
    if (!error) return "invalid error object";

    if (error->message) {
        return error->message;
    }

    if (error->type < sizeof(error_type_strings) / sizeof(error_type_strings[0])) {
        return error_type_strings[error->type];
    }

    return "unknown error";
}

// Clean up error structure
void tcpd_error_cleanup(tcpd_error_t *error) {
    if (!error) return;

    free(error->message);
    error->message = NULL;
    error->type = TCPD_ERROR_NONE;
    error->system_error = 0;
}

// Create error from system error code
tcpd_error_t tcpd_error_from_socket_error(int error_code) {
    tcpd_error_t error = {0};

    error.system_error = error_code;
    error.type = TCPD_ERROR_CONNECTION_RESET;
    error.message = strdup(evutil_socket_error_to_string(error_code));

    return error;
}

// Create timeout error
tcpd_error_t tcpd_error_timeout(short events) {
    tcpd_error_t error = {0};

    error.type = TCPD_ERROR_TIMEOUT;

    if (events & BEV_EVENT_READING) {
        error.message = strdup("read timeout");
    } else if (events & BEV_EVENT_WRITING) {
        error.message = strdup("write timeout");
    } else {
        error.message = strdup("unknown timeout");
    }

    return error;
}

// Create DNS error
tcpd_error_t tcpd_error_dns(int dns_error) {
    tcpd_error_t error = {0};

    error.type = TCPD_ERROR_DNS_FAILED;
    error.system_error = dns_error;
    error.message = strdup(evutil_gai_strerror(dns_error));

    return error;
}

// Create EOF error
tcpd_error_t tcpd_error_eof(tcpd_conn_type_t conn_type) {
    tcpd_error_t error = {0};

    error.type = TCPD_ERROR_EOF;

    switch (conn_type) {
        case TCPD_CONN_TYPE_ACCEPT:
            error.message = strdup("client disconnected");
            break;
        case TCPD_CONN_TYPE_CLIENT:
            error.message = strdup("server disconnected");
            break;
        default:
            error.message = strdup("connection closed");
            break;
    }

    return error;
}

// Create SSL error
tcpd_error_t tcpd_error_ssl(const char *ssl_error_msg) {
    tcpd_error_t error = {0};

    error.type = TCPD_ERROR_SSL_ERROR;
    if (ssl_error_msg) {
        error.message = malloc(strlen(ssl_error_msg) + 12); // "SSLError: " + msg + null
        if (error.message) {
            snprintf(error.message, strlen(ssl_error_msg) + 12, "SSLError: %s", ssl_error_msg);
        }
    } else {
        error.message = strdup("SSL error");
    }

    return error;
}

// Resource management helper structures
typedef struct tcpd_resource_list {
    void **resources;
    tcpd_resource_cleanup_func_t *cleanup_funcs;
    int count;
    int capacity;
} tcpd_resource_list_t;

// Initialize resource list
static int tcpd_resource_list_init(tcpd_resource_list_t *list) {
    if (!list) return -1;

    list->resources = NULL;
    list->cleanup_funcs = NULL;
    list->count = 0;
    list->capacity = 0;

    return 0;
}

// Add resource to cleanup list
static int tcpd_resource_list_add(tcpd_resource_list_t *list, void *resource, tcpd_resource_cleanup_func_t cleanup_func) {
    if (!list || !resource || !cleanup_func) return -1;

    // Expand capacity if needed
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity ? list->capacity * 2 : 4;

        void **new_resources = realloc(list->resources, new_capacity * sizeof(void*));
        if (!new_resources) return -1;

        tcpd_resource_cleanup_func_t *new_cleanup_funcs = realloc(list->cleanup_funcs,
                                                                  new_capacity * sizeof(tcpd_resource_cleanup_func_t));
        if (!new_cleanup_funcs) {
            free(new_resources);
            return -1;
        }

        list->resources = new_resources;
        list->cleanup_funcs = new_cleanup_funcs;
        list->capacity = new_capacity;
    }

    list->resources[list->count] = resource;
    list->cleanup_funcs[list->count] = cleanup_func;
    list->count++;

    return 0;
}

// Clean up all resources in list
static void tcpd_resource_list_cleanup(tcpd_resource_list_t *list) {
    if (!list) return;

    for (int i = 0; i < list->count; i++) {
        if (list->resources[i] && list->cleanup_funcs[i]) {
            list->cleanup_funcs[i](list->resources[i]);
        }
    }

    free(list->resources);
    free(list->cleanup_funcs);

    list->resources = NULL;
    list->cleanup_funcs = NULL;
    list->count = 0;
    list->capacity = 0;
}

// Resource manager structure
struct tcpd_resource_manager {
    tcpd_resource_list_t lua_refs;
    tcpd_resource_list_t memory_blocks;
    tcpd_resource_list_t file_handles;
    tcpd_resource_list_t network_resources;
};

// Cleanup functions for different resource types
static void tcpd_cleanup_lua_ref(void *resource) {
    // This would need access to lua_State - simplified for now
    // CLEAR_REF(L, *(int*)resource);
}

static void tcpd_cleanup_memory_block(void *resource) {
    free(resource);
}

static void tcpd_cleanup_file_handle(void *resource) {
    FILE *fp = (FILE*)resource;
    if (fp) fclose(fp);
}

static void tcpd_cleanup_bufferevent(void *resource) {
    struct bufferevent *bev = (struct bufferevent*)resource;
    if (bev) {
        tcpd_shutdown_bufferevent(bev);
    }
}

// Initialize resource manager
int tcpd_resource_manager_init(tcpd_resource_manager_t *manager) {
    if (!manager) return -1;

    tcpd_resource_list_init(&manager->lua_refs);
    tcpd_resource_list_init(&manager->memory_blocks);
    tcpd_resource_list_init(&manager->file_handles);
    tcpd_resource_list_init(&manager->network_resources);

    return 0;
}

// Add resource to manager
int tcpd_resource_manager_add_memory(tcpd_resource_manager_t *manager, void *ptr) {
    return tcpd_resource_list_add(&manager->memory_blocks, ptr, tcpd_cleanup_memory_block);
}

int tcpd_resource_manager_add_file(tcpd_resource_manager_t *manager, FILE *fp) {
    return tcpd_resource_list_add(&manager->file_handles, fp, tcpd_cleanup_file_handle);
}

int tcpd_resource_manager_add_bufferevent(tcpd_resource_manager_t *manager, struct bufferevent *bev) {
    return tcpd_resource_list_add(&manager->network_resources, bev, tcpd_cleanup_bufferevent);
}

// Clean up all resources
void tcpd_resource_manager_cleanup(tcpd_resource_manager_t *manager) {
    if (!manager) return;

    tcpd_resource_list_cleanup(&manager->lua_refs);
    tcpd_resource_list_cleanup(&manager->memory_blocks);
    tcpd_resource_list_cleanup(&manager->file_handles);
    tcpd_resource_list_cleanup(&manager->network_resources);
}

// Safe string duplication with resource tracking
char* tcpd_safe_strdup(tcpd_resource_manager_t *manager, const char *str) {
    if (!str) return NULL;

    char *copy = strdup(str);
    if (copy && manager) {
        tcpd_resource_manager_add_memory(manager, copy);
    }

    return copy;
}

// Safe memory allocation with resource tracking
void* tcpd_safe_malloc(tcpd_resource_manager_t *manager, size_t size) {
    void *ptr = malloc(size);
    if (ptr && manager) {
        tcpd_resource_manager_add_memory(manager, ptr);
    }

    return ptr;
}

// Exception-safe operation wrapper
typedef struct tcpd_operation_context {
    int error_code;
    tcpd_error_t error;
    tcpd_resource_manager_t resource_manager;
} tcpd_operation_context_t;

// Initialize operation context
int tcpd_operation_context_init(tcpd_operation_context_t *ctx) {
    if (!ctx) return -1;

    ctx->error_code = 0;
    memset(&ctx->error, 0, sizeof(tcpd_error_t));
    tcpd_resource_manager_init(&ctx->resource_manager);

    return 0;
}

// Clean up operation context
void tcpd_operation_context_cleanup(tcpd_operation_context_t *ctx) {
    if (!ctx) return;

    tcpd_error_cleanup(&ctx->error);
    tcpd_resource_manager_cleanup(&ctx->resource_manager);
}

// Set operation error
void tcpd_operation_set_error(tcpd_operation_context_t *ctx, tcpd_error_t error) {
    if (!ctx) return;

    tcpd_error_cleanup(&ctx->error);
    ctx->error = error;
    ctx->error_code = -1;
}

// Check if operation has error
int tcpd_operation_has_error(tcpd_operation_context_t *ctx) {
    return ctx ? ctx->error_code != 0 : 1;
}