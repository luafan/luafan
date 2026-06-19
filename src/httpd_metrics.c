// httpd_metrics.c — Prometheus-style metrics collection and /metrics endpoint

#include "httpd_internal.h"

// ============================================================
// Metrics structure and global instance
// ============================================================

typedef struct {
    unsigned long requests_total;
    unsigned long requests_active;
    unsigned long bytes_sent;
    unsigned long bytes_received;
    unsigned long errors_total;
    unsigned long memory_allocated;
    unsigned long connections_total;
    unsigned long keepalive_reused;
    time_t start_time;

    unsigned long requests_get;
    unsigned long requests_post;
    unsigned long requests_put;
    unsigned long requests_delete;
    unsigned long requests_other;

    unsigned long responses_2xx;
    unsigned long responses_3xx;
    unsigned long responses_4xx;
    unsigned long responses_5xx;
} httpd_metrics_t;

static httpd_metrics_t g_metrics = {0};

// ============================================================
// Metrics update functions
// ============================================================

void metrics_init(void) {
    memset(&g_metrics, 0, sizeof(g_metrics));
    g_metrics.start_time = time(NULL);
}

void metrics_update_request_start(const char* method) {
    g_metrics.requests_total++;
    g_metrics.requests_active++;

    if (method) {
        if (strcmp(method, "GET") == 0) {
            g_metrics.requests_get++;
        } else if (strcmp(method, "POST") == 0) {
            g_metrics.requests_post++;
        } else if (strcmp(method, "PUT") == 0) {
            g_metrics.requests_put++;
        } else if (strcmp(method, "DELETE") == 0) {
            g_metrics.requests_delete++;
        } else {
            g_metrics.requests_other++;
        }
    }
}

void metrics_update_request_end(int status_code, size_t bytes_sent) {
    if (g_metrics.requests_active > 0) {
        g_metrics.requests_active--;
    }

    g_metrics.bytes_sent += bytes_sent;

    if (status_code >= 200 && status_code < 300) {
        g_metrics.responses_2xx++;
    } else if (status_code >= 300 && status_code < 400) {
        g_metrics.responses_3xx++;
    } else if (status_code >= 400 && status_code < 500) {
        g_metrics.responses_4xx++;
        g_metrics.errors_total++;
    } else if (status_code >= 500) {
        g_metrics.responses_5xx++;
        g_metrics.errors_total++;
    }
}

void metrics_update_connection(void) {
    g_metrics.connections_total++;
}

// ============================================================
// Smoke test endpoint
// ============================================================

void smoke_request_cb(struct evhttp_request *req, void *arg) {
    (void)arg;
    evhttp_send_reply(req, 200, "OK", NULL);
}

// ============================================================
// Metrics endpoint
// ============================================================

void metrics_request_cb(struct evhttp_request *req, void *arg) {
    (void)arg;
    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_reply(req, 500, "Internal Server Error", NULL);
        return;
    }

    time_t now = time(NULL);
    time_t uptime = now - g_metrics.start_time;

    evbuffer_add_printf(buf,
        "# HTTPD Server Metrics\n"
        "uptime_seconds %ld\n"
        "requests_total %lu\n"
        "requests_active %lu\n"
        "bytes_sent_total %lu\n"
        "bytes_received_total %lu\n"
        "errors_total %lu\n"
        "memory_allocated_bytes %lu\n"
        "connections_total %lu\n"
        "keepalive_reused_total %lu\n"
        "\n# Requests by method\n"
        "requests_get_total %lu\n"
        "requests_post_total %lu\n"
        "requests_put_total %lu\n"
        "requests_delete_total %lu\n"
        "requests_other_total %lu\n"
        "\n# Responses by status class\n"
        "responses_2xx_total %lu\n"
        "responses_3xx_total %lu\n"
        "responses_4xx_total %lu\n"
        "responses_5xx_total %lu\n",
        (long)uptime,
        g_metrics.requests_total,
        g_metrics.requests_active,
        g_metrics.bytes_sent,
        g_metrics.bytes_received,
        g_metrics.errors_total,
        g_metrics.memory_allocated,
        g_metrics.connections_total,
        g_metrics.keepalive_reused,
        g_metrics.requests_get,
        g_metrics.requests_post,
        g_metrics.requests_put,
        g_metrics.requests_delete,
        g_metrics.requests_other,
        g_metrics.responses_2xx,
        g_metrics.responses_3xx,
        g_metrics.responses_4xx,
        g_metrics.responses_5xx
    );

    evhttp_add_header(req->output_headers, "Content-Type", "text/plain; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}
