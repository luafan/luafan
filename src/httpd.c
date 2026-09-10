// httpd.c — Core HTTP server: Lua bindings, dispatch, server lifecycle

#include "httpd_internal.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* Owner-thread teardown job (see docs/threading-fix-plan.md Phase 1).
 * One job per evhttp instance; slot points at the server field that owns
 * the instance (server->httpd or server->workers[i].httpd). */
typedef struct {
    LuaServer *server;
    struct evhttp **slot;
    int owner_id; /* -1 = main base, >= 0 = worker id */
} httpd_teardown_job_t;

static void httpd_teardown_job_cb(evutil_socket_t fd, short what, void *arg);
static void httpd_drain_check_cb(evutil_socket_t fd, short what, void *arg);
static void httpd_server_teardown_instance(LuaServer *server,
                                           struct evhttp **slot, int owner_id);
static void httpd_server_teardown_done(LuaServer *server);
static void httpd_server_finalize(LuaServer *server);

const MethodMap methodMap[] = {
    {"GET", EVHTTP_REQ_GET},       {"POST", EVHTTP_REQ_POST},
    {"HEAD", EVHTTP_REQ_HEAD},     {"PUT", EVHTTP_REQ_PUT},
    {"DELETE", EVHTTP_REQ_DELETE}, {"OPTIONS", EVHTTP_REQ_OPTIONS},
    {"TRACE", EVHTTP_REQ_TRACE},   {"CONNECT", EVHTTP_REQ_CONNECT},
    {"PATCH", EVHTTP_REQ_PATCH},   {NULL, EVHTTP_REQ_GET}};

// ============================================================
// Logging
// ============================================================

void httpd_log(log_level_t level, const char* format, ...) {
    const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);

    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_names[level], message);
    fflush(stderr);
}

// ============================================================
// Request lifecycle
// ============================================================

void httpd_finish_metrics(Request *request, int status_code, size_t bytes_sent);

static void httpd_conn_close_cb(struct evhttp_connection *evcon, void *arg) {
    (void)evcon;
    Request *request = (Request *)arg;
    if (!request || request->close_cb_running) {
        return;
    }
    request->close_cb_running = 1;
    if (request->req && request->reply_status == REPLY_STATUS_REPLY_START) {
        LOG_WARN_FMT("HTTP connection closed before response completed uri=%s",
                     evhttp_request_get_uri(request->req));
        request->reply_status = REPLY_STATUS_REPLYED;
        evhttp_send_reply_end(request->req);
    }
    httpd_finish_metrics(request, 499, 0);
    request->req = NULL;
    request->reply_status = REPLY_STATUS_REPLYED;
    if (request->prevent_gc_ref != LUA_NOREF && request->mainthread) {
        CLEAR_REF(request->mainthread, request->prevent_gc_ref);
    }
}

void httpd_finish_metrics(Request *request, int status_code, size_t bytes_sent) {
    if (!request || request->metrics_finished) {
        return;
    }
    request->metrics_finished = 1;
    metrics_update_request_end(status_code, bytes_sent);
}

void httpd_release_conn_guard(Request *request) {
    if (request->req) {
        struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
        if (evcon) {
            evhttp_connection_set_closecb(evcon, NULL, NULL);
        }
    }
    if (request->prevent_gc_ref != LUA_NOREF && request->mainthread) {
        CLEAR_REF(request->mainthread, request->prevent_gc_ref);
    }
}

void newtable_from_req(lua_State *L, struct evhttp_request *req, LuaServer *server) {
    lua_newtable(L);
    Request *request = (Request *)lua_newuserdata(L, sizeof(Request));
    luaL_getmetatable(L, LUA_EVHTTP_REQUEST_DATA_TYPE);
    lua_setmetatable(L, -2);
    memset(request, 0, sizeof(Request));
    request->req = req;
    request->server = server;
    request->worker_id = event_mgr_current_worker_id();
    pthread_mutex_init(&request->ws_mutex, NULL);
    request->ws_mutex_initialized = 1;
    request->ws_pending_sends = 0;
    request->reply_status = REPLY_STATUS_NONE;
    request->is_websocket = 0;
    request->ws_state = WS_STATE_CONNECTING;
    request->ws_bev = NULL;
    request->ws_deferred_bev = NULL;
    request->ws_cleanup_requested = 0;
    request->mainthread = utlua_mainthread(L);
    request->_ref_ = LUA_NOREF;
    request->self_ref = LUA_NOREF;
    request->prevent_gc_ref = LUA_NOREF;
    request->frame_queue_head = NULL;
    request->frame_queue_tail = NULL;
    request->frame_queue_len = 0;
    request->owns_request = 0;
    request->ws_cleaning_up = 0;
    /* ws_pmd / zlib streams zeroed by memset */
    lua_rawseti(L, -2, 1);

    lua_lock(L);
    lua_pushvalue(L, -1);
    request->prevent_gc_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_unlock(L);

    struct evhttp_connection *evcon = evhttp_request_get_connection(req);
    if (evcon) {
        evhttp_connection_set_closecb(evcon, httpd_conn_close_cb, request);
    }
}

// ============================================================
// Connection header management
// ============================================================

void set_connection_header(struct evhttp_request *req, LuaServer *server) {
    if (!server->enable_keep_alive) {
        evhttp_add_header(req->output_headers, "Connection", "close");
        return;
    }

    int major = req->major;
    int minor = req->minor;
    if (major < 0 || minor < 0) {
        major = 1;
        minor = 0;
    }

    if (major > 1 || (major == 1 && minor >= 1)) {
        const char *connection = evhttp_find_header(req->input_headers, "Connection");
        if (connection && strcasecmp(connection, "close") == 0) {
            evhttp_add_header(req->output_headers, "Connection", "close");
        } else {
            char keep_alive_header[64];
            snprintf(keep_alive_header, sizeof(keep_alive_header),
                    "timeout=%d, max=%d",
                    server->keep_alive_timeout,
                    server->max_keep_alive_requests);
            evhttp_add_header(req->output_headers, "Keep-Alive", keep_alive_header);
            evhttp_add_header(req->output_headers, "Connection", "keep-alive");
        }
    } else {
        const char *connection = evhttp_find_header(req->input_headers, "Connection");
        if (connection && strcasecmp(connection, "keep-alive") == 0) {
            char keep_alive_header[64];
            snprintf(keep_alive_header, sizeof(keep_alive_header),
                    "timeout=%d, max=%d",
                    server->keep_alive_timeout,
                    server->max_keep_alive_requests);
            evhttp_add_header(req->output_headers, "Keep-Alive", keep_alive_header);
            evhttp_add_header(req->output_headers, "Connection", "keep-alive");
        } else {
            evhttp_add_header(req->output_headers, "Connection", "close");
        }
    }
}

