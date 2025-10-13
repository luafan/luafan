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

