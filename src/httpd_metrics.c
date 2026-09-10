// httpd_metrics.c — Prometheus-style metrics collection and /metrics endpoint

#include "httpd_internal.h"

// ============================================================
// Metrics structure and global instance
// ============================================================

typedef struct {
    _Atomic unsigned long requests_total;
    _Atomic unsigned long requests_active;
    _Atomic unsigned long bytes_sent;
    _Atomic unsigned long bytes_received;
    _Atomic unsigned long errors_total;
    _Atomic unsigned long memory_allocated;
    _Atomic unsigned long connections_total;
    _Atomic unsigned long keepalive_reused;
    time_t start_time;

    _Atomic unsigned long requests_get;
    _Atomic unsigned long requests_post;
    _Atomic unsigned long requests_put;
    _Atomic unsigned long requests_delete;
    _Atomic unsigned long requests_other;

    _Atomic unsigned long responses_2xx;
    _Atomic unsigned long responses_3xx;
    _Atomic unsigned long responses_4xx;
    _Atomic unsigned long responses_5xx;
} httpd_metrics_t;

static httpd_metrics_t g_metrics = {0};

// ============================================================
// Metrics update functions
// ============================================================

void metrics_init(void) {
    /* Metrics are process-global and shared by every HTTPD instance. Only the
     * first bind initialises them; later binds must not wipe counters that
     * other live servers are still updating. */
    static int initialized = 0;
    if (initialized) {
        return;
    }
    initialized = 1;
    atomic_store(&g_metrics.requests_total, 0u);
    atomic_store(&g_metrics.requests_active, 0u);
    atomic_store(&g_metrics.bytes_sent, 0u);
    atomic_store(&g_metrics.bytes_received, 0u);
    atomic_store(&g_metrics.errors_total, 0u);
    atomic_store(&g_metrics.memory_allocated, 0u);
    atomic_store(&g_metrics.connections_total, 0u);
    atomic_store(&g_metrics.keepalive_reused, 0u);
    atomic_store(&g_metrics.requests_get, 0u);
    atomic_store(&g_metrics.requests_post, 0u);
    atomic_store(&g_metrics.requests_put, 0u);
    atomic_store(&g_metrics.requests_delete, 0u);
    atomic_store(&g_metrics.requests_other, 0u);
    atomic_store(&g_metrics.responses_2xx, 0u);
    atomic_store(&g_metrics.responses_3xx, 0u);
    atomic_store(&g_metrics.responses_4xx, 0u);
    atomic_store(&g_metrics.responses_5xx, 0u);
    g_metrics.start_time = time(NULL);
}

void metrics_update_request_start(const char* method) {
    atomic_fetch_add(&g_metrics.requests_total, 1u);
    atomic_fetch_add(&g_metrics.requests_active, 1u);

    if (method) {
        if (strcmp(method, "GET") == 0) {
            atomic_fetch_add(&g_metrics.requests_get, 1u);
        } else if (strcmp(method, "POST") == 0) {
            atomic_fetch_add(&g_metrics.requests_post, 1u);
        } else if (strcmp(method, "PUT") == 0) {
            atomic_fetch_add(&g_metrics.requests_put, 1u);
        } else if (strcmp(method, "DELETE") == 0) {
            atomic_fetch_add(&g_metrics.requests_delete, 1u);
        } else {
            atomic_fetch_add(&g_metrics.requests_other, 1u);
        }
    }
}

void metrics_update_request_end(int status_code, size_t bytes_sent) {
    unsigned long active = atomic_load(&g_metrics.requests_active);
    while (active > 0 &&
           !atomic_compare_exchange_weak(&g_metrics.requests_active, &active, active - 1u)) {
        /* active is refreshed by the failed compare-exchange. */
    }

    atomic_fetch_add(&g_metrics.bytes_sent, (unsigned long)bytes_sent);

    if (status_code >= 200 && status_code < 300) {
        atomic_fetch_add(&g_metrics.responses_2xx, 1u);
    } else if (status_code >= 300 && status_code < 400) {
        atomic_fetch_add(&g_metrics.responses_3xx, 1u);
    } else if (status_code >= 400 && status_code < 500) {
        atomic_fetch_add(&g_metrics.responses_4xx, 1u);
        atomic_fetch_add(&g_metrics.errors_total, 1u);
    } else if (status_code >= 500) {
        atomic_fetch_add(&g_metrics.responses_5xx, 1u);
        atomic_fetch_add(&g_metrics.errors_total, 1u);
    }
}

void metrics_update_connection(void) {
    atomic_fetch_add(&g_metrics.connections_total, 1u);
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
        atomic_load(&g_metrics.requests_total),
        atomic_load(&g_metrics.requests_active),
        atomic_load(&g_metrics.bytes_sent),
        atomic_load(&g_metrics.bytes_received),
        atomic_load(&g_metrics.errors_total),
        atomic_load(&g_metrics.memory_allocated),
        atomic_load(&g_metrics.connections_total),
        atomic_load(&g_metrics.keepalive_reused),
        atomic_load(&g_metrics.requests_get),
        atomic_load(&g_metrics.requests_post),
        atomic_load(&g_metrics.requests_put),
        atomic_load(&g_metrics.requests_delete),
        atomic_load(&g_metrics.requests_other),
        atomic_load(&g_metrics.responses_2xx),
        atomic_load(&g_metrics.responses_3xx),
        atomic_load(&g_metrics.responses_4xx),
        atomic_load(&g_metrics.responses_5xx)
    );

    evhttp_add_header(req->output_headers, "Content-Type", "text/plain; charset=utf-8");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}