// ============================================================
// Request body preflight
// ============================================================

/*
 * evhttp normally invokes the generic callback after the request body has
 * been collected.  Do not pass a request to Lua when its wire metadata says
 * that a body exists but the input buffer is unavailable/empty.  Previously
 * request_push_body() converted these transport failures into Lua nil, which
 * the API layer reported as the misleading {"error":"empty body"}.
 *
 * This check deliberately does not reject requests without Content-Length:
 * those may be GETs or valid bodyless requests, and libevent may represent
 * chunked requests without a Content-Length header.
 */
static int httpd_request_body_preflight(struct evhttp_request *req,
                                        LuaServer *server) {
    const char *content_length =
        evhttp_find_header(req->input_headers, "Content-Length");
    int has_content_length = content_length && *content_length != '\0';
    unsigned long long declared = 0;

    if (has_content_length) {
        const char *digit = content_length;
        while (*digit >= '0' && *digit <= '9') {
            digit++;
        }
        errno = 0;
        char *end = NULL;
        declared = strtoull(content_length, &end, 10);
        if (*digit != '\0' || errno == ERANGE || end == content_length || *end != '\0') {
            LOG_ERROR_FMT("HTTP request rejected: invalid Content-Length '%s' uri=%s",
                          content_length,
                          evhttp_request_get_uri(req));
            evhttp_send_error(req, 400, "Invalid Content-Length");
            return 0;
        }
    }

    size_t limit = server && server->max_body_size
        ? server->max_body_size : HTTP_POST_BODY_LIMIT;
    if (has_content_length && declared > (unsigned long long)limit) {
        LOG_ERROR_FMT("HTTP request rejected: body too large declared=%llu limit=%zu uri=%s",
                      declared, limit, evhttp_request_get_uri(req));
        evhttp_send_error(req, 413, "Request Entity Too Large");
        return 0;
    }

    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
    size_t buffered = bodybuf ? evbuffer_get_length(bodybuf) : 0;
    if (!has_content_length && buffered > limit) {
        LOG_ERROR_FMT("HTTP request rejected: body too large buffered=%zu limit=%zu uri=%s",
                      buffered, limit, evhttp_request_get_uri(req));
        evhttp_send_error(req, 413, "Request Entity Too Large");
        return 0;
    }
    if (has_content_length && declared > 0 && (!bodybuf || buffered == 0)) {
        LOG_ERROR_FMT("HTTP request rejected: body unavailable declared=%llu buffered=%zu uri=%s",
                      declared, buffered, evhttp_request_get_uri(req));
        evhttp_send_error(req, 400, "Request Body Unavailable");
        return 0;
    }
    if (has_content_length && buffered < (size_t)declared) {
        LOG_ERROR_FMT("HTTP request rejected: incomplete body declared=%llu buffered=%zu uri=%s",
                      declared, buffered, evhttp_request_get_uri(req));
        evhttp_send_error(req, 400, "Incomplete Request Body");
        return 0;
    }

    return 1;
}

// ============================================================
// Main request dispatch
// ============================================================

static void httpd_handler_cgi_bin(struct evhttp_request *req, LuaServer *server) {
    if (!httpd_request_body_preflight(req, server)) {
        return;
    }
    metrics_update_connection();

    const char* method = NULL;
    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    const MethodMap *method_map;
    for (method_map = methodMap; method_map->name; method_map++) {
        if (cmd == method_map->cmd) {
            method = method_map->name;
            break;
        }
    }

    metrics_update_request_start(method);

    lua_State *mainthread = server->mainthread;
    lua_lock(mainthread);
    fan_cb_setup_t cbs = fan_cb_setup(mainthread, server->onServiceRef);
    if (!cbs.co) {
        LOG_ERROR_FMT("HTTP callback setup failed method=%s uri=%s",
                      method ? method : "UNKNOWN", evhttp_request_get_uri(req));
        lua_unlock(mainthread);
        evhttp_send_error(req, 500, "Internal Server Error");
        metrics_update_request_end(500, 0);
        return;
    }

    if (!lua_isfunction(cbs.co, -1)) {
        LOGE("httpd gen_cb: onServiceRef=%d resolved to %s, expected function\n",
             server->onServiceRef, luaL_typename(cbs.co, -1));
        lua_pop(cbs.co, 1);
        lua_unlock(mainthread);
        FAN_CB_CLEANUP(mainthread, cbs);
        evhttp_send_error(req, 500, "Internal Server Error");
        metrics_update_request_end(500, 0);
        return;
    }

    newtable_from_req(cbs.co, req, server);

    luaL_getmetatable(cbs.co, LUA_EVHTTP_REQUEST_TYPE);
    lua_setmetatable(cbs.co, -2);

    lua_rawgeti(cbs.co, -1, 1);
    Request *request = (Request *)lua_touserdata(cbs.co, -1);
    lua_pop(cbs.co, 1);

    lua_pushvalue(cbs.co, -1);

    lua_unlock(mainthread);
    int status = FAN_RESUME(cbs.co, mainthread, 2);
    if (status != LUA_YIELD && status != LUA_OK) {
        LOG_ERROR_FMT("HTTP Lua callback failed method=%s uri=%s status=%d",
                      method ? method : "UNKNOWN", evhttp_request_get_uri(req), status);
        if (request->req && request->reply_status == REPLY_STATUS_NONE) {
            evhttp_send_error(request->req, 500, "Internal Server Error");
            request->reply_status = REPLY_STATUS_REPLYED;
            httpd_finish_metrics(request, 500, 0);
            httpd_release_conn_guard(request);
        }
    }
    if (status != LUA_YIELD && request->reply_status != REPLY_STATUS_REPLYED) {
        httpd_release_conn_guard(request);
    }
    FAN_CB_CLEANUP(mainthread, cbs);
}

// ============================================================
// Request method table
// ============================================================

static const struct luaL_Reg evhttp_request_lib[] = {
    {"read", lua_evhttp_request_read},
    {"available", lua_evhttp_request_available},

    {"addheader", lua_evhttp_request_reply_addheader},

    {"reply", lua_evhttp_request_reply},
    {"reply_start", lua_evhttp_request_reply_start},
    {"reply_chunk", lua_evhttp_request_reply_chunk},
    {"reply_end", lua_evhttp_request_reply_end},

    {"is_websocket_upgrade", lua_evhttp_request_is_websocket_upgrade},
    {"websocket_accept", lua_evhttp_request_websocket_accept},
    {"websocket_send", lua_evhttp_request_websocket_send},
    {"websocket_ping", lua_evhttp_request_websocket_ping},
    {"websocket_pong", lua_evhttp_request_websocket_pong},
    {"websocket_close", lua_evhttp_request_websocket_close},
    {"websocket_state", lua_evhttp_request_websocket_state},
    {"websocket_receive", lua_evhttp_request_websocket_receive},
    {NULL, NULL},
};

// ============================================================
// Server GC
// ============================================================

/* ============================================================
 * Server lifecycle: asynchronous, drain-aware teardown
 * (see docs/threading-fix-plan.md Phase 1)
 * ============================================================ */

/* Attach a live WebSocket request to its server's tracking structures.
 * Returns 0 on success, -1 when the server is no longer live (caller
 * must abort the upgrade / close the connection). Owner thread. */
int httpd_server_ws_attach(Request *request) {
    if (!request || !request->server) return -1;
    LuaServer *server = request->server;
    int rc = -1;
    pthread_mutex_lock(&server->accept_mutex);
    if (!request->ws_linked &&
        (httpd_life_state_t)atomic_load(&server->life_state) == HTTPD_LIVE) {
        request->ws_next = server->ws_list;
        request->ws_prev = NULL;
        if (server->ws_list) {
            server->ws_list->ws_prev = request;
        }
        server->ws_list = request;
        request->ws_linked = 1;
        int idx = request->worker_id + 1;
        if (server->instance_ws && idx >= 0 && idx < server->instance_ws_len) {
            server->instance_ws[idx]++;
        }
        rc = 0;
    }
    pthread_mutex_unlock(&server->accept_mutex);
    return rc;
}

/* Detach a WebSocket request whose deferred cleanup finished. Idempotent;
 * safe to call when the request was never attached. Owner thread. */
void httpd_server_ws_detach(Request *request) {
    if (!request || !request->server) return;
    LuaServer *server = request->server;
    pthread_mutex_lock(&server->accept_mutex);
    if (request->ws_linked) {
        if (request->ws_prev) {
            request->ws_prev->ws_next = request->ws_next;
        } else {
            server->ws_list = request->ws_next;
        }
        if (request->ws_next) {
            request->ws_next->ws_prev = request->ws_prev;
        }
        request->ws_next = NULL;
        request->ws_prev = NULL;
        request->ws_linked = 0;
        int idx = request->worker_id + 1;
        if (server->instance_ws && idx >= 0 && idx < server->instance_ws_len &&
            server->instance_ws[idx] > 0) {
            server->instance_ws[idx]--;
        }
    }
    pthread_mutex_unlock(&server->accept_mutex);
}

static void httpd_finalize_job_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd;
    (void)what;
    httpd_server_finalize((LuaServer *)arg);
}

static void httpd_server_teardown_done(LuaServer *server) {
    int last = 0;
    pthread_mutex_lock(&server->accept_mutex);
    if (server->teardown_remaining > 0) {
        server->teardown_remaining--;
    }
    last = (server->teardown_remaining == 0);
    pthread_mutex_unlock(&server->accept_mutex);
    if (!last) {
        return;
    }
    /* Finalize on the main base rather than on whichever thread finished the
     * last teardown job. The main-base FIFO then guarantees that every
     * accept/resume job queued before this point has already run, so no bare
     * LuaServer* is left behind when the userdata becomes collectable. */
    if (event_mgr_worker_once_internal(-1, httpd_finalize_job_cb, server) != 0) {
        LOG_ERROR_FMT("httpd finalize dispatch failed; resources retained until exit");
    }
}

/* Runs on the thread of the last completed teardown job. Releases all
 * native allocations and drops the Lua-side self pin so the userdata can
 * be collected. The accept mutex itself is intentionally NOT destroyed
 * (process-lifetime object; destroying it here would race late Lua calls
 * such as rebind/close on other threads). */
static void httpd_server_finalize(LuaServer *server) {
    if (!server) return;
    pthread_mutex_lock(&server->accept_mutex);
    if (server->ws_list) {
        LOG_WARN_FMT("httpd finalize with live WebSocket entries (leak path)");
    }
    if (server->pending_accepts != 0) {
        LOG_WARN_FMT("httpd finalize with pending accepts=%u (leak path)",
                     server->pending_accepts);
    }
    pthread_mutex_unlock(&server->accept_mutex);

    if (server->workers) {
        free(server->workers);
        server->workers = NULL;
    }
    server->worker_count = 0;
    if (server->instance_ws) {
        free(server->instance_ws);
        server->instance_ws = NULL;
    }
    if (server->instance_accepts) {
        free(server->instance_accepts);
        server->instance_accepts = NULL;
    }
    server->instance_ws_len = 0;
#if FAN_HAS_OPENSSL
    if (server->ctx) {
        SSL_CTX_free(server->ctx);
        server->ctx = NULL;
    }
#endif
    free(server->host);
    server->host = NULL;
    atomic_store(&server->life_state, HTTPD_GONE);

    if (server->self_ref != LUA_NOREF && server->mainthread) {
        lua_lock(server->mainthread);
        if (server->self_ref != LUA_NOREF) {
            luaL_unref(server->mainthread, LUA_REGISTRYINDEX, server->self_ref);
            server->self_ref = LUA_NOREF;
        }
        lua_unlock(server->mainthread);
    }
}

/* Owner-thread drain check. Re-arms itself while the instance still has
 * attached WebSocket connections (their deferred cleanups run on this same
 * loop and will drain the counter); once clear, frees the evhttp instance. */
static void httpd_drain_check_cb(evutil_socket_t fd, short what, void *arg) {
    httpd_teardown_job_t *job = (httpd_teardown_job_t *)arg;
    (void)fd;
    (void)what;
    LuaServer *server = job->server;
    struct evhttp **slot = job->slot;
    int owner_id = job->owner_id;

    if ((httpd_life_state_t)atomic_load(&server->life_state) != HTTPD_DRAINING) {
        free(job);
        return;
    }

    int idx = owner_id + 1;
    pthread_mutex_lock(&server->accept_mutex);
    unsigned int ws = (server->instance_ws && idx >= 0 && idx < server->instance_ws_len)
                          ? server->instance_ws[idx] : 0;
    unsigned int accepts =
        (server->instance_accepts && idx >= 0 && idx < server->instance_ws_len)
            ? server->instance_accepts[idx] : 0;
    pthread_mutex_unlock(&server->accept_mutex);

    if (ws > 0 || accepts > 0) {
        /* Wait for deferred WebSocket cleanups and in-flight accept jobs that
         * still hold a raw pointer to this evhttp instance. Re-arm with a small
         * delay so a slow/wedged connection cannot busy-spin the owner loop. */
        if (event_mgr_worker_once_internal_delay(owner_id, httpd_drain_check_cb, job, 5) == 0) {
            return;
        }
        LOG_ERROR_FMT("httpd drain-check could not be rescheduled (owner=%d); "
                      "resources retained until exit", owner_id);
        free(job);
        return;
    }

    struct evhttp *httpd = slot ? *slot : NULL;
    if (slot) {
        *slot = NULL;
    }
    if (httpd) {
        /* Plain-HTTP connections still attached are force-closed here on the
         * owner thread — no Lua can be running concurrently. WebSockets have
         * all finished their deferred cleanup (counter == 0). */
        evhttp_free(httpd);
    }
    free(job);
    httpd_server_teardown_done(server);
}

/* Runs on the instance owner thread: stop accepting (listener instance),
 * drain this owner's WebSockets, then arm the drain-check. */
static void httpd_server_teardown_instance(LuaServer *server,
                                           struct evhttp **slot, int owner_id) {
    struct evhttp *httpd = slot ? *slot : NULL;
    if (!httpd) {
        httpd_server_teardown_done(server);
        return;
    }

    pthread_mutex_lock(&server->accept_mutex);
    struct evhttp_bound_socket **bound_slot = NULL;
    if (slot == &server->httpd) {
        bound_slot = &server->boundsocket;
    } else if (owner_id >= 0 && server->workers
               && owner_id < server->worker_count) {
        bound_slot = &server->workers[owner_id].boundsocket;
    }
    if (bound_slot && *bound_slot) {
        evhttp_del_accept_socket(httpd, *bound_slot);
        *bound_slot = NULL;
    }
    pthread_mutex_unlock(&server->accept_mutex);

    /* Drain this instance's WebSocket connections. ws_connection_cleanup is
     * idempotent (CAS) and never touches Lua; the connections then progress
     * through their deferred-cleanup path on this same loop. */
    for (;;) {
        Request *req = NULL;
        pthread_mutex_lock(&server->accept_mutex);
        for (Request *candidate = server->ws_list; candidate; candidate = candidate->ws_next) {
            if (candidate->ws_linked && candidate->worker_id == owner_id &&
                !candidate->ws_cleanup_requested) {
                candidate->ws_cleanup_requested = 1;
                req = candidate;
                break;
            }
        }
        pthread_mutex_unlock(&server->accept_mutex);
        if (!req) break;
        ws_connection_cleanup(req);
    }

    httpd_teardown_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        LOG_ERROR_FMT("httpd teardown drain-check alloc failed (owner=%d)", owner_id);
        return;
    }
    job->server = server;
    job->slot = slot;
    job->owner_id = owner_id;
    /* Internal hand-off (see event_mgr.h): this arm runs on the owner thread
     * and may happen while a shutdown already closed the user-level dispatch
     * gate; the shutdown drain of that owner base still runs it. */
    if (event_mgr_worker_once_internal(owner_id, httpd_drain_check_cb, job) != 0) {
        free(job);
        LOG_ERROR_FMT("httpd teardown drain-check dispatch failed (owner=%d); "
                      "resources retained until exit", owner_id);
    }
}

static void httpd_teardown_job_cb(evutil_socket_t fd, short what, void *arg) {
    httpd_teardown_job_t *job = (httpd_teardown_job_t *)arg;
    (void)fd;
    (void)what;
    httpd_server_teardown_instance(job->server, job->slot, job->owner_id);
    free(job);
}

/* Queue an owner-thread teardown job for one evhttp instance. Never blocks:
 * when the owner loop is unreachable the instance is left in place and the
 * whole server stays pinned until process exit (fix-plan R7 leak path). */
static void httpd_server_queue_teardown(LuaServer *server,
                                        struct evhttp **slot, int owner_id) {
    if (!slot || !*slot) {
        httpd_server_teardown_done(server);
        return;
    }
    if (event_mgr_is_current_owner(owner_id)) {
        httpd_server_teardown_instance(server, slot, owner_id);
        return;
    }
    if (!event_mgr_is_loop_running()) {
        LOG_WARN_FMT("httpd teardown skipped: owner loop stopped (owner=%d); "
                     "resources retained until exit", owner_id);
        return;
    }
    httpd_teardown_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        LOG_ERROR_FMT("httpd teardown job alloc failed (owner=%d)", owner_id);
        return;
    }
    job->server = server;
    job->slot = slot;
    job->owner_id = owner_id;
    /* Internal hand-off: the drain check arms itself from the owner callback,
     * so this dispatch must survive a shutdown that already closed the
     * user-level gate (see event_mgr.h). */
    if (event_mgr_worker_once_internal(owner_id, httpd_teardown_job_cb, job) != 0) {
        free(job);
        LOG_ERROR_FMT("httpd teardown dispatch failed (owner=%d); "
                      "resources retained until exit", owner_id);
    }
}

/* Request asynchronous teardown of the whole server. Non-blocking and safe
 * from any thread; must NOT be called while holding the Lua lock with the
 * intent to wait — it never waits. */
void httpd_server_begin_destroy(LuaServer *server) {
    if (!server) return;
    pthread_mutex_lock(&server->accept_mutex);
    if ((httpd_life_state_t)atomic_load(&server->life_state) != HTTPD_LIVE) {
        pthread_mutex_unlock(&server->accept_mutex);
        return;
    }
    atomic_store(&server->life_state, HTTPD_DRAINING);
    server->accepting = 0;
    server->teardown_remaining =
        server->distribute_connections ? (unsigned int)(server->worker_count + 1) : 1u;
    pthread_mutex_unlock(&server->accept_mutex);

    if (server->distribute_connections) {
        /* Workers own listeners; the main evhttp is a listener-less config
         * instance and still needs owner-thread teardown. */
        httpd_server_queue_teardown(server, &server->httpd, -1);
        for (int i = 0; i < server->worker_count; i++) {
            httpd_server_queue_teardown(server, &server->workers[i].httpd, i);
        }
    } else {
        /* Single instance: listener + connections on the selected base. */
        int owner = (server->worker_specified && server->worker_id >= 0)
                        ? server->worker_id : -1;
        httpd_server_queue_teardown(server, &server->httpd, owner);
    }
}

/* Bind-error path: the server was never exposed (no connections, listener
 * absent or already detached), so synchronous release on the bind thread is
 * safe. Marks GONE so a later __gc is a no-op. */
static void httpd_server_free_after_bind_error(LuaServer *server) {
    if (!server) return;
    if (server->httpd) {
        evhttp_free(server->httpd);
        server->httpd = NULL;
    }
    server->boundsocket = NULL;
    if (server->workers) {
        for (int i = 0; i < server->worker_count; i++) {
            if (server->workers[i].httpd) {
                evhttp_free(server->workers[i].httpd);
                server->workers[i].httpd = NULL;
            }
        }
        free(server->workers);
        server->workers = NULL;
    }
    server->worker_count = 0;
    if (server->instance_ws) {
        free(server->instance_ws);
        server->instance_ws = NULL;
    }
    if (server->instance_accepts) {
        free(server->instance_accepts);
        server->instance_accepts = NULL;
    }
    server->instance_ws_len = 0;
#if FAN_HAS_OPENSSL
    if (server->ctx) {
        SSL_CTX_free(server->ctx);
        server->ctx = NULL;
    }
#endif
    free(server->host);
    server->host = NULL;
    atomic_store(&server->life_state, HTTPD_GONE);
}

LUA_API int lua_evhttp_server_gc(lua_State *L) {
    LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
    /* Idempotent re-entry after finalize released everything. */
    if ((httpd_life_state_t)atomic_load(&server->life_state) == HTTPD_GONE) {
        lua_pop(L, 1);
        return 0;
    }
    /* Pin the userdata for the (possibly async, cross-thread) teardown.
     * finalize drops the pin; until then the native block stays valid. */
    lua_lock(L);
    lua_pushvalue(L, 1);
    server->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_unlock(L);
    CLEAR_REF(L, server->onServiceRef)
    if ((httpd_life_state_t)atomic_load(&server->life_state) == HTTPD_LIVE) {
        httpd_server_begin_destroy(server);
    }
    lua_pop(L, 1);
    return 0;
}

/* Explicit, idempotent shutdown. Non-blocking: native teardown completes on
 * the owner loops asynchronously (see fix-plan Phase 1). */
LUA_API int lua_evhttp_server_close(lua_State *L) {
    LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
    if ((httpd_life_state_t)atomic_load(&server->life_state) == HTTPD_LIVE) {
        /* Keep the userdata alive until owner-thread teardown finishes. The
         * pin is idempotent so repeated close calls do not leak registry refs. */
        lua_lock(L);
        if (server->self_ref == LUA_NOREF) {
            lua_pushvalue(L, 1);
            server->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_unlock(L);
        CLEAR_REF(L, server->onServiceRef)
        httpd_server_begin_destroy(server);
    }
    return 0;
}

/* Request data userdata finalizer (fix-plan Phase 3): destroy ws_mutex once
 * the WebSocket connection is fully detached. The deferred-cleanup path
 * guarantees ws_bev == NULL and the self/pin refs dropped before GC can run;
 * a request collected with a live ws_bev would be a leak (log + retain). */
static int lua_evhttp_request_data_gc(lua_State *L) {
    Request *request = (Request *)luaL_checkudata(L, 1, LUA_EVHTTP_REQUEST_DATA_TYPE);
    if (request->ws_mutex_initialized) {
        pthread_mutex_lock(&request->ws_mutex);
        struct bufferevent *bev = request->ws_bev;
        pthread_mutex_unlock(&request->ws_mutex);
        if (bev) {
            LOG_WARN_FMT("WebSocket request collected with a live ws_bev (leak)");
        } else {
            pthread_mutex_destroy(&request->ws_mutex);
            request->ws_mutex_initialized = 0;
        }
    }
    return 0;
}

// ============================================================
// Request __index metamethod
// ============================================================

LUA_API int lua_evhttp_request_lookup(lua_State *L) {
    Request *request = request_from_table(L, 1);
    const char *p = luaL_checkstring(L, 2);

    const luaL_Reg *lib;

    for (lib = evhttp_request_lib; lib->func; lib++) {
        if (strcmp(p, lib->name) == 0) {
            lua_pushcfunction(L, lib->func);
            return 1;
        }
    }

    struct evhttp_request *req = request->req;
    if (!req) {
        return luaL_error(L, "request already closed (connection dropped)");
    }

    if (strcmp(p, "path") == 0) {
        lua_pushstring(L, evhttp_uri_get_path(req->uri_elems));
        return 1;
    } else if (strcmp(p, "query") == 0) {
        lua_pushstring(L, evhttp_uri_get_query(req->uri_elems));
        return 1;
    } else if (strcmp(p, "method") == 0) {
        const MethodMap *method;
        enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
        for (method = methodMap; method->name; method++) {
            if (cmd == method->cmd) {
                lua_pushstring(L, method->name);
                return 1;
            }
        }
    } else if (strcmp(p, "version") == 0) {
        int major = req->major;
        int minor = req->minor;
        if (major < 0 || minor < 0) {
            lua_pushliteral(L, "");
        } else {
            lua_pushfstring(L, "HTTP/%d.%d", major, minor);
        }
        return 1;
    } else if (strcmp(p, "headers") == 0) {
        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);

        lua_newtable(L);
        struct evkeyval *item;
        TAILQ_FOREACH(item, headers, next) {
            lua_getfield(L, -1, item->key);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushstring(L, item->value);
                lua_setfield(L, -2, item->key);
            } else if (lua_isstring(L, -1)) {
                lua_pushfstring(L, "%s, %s", lua_tostring(L, -1), item->value);
                lua_remove(L, -2);
                lua_setfield(L, -2, item->key);
            } else {
                lua_pop(L, 1);
            }
        }
        return 1;
    } else if (strcmp(p, "params") == 0) {
        struct evkeyvalq params;
        evhttp_parse_query_str(evhttp_uri_get_query(req->uri_elems), &params);

        lua_newtable(L);
        struct evkeyval *item;
        TAILQ_FOREACH(item, &params, next) {
            lua_getfield(L, -1, item->key);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushstring(L, item->value);
                lua_setfield(L, -2, item->key);
            } else if (lua_isstring(L, -1)) {
                lua_pushfstring(L, "%s, %s", lua_tostring(L, -1), item->value);
                lua_remove(L, -2);
                lua_setfield(L, -2, item->key);
            } else {
                lua_pop(L, 1);
            }
        }
        evhttp_clear_headers(&params);

        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
        const char *contentType = evhttp_find_header(headers, "Content-Type");
        if (contentType && strstr(contentType, "application/x-www-form-urlencoded") == contentType) {
            request_push_body(L, 1);
            if (lua_type(L, -1) == LUA_TSTRING) {
                const char *data = lua_tostring(L, -1);
                struct evkeyvalq form_params;
                evhttp_parse_query_str(data, &form_params);
                lua_pop(L, 1);

                TAILQ_FOREACH(item, &form_params, next) {
                    lua_getfield(L, -1, item->key);
                    if (lua_isnil(L, -1)) {
                        lua_pop(L, 1);
                        lua_pushstring(L, item->value);
                        lua_setfield(L, -2, item->key);
                    } else if (lua_isstring(L, -1)) {
                        lua_pushfstring(L, "%s, %s", lua_tostring(L, -1), item->value);
                        lua_remove(L, -2);
                        lua_setfield(L, -2, item->key);
                    } else {
                        lua_pop(L, 1);
                    }
                }
                evhttp_clear_headers(&form_params);
            } else {
                lua_pop(L, 1);
            }
        }
        return 1;
    } else if (strcmp(p, "body") == 0) {
        request_push_body(L, 1);
        return 1;
    } else if (strcmp(p, "worker_id") == 0) {
        lua_pushinteger(L, request->worker_id);
        return 1;
    } else if (strcmp(p, "remoteip") == 0) {
        char *address = NULL;
        ev_uint16_t port = 0;
        evhttp_connection_get_peer(req->evcon, &address, &port);

        lua_pushstring(L, address);

        return 1;
    } else if (strcmp(p, "remoteport") == 0) {
        char *address = NULL;
        ev_uint16_t port = 0;
        evhttp_connection_get_peer(req->evcon, &address, &port);

        lua_pushinteger(L, port);

        return 1;
    }

    return 0;
}

// ============================================================
// SSL bevcb
// ============================================================

#if FAN_HAS_OPENSSL

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
static struct bufferevent *bevcb(struct event_base *base, void *arg) {
    struct bufferevent *r;
    SSL_CTX *ctx = (SSL_CTX *)arg;

    r = bufferevent_openssl_socket_new(base, -1, SSL_new(ctx), BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
    return r;
}
#endif
#endif

#endif

static void httpd_configure_instance(LuaServer *server, struct evhttp *httpd,
                                      void *tls_ctx) {
    evhttp_set_timeout(httpd, server->keep_alive_timeout + 30);
    evhttp_set_allowed_methods(httpd,
        EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
        EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS |
        EVHTTP_REQ_TRACE | EVHTTP_REQ_CONNECT | EVHTTP_REQ_PATCH);
    evhttp_set_cb(httpd, "/smoketest", smoke_request_cb, NULL);
    evhttp_set_cb(httpd, "/metrics", metrics_request_cb, NULL);
    evhttp_set_gencb(httpd, (void (*)(struct evhttp_request *, void *))httpd_handler_cgi_bin, server);
#if FAN_HAS_OPENSSL
    if (tls_ctx) {
        evhttp_set_bevcb(httpd, bevcb, tls_ctx);
    }
#else
    (void)tls_ctx;
#endif
}


// ============================================================
// Configuration validation
// ============================================================

static int validate_httpd_config(LuaServer *server) {
    if (!server) return 0;

    if (server->keep_alive_timeout < 1 || server->keep_alive_timeout > 3600) {
        return 0;
    }

    if (server->max_keep_alive_requests < 1 || server->max_keep_alive_requests > 10000) {
        return 0;
    }

    if (server->max_body_size < 1024 || server->max_body_size > 1073741824) {
        return 0;
    }

    if (server->port < 0 || server->port > 65535) {
        return 0;
    }

    return 1;
}

// ============================================================
// Server bind and rebind
// ============================================================

static struct evhttp_bound_socket *httpd_bind_on_base(
    struct evhttp *httpd, struct event_base *base, const char *host, int port,
    int reuse_port) {
    struct evutil_addrinfo hints;
    struct evutil_addrinfo *answer = NULL;
    char portbuf[16];
    memset(&hints, 0, sizeof(hints));
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
    if (evutil_getaddrinfo(host, portbuf, &hints, &answer) != 0 || !answer) return NULL;
    struct evconnlistener *listener = NULL;
    for (struct evutil_addrinfo *ai = answer; ai; ai = ai->ai_next) {
        evutil_socket_t fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        if (reuse_port && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
            evutil_closesocket(fd);
            continue;
        }
#else
        if (reuse_port) {
            evutil_closesocket(fd);
            continue;
        }
#endif
        evutil_make_socket_nonblocking(fd);
        if (bind(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0
            || listen(fd, 128) != 0) {
            evutil_closesocket(fd);
            continue;
        }
        listener = evconnlistener_new(base, NULL, NULL,
            LEV_OPT_CLOSE_ON_FREE, 0, fd);
        if (listener) break;
        evutil_closesocket(fd);
    }
    evutil_freeaddrinfo(answer);
    if (!listener) return NULL;
    struct evhttp_bound_socket *bound = evhttp_bind_listener(httpd, listener);
    if (!bound) evconnlistener_free(listener);
    return bound;
}

int httpd_server_rebind(LuaServer *server) {
    if (!server || !server->httpd) {
        return 0;
    }
    if (server->distribute_connections && server->workers) {
        int requested_port = server->port;
        for (int i = 0; i < server->worker_count; i++) {
            if (server->workers[i].boundsocket) {
                evhttp_del_accept_socket(server->workers[i].httpd, server->workers[i].boundsocket);
                server->workers[i].boundsocket = NULL;
            }
        }
        for (int i = 0; i < server->worker_count; i++) {
            server->workers[i].boundsocket = httpd_bind_on_base(
                server->workers[i].httpd, event_mgr_worker_base(i), server->host,
                requested_port, 1);
            if (!server->workers[i].boundsocket) {
                server->bind_errno = errno;
                return 0;
            }
            if (i == 0) {
                server->port = regress_get_socket_port(
                    evhttp_bound_socket_get_fd(server->workers[i].boundsocket));
                if (server->port < 0) return 0;
                requested_port = server->port;
            }
        }
        server->boundsocket = NULL;
        server->bind_errno = 0;
        return 1;
    }
    if (server->boundsocket) {
        evhttp_del_accept_socket(server->httpd, server->boundsocket);
        server->boundsocket = NULL;
    }

    int requested_port = server->port;
    struct evhttp_bound_socket *boundsocket =
        evhttp_bind_socket_with_handle(server->httpd, server->host, requested_port);

    server->boundsocket = boundsocket;
    if (!boundsocket) {
        server->bind_errno = errno;
        server->port = 0;
        LOG_ERROR_FMT("HTTP server bind failed host=%s port=%d errno=%d (%s)",
                      server->host ? server->host : "(null)",
                      requested_port,
                      server->bind_errno,
                      strerror(server->bind_errno));
        return 0;
    }

    server->bind_errno = 0;
    server->port = regress_get_socket_port(evhttp_bound_socket_get_fd(boundsocket));
    if (server->port < 0) {
        LOG_ERROR_FMT("HTTP server bound but failed to determine port host=%s requested_port=%d",
                      server->host ? server->host : "(null)", requested_port);
        evhttp_del_accept_socket(server->httpd, boundsocket);
        server->boundsocket = NULL;
        server->port = 0;
        server->bind_errno = EIO;
        return 0;
    }
    return 1;
}

LUA_API int utd_bind(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    LuaServer *server = (LuaServer *)lua_newuserdata(L, sizeof(LuaServer));
    memset(server, 0, sizeof(LuaServer));
    luaL_getmetatable(L, LUA_EVHTTP_SERVER_TYPE);
    lua_setmetatable(L, -2);

    server->mainthread = utlua_mainthread(L);
    server->onServiceRef = LUA_NOREF;
    server->boundsocket = NULL;
    server->httpd = NULL;
    server->worker_id = -1;
    server->worker_specified = 0;
    server->distribute_connections = 0;
    server->workers = NULL;
    server->worker_count = 0;
    atomic_init(&server->next_worker, 0);
    pthread_mutex_init(&server->accept_mutex, NULL);
    server->pending_accepts = 0;
    server->accepting = 0;
    atomic_init(&server->life_state, HTTPD_LIVE);
    server->teardown_remaining = 0;
    server->ws_list = NULL;
    server->instance_ws = NULL;
    server->instance_accepts = NULL;
    server->instance_ws_len = 0;
    server->self_ref = LUA_NOREF;

    lua_getfield(L, 1, "worker");
    if (!lua_isnil(L, -1)) {
        server->worker_specified = 1;
        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "HTTP server worker must be an integer");
        }
        server->worker_id = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    if (server->worker_id < -1 ||
        (server->worker_id >= 0 && server->worker_id >= event_mgr_worker_count())) {
        return luaL_error(L, "HTTP server worker is unavailable");
    }

    server->enable_keep_alive = 1;
    server->keep_alive_timeout = 30;
    server->max_keep_alive_requests = 100;
    server->max_body_size = HTTP_POST_BODY_LIMIT;

    lua_getfield(L, 1, "enable_keep_alive");
    if (lua_isboolean(L, -1)) {
        server->enable_keep_alive = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "keep_alive_timeout");
    if (lua_isnumber(L, -1)) {
        server->keep_alive_timeout = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "max_keep_alive_requests");
    if (lua_isnumber(L, -1)) {
        server->max_keep_alive_requests = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "max_body_size");
    if (lua_isnumber(L, -1)) {
        server->max_body_size = (size_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    if (!validate_httpd_config(server)) {
        LOG_ERROR_FMT("Invalid HTTP server configuration host=%s port=%d keep_alive_timeout=%d max_keep_alive_requests=%d max_body_size=%zu",
                      server->host ? server->host : "(null)",
                      server->port,
                      server->keep_alive_timeout,
                      server->max_keep_alive_requests,
                      server->max_body_size);
        return luaL_error(L, "Invalid server configuration parameters");
    }

    struct event_base *http_base =
        server->worker_id >= 0 ? event_mgr_worker_base(server->worker_id)
                               : event_mgr_base();
    struct evhttp *httpd = evhttp_new(http_base);
    if (!httpd) {
        LOG_ERROR_FMT("Failed to create HTTP server host=%s port=%d",
                      server->host ? server->host : "(null)", server->port);
        return luaL_error(L, "Failed to create HTTP server");
    }

#if FAN_HAS_OPENSSL
    server->ctx = NULL;

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
    lua_getfield(L, 1, "cert");
    const char *cert = lua_tostring(L, -1);
    lua_getfield(L, 1, "key");
    const char *key = lua_tostring(L, -1);

    if (cert && key) {
#if OPENSSL_VERSION_NUMBER < 0x1010000fL
        SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
#else
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
#endif
        if (!ctx) {
            lua_pop(L, 2);
            evhttp_free(httpd);
            return luaL_error(L, "Failed to create SSL context");
        }
        server->ctx = ctx;
        SSL_CTX_set_options(ctx,
                            SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE |
                            SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                            SSL_OP_NO_COMPRESSION |
                            SSL_OP_CIPHER_SERVER_PREFERENCE);

        SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS");

#if OPENSSL_VERSION_NUMBER < 0x30000000L
        EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!ecdh) {
            SSL_CTX_free(ctx);
            server->ctx = NULL;
            lua_pop(L, 2);
            evhttp_free(httpd);
            return luaL_error(L, "Failed to create ephemeral ECDH key");
        }

        if (1 != SSL_CTX_set_tmp_ecdh(ctx, ecdh)) {
            EC_KEY_free(ecdh);
            SSL_CTX_free(ctx);
            server->ctx = NULL;
            lua_pop(L, 2);
            evhttp_free(httpd);
            return luaL_error(L, "Failed to configure ephemeral ECDH key");
        }
        EC_KEY_free(ecdh);
#endif

        if (!server_setup_certs(ctx, cert, key)) {
            SSL_CTX_free(ctx);
            server->ctx = NULL;
            lua_pop(L, 2);
            evhttp_free(httpd);
            return luaL_error(L, "Failed to load TLS certificate or private key");
        }

        evhttp_set_bevcb(httpd, bevcb, ctx);
    }

    lua_pop(L, 2);
#endif
#endif

#endif

    DUP_STR_FROM_TABLE(L, server->host, 1, "host")
    SET_INT_FROM_TABLE(L, server->port, 1, "port")

    server->httpd = httpd;
    SET_FUNC_REF_FROM_TABLE(L, server->onServiceRef, 1, "onService")
    httpd_configure_instance(server, httpd, server->ctx);

    if (!server->worker_specified && event_mgr_worker_count() > 0) {
        server->worker_count = event_mgr_worker_count();
        server->workers = calloc((size_t)server->worker_count, sizeof(*server->workers));
        if (!server->workers) {
            CLEAR_REF(L, server->onServiceRef)
            httpd_server_free_after_bind_error(server);
            return luaL_error(L, "Failed to allocate HTTP worker instances");
        }
        server->distribute_connections = 1;
        for (int i = 0; i < server->worker_count; i++) {
            server->workers[i].httpd = evhttp_new(event_mgr_worker_base(i));
            if (!server->workers[i].httpd) {
                CLEAR_REF(L, server->onServiceRef)
                httpd_server_free_after_bind_error(server);
                return luaL_error(L, "Failed to create HTTP worker instance");
            }
            server->workers[i].boundsocket = NULL;
            httpd_configure_instance(server, server->workers[i].httpd, server->ctx);
        }
    }

    /* Per-owner WebSocket counters, index = worker_id + 1 (accept_mutex).
     * Distributed mode has one owner slot per worker; single-instance mode
     * has one slot for main or the selected worker. */
    size_t ws_count = 1;
    if (server->distribute_connections) {
        ws_count = (size_t)server->worker_count;
    } else if (server->worker_specified && server->worker_id >= 0) {
        ws_count = (size_t)server->worker_id + 2;
    }
    server->instance_ws = calloc(ws_count, sizeof(unsigned int));
    server->instance_accepts = calloc(ws_count, sizeof(unsigned int));
    if (!server->instance_ws || !server->instance_accepts) {
        CLEAR_REF(L, server->onServiceRef)
        httpd_server_free_after_bind_error(server);
        return luaL_error(L, "Failed to allocate HTTP server state");
    }
    server->instance_ws_len = (int)ws_count;

    if (!httpd_server_rebind(server)) {
        CLEAR_REF(L, server->onServiceRef)
        httpd_server_free_after_bind_error(server);
        return luaL_error(L, "HTTP server bind failed for %s:%d: %s",
                          server->host ? server->host : "(null)",
                          server->port,
                          strerror(server->bind_errno ? server->bind_errno : EADDRINUSE));
    }
    pthread_mutex_lock(&server->accept_mutex);
    server->accepting = server->distribute_connections;
    server->accept_high_water = server->distribute_connections
        ? (server->worker_count * 16 > 64 ? server->worker_count * 16 : 64)
        : 0;
    server->listener_paused = 0;
    server->pending_resume = 0;
    pthread_mutex_unlock(&server->accept_mutex);

    metrics_init();

    lua_newtable(L);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "serv");

    lua_pushinteger(L, server->port);
    lua_setfield(L, -2, "port");

    if (server->host) {
        lua_getfield(L, 1, "host");
    } else {
        lua_pushliteral(L, "0.0.0.0");
    }
    lua_setfield(L, -2, "host");

    return 1;
}

LUA_API int lua_evhttp_server_rebind(lua_State *L) {
    LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
    /* Rebind is only meaningful while the server is live (fix-plan §2.6). */
    if ((httpd_life_state_t)atomic_load(&server->life_state) != HTTPD_LIVE) {
        return luaL_error(L, "HTTP server is not live (draining or closed)");
    }
    if (!httpd_server_rebind(server)) {
        return luaL_error(L, "HTTP server rebind failed for %s:%d: %s",
                          server->host ? server->host : "(null)",
                          server->port,
                          strerror(server->bind_errno ? server->bind_errno : EADDRINUSE));
    }
    return 0;
}

// ============================================================
// Lua module registration
// ============================================================

static const luaL_Reg utdlib[] = {{"bind", utd_bind}, {NULL, NULL}};

LUA_API int luaopen_fan_httpd_core(lua_State *L) {
    luaL_newmetatable(L, LUA_EVHTTP_REQUEST_DATA_TYPE);

    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, &lua_evhttp_request_data_gc);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    luaL_newmetatable(L, LUA_EVHTTP_REQUEST_TYPE);

    lua_pushstring(L, "__index");
    lua_pushcfunction(L, &lua_evhttp_request_lookup);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    luaL_newmetatable(L, LUA_EVHTTP_SERVER_TYPE);

    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, &lua_evhttp_server_gc);
    lua_rawset(L, -3);

    lua_pushcfunction(L, &lua_evhttp_server_rebind);
    lua_setfield(L, -2, "rebind");

    lua_pushcfunction(L, &lua_evhttp_server_close);
    lua_setfield(L, -2, "close");

    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    lua_newtable(L);
    luaL_register(L, "httpd", utdlib);

    return 1;
}
