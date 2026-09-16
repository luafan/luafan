// httpd_request.c — Body reading, reply functions, chunked transfer

#include "httpd_internal.h"
#include <string.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/http_struct.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* Owner-thread affinity helpers (defined in the reply section below). */
static int reply_on_owner_thread(Request *request);
static int reply_wrong_thread_error(lua_State *L, Request *request, const char *what);

// ============================================================
// Body reading
// ============================================================

int request_push_body(lua_State *L, int idx) {
    if (idx < 0) {
        idx = lua_gettop(L) + idx + 1;
    }
    Request *request = request_from_table(L, idx);
    if (!reply_on_owner_thread(request)) {
        return reply_wrong_thread_error(L, request, "request body access");
    }
    lua_rawgetp(L, idx, "body");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);

        struct evhttp_request *req = request->req;
        if (!req) {
            lua_pushnil(L);
            return 1;
        }
        struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
        if (bodybuf) {
            size_t len = evbuffer_get_length(bodybuf);

            size_t body_limit = HTTP_POST_BODY_LIMIT;
            if (request->server && request->server->max_body_size) {
                body_limit = request->server->max_body_size;
            }

            if (len > 0 && len <= body_limit) {
                char *data = calloc(1, len + 1);
                if (!data) {
                    LOG_ERROR_FMT("Memory allocation failed for request body: %zu bytes", len + 1);
                    return luaL_error(L, "Failed to allocate request body");
                }

                size_t total_read = 0;
                char *ptrdata = data;
                while (total_read < len) {
                    int read = evbuffer_remove(bodybuf, ptrdata, len - total_read);
                    if (read <= 0) break;
                    ptrdata += read;
                    total_read += read;
                }

                if (total_read != len) {
                    LOG_ERROR_FMT("Request body read failed: expected=%zu read=%zu", len, total_read);
                    free(data);
                    lua_pushnil(L);
                    return 1;
                }

                lua_pushlstring(L, data, total_read);
                free(data);

#if (LUA_VERSION_NUM >= 502)
                lua_pushvalue(L, -1);
                lua_rawsetp(L, idx, "body");
#else
                lua_pushliteral(L, "body");
                lua_pushvalue(L, -2);
                lua_rawset(L, idx);
#endif
                return 1;
            }
        }

        lua_pushnil(L);
    }
    return 1;
}

LUA_API int lua_evhttp_request_available(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!reply_on_owner_thread(request)) {
        return reply_wrong_thread_error(L, request, "request:available()");
    }
    struct evhttp_request *req = request->req;
    if (!req) {
        lua_pushinteger(L, 0);
        return 1;
    }
    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
    if (bodybuf) {
        size_t len = evbuffer_get_length(bodybuf);
        lua_pushinteger(L, len);
    } else {
        lua_pushinteger(L, 0);
    }

    return 1;
}

LUA_API int lua_evhttp_request_read(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!reply_on_owner_thread(request)) {
        return reply_wrong_thread_error(L, request, "request:read()");
    }
    struct evhttp_request *req = request->req;
    if (!req) {
        lua_pushnil(L);
        return 1;
    }
    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
    if (!bodybuf) {
        lua_pushnil(L);
        return 1;
    }
    size_t evbuffer_length = evbuffer_get_length(bodybuf);

    if (evbuffer_length) {
        lua_Integer buff_len = luaL_optinteger(L, 2, MIN(READ_BUFF_LEN, evbuffer_length));

        if (buff_len <= 0 || buff_len > MAX_READ_BUFFER_SIZE) {
            lua_pushnil(L);
            return 1;
        }

        char *data = malloc(buff_len);
        if (!data) {
            LOG_ERROR_FMT("Memory allocation failed for read buffer: %ld bytes", (long)buff_len);
            lua_pushnil(L);
            return 1;
        }

        int read = evbuffer_remove(bodybuf, data, buff_len);
        if (read > 0) {
            lua_pushlstring(L, data, read);
        } else {
            lua_pushnil(L);
        }
        free(data);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

// ============================================================
// Reply functions
// ============================================================

/* ---- owner-thread discipline --------------------------------------------
 *
 * The plain-HTTP reply/read state (request->req, its output headers and the
 * connection's bufferevent) belongs to the event base that accepted the
 * request. A handler coroutine, however, can be resumed on a different thread
 * than the owner one: fan.sleep() parks on the main base, a fan.mariadb
 * connection has its own worker affinity, a tcpd/udpd callback fires on its
 * connection's owner, ...
 *
 * Calling evhttp_send_reply*() from such a thread would race the owner loop on
 * the same evbuffer (heap corruption), so:
 *
 *  - reply()/reply_start()/reply_chunk()/reply_end()/addheader() called from a
 *    foreign thread enqueue a copy of their arguments into a per-request FIFO
 *    (httpd_reply_op) and ask the owner loop to apply them in order
 *    (httpd_reply_drain_cb). The API stays synchronous for the caller; only
 *    the wire write happens later, on the owner thread.
 *  - read()/available()/body consume the request's input buffer and have no
 *    marshaled form, so they fail loudly when called from a foreign thread.
 */

static int reply_on_owner_thread(Request *request) {
    return event_mgr_is_current_owner(request->worker_id);
}

/* Reply status as the calling thread must see it. reply_pending_status is the
 * "applied ∪ queued" view of reply_status: the owner thread advances it when
 * it applies an operation, a foreign thread advances it when it enqueues one,
 * and the drain resyncs it to the applied reply_status once its queue is empty.
 * Without it, reply_status would still be stale (NONE) while a marshaled
 * reply_start sits in the queue, and a following marshaled reply_chunk on the
 * same foreign thread would abort with "reply has not started yet".
 * Guarded by ws_mutex. */
static int reply_effective_status(Request *request) {
    pthread_mutex_lock(&request->ws_mutex);
    int status = request->reply_pending_status;
    pthread_mutex_unlock(&request->ws_mutex);
    return status;
}

/* Publish a reply-state transition to foreign threads. Used by the owner-thread
 * implementation functions so an inline reply and a marshaled one on the same
 * request keep the same caller-visible status. */
static void reply_note_status(Request *request, int status) {
    pthread_mutex_lock(&request->ws_mutex);
    request->reply_pending_status = status;
    pthread_mutex_unlock(&request->ws_mutex);
}

static int reply_wrong_thread_error(lua_State *L, Request *request, const char *what) {
    return luaL_error(L,
                      "%s must run on the connection's owner worker (worker_id=%d); "
                      "calling it from another thread would race the owner event loop",
                      what, request->worker_id);
}

/* ---- owner-thread implementations (shared by the inline and the marshaled
 *      path) -------------------------------------------------------------- */

static void do_addheader(Request *request, const char *key, const char *value) {
    evhttp_add_header(request->req->output_headers, key, value);
}

static void do_reply_start(Request *request, int responseCode, const char *responseMessage) {
    LuaServer *server = request->server;
    if (server) {
        set_connection_header(request->req, server);
    } else {
        evhttp_add_header(request->req->output_headers, "Connection", "close");
    }
    evhttp_send_reply_start(request->req, responseCode, responseMessage);
    request->response_code = responseCode;
    request->reply_status = REPLY_STATUS_REPLY_START;
    reply_note_status(request, REPLY_STATUS_REPLY_START);
}

/* Emits one HTTP chunk whose payload the caller already collected in *buf
 * (possibly empty, like the inline API always did). *buf is always consumed.
 * 0 = sent, 1 = peer/connection already gone. */
static int do_reply_chunk(Request *request, struct evbuffer *buf) {
    struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
    if (!evcon) {
        evbuffer_free(buf);
        request->reply_status = REPLY_STATUS_REPLYED;
        reply_note_status(request, REPLY_STATUS_REPLYED);
        return 1;
    }

    struct bufferevent *bev = evhttp_connection_get_bufferevent(evcon);
    if (bev) {
        evutil_socket_t fd = bufferevent_getfd(bev);
        if (fd >= 0) {
            char peek_buf[1];
            ssize_t n = recv(fd, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                evbuffer_free(buf);
                request->reply_status = REPLY_STATUS_REPLYED;
                reply_note_status(request, REPLY_STATUS_REPLYED);
                return 1;
            }
        }
    }

    evhttp_send_reply_chunk(request->req, buf);
    evbuffer_free(buf);
    return 0;
}

static void do_reply_end(Request *request) {
    struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
    if (evcon) {
        evhttp_send_reply_end(request->req);
    }
    request->reply_status = REPLY_STATUS_REPLYED;
    reply_note_status(request, REPLY_STATUS_REPLYED);
    httpd_release_conn_guard(request);
    httpd_finish_metrics(request, request->response_code, 0);
}

/* 0 = sent, 1 = response buffer could not be created/filled (nothing sent). */
static int do_reply_full(Request *request, int responseCode, const char *responseMessage,
                         const char *responseBuff, size_t responseBuffLen) {
    LuaServer *server = request->server;
    if (server) {
        set_connection_header(request->req, server);
    } else {
        evhttp_add_header(request->req->output_headers, "Connection", "close");
    }

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        LOG_ERROR_FMT("Failed to create response buffer for request");
        return 1;
    }

    if (responseBuff && responseBuffLen > 0) {
        if (evbuffer_add(buf, responseBuff, responseBuffLen) < 0) {
            evbuffer_free(buf);
            LOG_ERROR_FMT("Failed to add %zu bytes to response buffer", responseBuffLen);
            return 2;
        }
    }

    evhttp_send_reply(request->req, responseCode, responseMessage, buf);
    evbuffer_free(buf);

    request->response_code = responseCode;
    request->reply_status = REPLY_STATUS_REPLYED;
    reply_note_status(request, REPLY_STATUS_REPLYED);
    httpd_release_conn_guard(request);

    httpd_finish_metrics(request, responseCode, responseBuffLen);
    return 0;
}

/* ---- marshaled reply queue ---------------------------------------------- */

static void httpd_reply_op_free(httpd_reply_op_t *op) {
    free(op->a);
    free(op->b);
    free(op);
}

static void httpd_reply_ops_free(httpd_reply_op_t *op) {
    while (op) {
        httpd_reply_op_t *next = op->next;
        httpd_reply_op_free(op);
        op = next;
    }
}

static httpd_reply_op_t *httpd_reply_op_new(int kind, int code,
                                            const char *a, size_t a_len,
                                            const char *b, size_t b_len) {
    httpd_reply_op_t *op = calloc(1, sizeof(*op));
    if (!op) {
        return NULL;
    }
    op->kind = kind;
    op->code = code;
    /* a and b carry C strings for ADDHEADER / REPLY_START / REPLY (message,
     * header key and value) which the owner-thread apply path passes straight
     * to libevent's evhttp_add_header / evhttp_send_reply*. Those treat them as
     * NUL-terminated, so every copied string gets a trailing NUL. For the
     * payload op (REPLY body / CHUNK) the length is used, and the extra NUL is
     * harmless. */
    if (a && a_len > 0) {
        op->a = malloc(a_len + 1);
        if (!op->a) {
            free(op);
            return NULL;
        }
        memcpy(op->a, a, a_len);
        op->a[a_len] = '\0';
        op->a_len = a_len;
    }
    if (b && b_len > 0) {
        op->b = malloc(b_len + 1);
        if (!op->b) {
            free(op->a);
            free(op);
            return NULL;
        }
        memcpy(op->b, b, b_len);
        op->b[b_len] = '\0';
        op->b_len = b_len;
    }
    return op;
}

/* Runs on the connection's owner thread: applies one queued reply operation.
 * Violations of the reply state machine are logged instead of raised — the
 * calling Lua coroutine is long gone by now. */
static void httpd_reply_op_apply(Request *request, httpd_reply_op_t *op) {
    switch (op->kind) {
        case HTTPD_REPLY_OP_ADDHEADER:
            if (!request->req || request->reply_status != REPLY_STATUS_NONE) {
                LOG_WARN_FMT("marshaled addheader dropped (status=%d)", request->reply_status);
                return;
            }
            do_addheader(request, op->a, op->b);
            return;
        case HTTPD_REPLY_OP_REPLY_START:
            if (!request->req) {
                return;
            }
            if (request->reply_status != REPLY_STATUS_NONE) {
                LOG_WARN_FMT("marshaled reply_start dropped (status=%d)", request->reply_status);
                return;
            }
            do_reply_start(request, op->code, op->a);
            return;
        case HTTPD_REPLY_OP_REPLY_CHUNK: {
            if (!request->req) {
                return;
            }
            if (request->reply_status != REPLY_STATUS_REPLY_START) {
                LOG_WARN_FMT("marshaled reply_chunk dropped (status=%d)", request->reply_status);
                return;
            }
            struct evbuffer *buf = evbuffer_new();
            if (!buf) {
                LOG_ERROR_FMT("marshaled reply_chunk: failed to create chunk buffer");
                return;
            }
            if (op->a && op->a_len > 0 && evbuffer_add(buf, op->a, op->a_len) < 0) {
                evbuffer_free(buf);
                LOG_ERROR_FMT("marshaled reply_chunk: failed to fill chunk buffer");
                return;
            }
            if (do_reply_chunk(request, buf) != 0) {
                LOG_WARN_FMT("marshaled reply_chunk not sent (peer closed)");
            }
            return;
        }
        case HTTPD_REPLY_OP_REPLY_END:
            if (!request->req) {
                request->reply_status = REPLY_STATUS_REPLYED;
                reply_note_status(request, REPLY_STATUS_REPLYED);
                return;
            }
            if (request->reply_status != REPLY_STATUS_REPLY_START) {
                LOG_WARN_FMT("marshaled reply_end dropped (status=%d)", request->reply_status);
                return;
            }
            do_reply_end(request);
            return;
        case HTTPD_REPLY_OP_REPLY:
        default:
            if (!request->req) {
                return;
            }
            if (request->reply_status == REPLY_STATUS_REPLY_START) {
                /* reply() on a started chunked reply completes it. */
                do_reply_end(request);
                return;
            }
            if (request->reply_status != REPLY_STATUS_NONE) {
                LOG_WARN_FMT("marshaled reply dropped (status=%d)", request->reply_status);
                return;
            }
            if (do_reply_full(request, op->code, op->a, op->b, op->b_len) != 0) {
                LOG_WARN_FMT("marshaled reply could not be sent");
            }
            return;
    }
}

/* Owner-thread drain: apply every queued operation in FIFO order, then release
 * the batch's registry pin of the request userdata. */
static void httpd_reply_drain_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd;
    (void)what;
    Request *request = (Request *)arg;

    pthread_mutex_lock(&request->ws_mutex);
    httpd_reply_op_t *batch = request->reply_head;
    request->reply_head = NULL;
    request->reply_tail = NULL;
    request->reply_queued = 0;
    /* reply_pending_status intentionally keeps the batch's final state while we
     * apply: an enqueue landing in this window must still observe the
     * transition this batch is about to perform. Resynced below. */
    int ref = request->reply_chain_ref;
    request->reply_chain_ref = LUA_NOREF;
    pthread_mutex_unlock(&request->ws_mutex);

    for (httpd_reply_op_t *op = batch; op; op = op->next) {
        httpd_reply_op_apply(request, op);
    }
    httpd_reply_ops_free(batch);

    /* Resync the caller-visible status with what was actually applied — but
     * only when no new operation was queued while we applied (its enqueue
     * already advanced the pending view past this batch). */
    pthread_mutex_lock(&request->ws_mutex);
    if (!request->reply_queued) {
        request->reply_pending_status = request->reply_status;
    }
    pthread_mutex_unlock(&request->ws_mutex);

    if (ref != LUA_NOREF && request->mainthread) {
        CLEAR_REF(request->mainthread, ref);
    }
}

/* Queue one reply operation from a non-owner Lua thread. Lua calls
 * (lua_rawgeti/luaL_ref/unref through CLEAR_REF) deliberately happen OUTSIDE
 * ws_mutex: owner-thread Lua callbacks take the global Lua lock and then
 * briefly take ws_mutex, so holding ws_mutex across Lua calls here would invert
 * the lock order and can deadlock.
 * Returns 0 when the operation is queued, -1 when it had to be dropped. */
static int reply_enqueue(lua_State *L, Request *request, httpd_reply_op_t *op) {
    op->next = NULL;

    /* Pin the request userdata so it cannot be collected while queued
     * operations or the armed drain job still reference it. */
    lua_rawgeti(L, 1, 1);
    int pin = luaL_ref(L, LUA_REGISTRYINDEX);
    if (pin == LUA_NOREF) {
        httpd_reply_op_free(op);
        return -1;
    }

    pthread_mutex_lock(&request->ws_mutex);
    if (request->reply_tail) {
        request->reply_tail->next = op;
    } else {
        request->reply_head = op;
    }
    request->reply_tail = op;
    /* Advance the caller-visible status so later marshaled calls on the same
     * thread observe the queued transition. */
    if (op->kind == HTTPD_REPLY_OP_REPLY_START) {
        request->reply_pending_status = REPLY_STATUS_REPLY_START;
    } else if (op->kind == HTTPD_REPLY_OP_REPLY_END ||
               op->kind == HTTPD_REPLY_OP_REPLY) {
        request->reply_pending_status = REPLY_STATUS_REPLYED;
    }
    if (!request->reply_queued) {
        request->reply_queued = 1;
        request->reply_chain_ref = pin;
        pin = LUA_NOREF; /* ownership moved to the request; we arm the drain */
    }
    pthread_mutex_unlock(&request->ws_mutex);

    if (pin != LUA_NOREF) {
        /* Another thread armed the drain first; drop our duplicate pin. */
        CLEAR_REF(L, pin);
        return 0;
    }

    if (event_mgr_worker_once(request->worker_id, httpd_reply_drain_cb, request) != 0) {
        /* Owner loop unreachable (stopped or shutting down): nothing can ever
         * run this batch. Drop it instead of pinning the request forever. */
        pthread_mutex_lock(&request->ws_mutex);
        httpd_reply_op_t *batch = request->reply_head;
        request->reply_head = NULL;
        request->reply_tail = NULL;
        request->reply_queued = 0;
        request->reply_pending_status = REPLY_STATUS_REPLYED;
        int ref = request->reply_chain_ref;
        request->reply_chain_ref = LUA_NOREF;
        pthread_mutex_unlock(&request->ws_mutex);
        httpd_reply_ops_free(batch);
        CLEAR_REF(L, ref);
        return -1;
    }
    return 0;
}

LUA_API int lua_evhttp_request_reply(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!reply_on_owner_thread(request)) {
        /* reply_pending_status is maintained by Lua-facing calls only, so this
         * check is safe from a foreign thread (see reply_effective_status). */
        if (reply_effective_status(request) == REPLY_STATUS_REPLYED) {
            return luaL_error(L, "reply has completed already.");
        }
        int responseCode = (int)lua_tointeger(L, 2);
        const char *responseMessage = lua_tostring(L, 3);
        size_t responseBuffLen = 0;
        const char *responseBuff = lua_tolstring(L, 4, &responseBuffLen);
        httpd_reply_op_t *op = httpd_reply_op_new(
            HTTPD_REPLY_OP_REPLY, responseCode,
            responseMessage, responseMessage ? strlen(responseMessage) : 0,
            responseBuff, responseBuffLen);
        if (!op) {
            return luaL_error(L, "Failed to allocate marshaled reply");
        }
        if (reply_enqueue(L, request, op) != 0) {
            return luaL_error(L, "Failed to queue marshaled reply");
        }
        return 0;
    }

    if (!request->req) {
        return luaL_error(L, "connection closed by peer");
    }
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            do_reply_end(request);
            lua_settop(L, 1);
            return 1;
        default:
            break;
    }

    int responseCode = (int)lua_tointeger(L, 2);
    const char *responseMessage = lua_tostring(L, 3);

    size_t responseBuffLen = 0;
    const char *responseBuff = lua_tolstring(L, 4, &responseBuffLen);

    int rc = do_reply_full(request, responseCode, responseMessage, responseBuff, responseBuffLen);
    if (rc == 1) {
        return luaL_error(L, "Failed to create response buffer");
    }
    if (rc == 2) {
        return luaL_error(L, "Failed to add data to response buffer");
    }

    return 0;
}

LUA_API int lua_evhttp_request_reply_addheader(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!reply_on_owner_thread(request)) {
        switch (reply_effective_status(request)) {
            case REPLY_STATUS_REPLYED:
                return luaL_error(L, "reply has completed already.");
            case REPLY_STATUS_REPLY_START:
                return luaL_error(L, "reply has started already.");
            default:
                break;
        }
        const char *key = luaL_checkstring(L, 2);
        const char *value = luaL_checkstring(L, 3);
        httpd_reply_op_t *op = httpd_reply_op_new(
            HTTPD_REPLY_OP_ADDHEADER, 0, key, strlen(key), value, strlen(value));
        if (!op) {
            return luaL_error(L, "Failed to allocate marshaled addheader");
        }
        if (reply_enqueue(L, request, op) != 0) {
            return luaL_error(L, "Failed to queue marshaled addheader");
        }
        lua_settop(L, 1);
        return 1;
    }

    if (!request->req) {
        return luaL_error(L, "connection closed by peer");
    }
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            return luaL_error(L, "reply has started already.");
        default:
            break;
    }

    const char *key = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);

    do_addheader(request, key, value);

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_start(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!reply_on_owner_thread(request)) {
        switch (reply_effective_status(request)) {
            case REPLY_STATUS_REPLYED:
                return luaL_error(L, "reply has completed already.");
            case REPLY_STATUS_REPLY_START:
                return luaL_error(L, "reply has started already.");
            default:
                break;
        }
        int responseCode = (int)lua_tointeger(L, 2);
        const char *responseMessage = lua_tostring(L, 3);
        httpd_reply_op_t *op = httpd_reply_op_new(
            HTTPD_REPLY_OP_REPLY_START, responseCode,
            responseMessage, responseMessage ? strlen(responseMessage) : 0,
            NULL, 0);
        if (!op) {
            return luaL_error(L, "Failed to allocate marshaled reply_start");
        }
        if (reply_enqueue(L, request, op) != 0) {
            return luaL_error(L, "Failed to queue marshaled reply_start");
        }
        lua_settop(L, 1);
        return 1;
    }

    if (!request->req) {
        return luaL_error(L, "connection closed by peer");
    }
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            return luaL_error(L, "reply has started already.");
        default:
            break;
    }

    int responseCode = (int)lua_tointeger(L, 2);
    const char *responseMessage = lua_tostring(L, 3);
    do_reply_start(request, responseCode, responseMessage);

    lua_settop(L, 1);
    return 1;
}

/* Concatenates the string arguments on the stack [from, top] into a freshly
 * allocated buffer, so one Lua call stays one HTTP chunk on the wire even when
 * several foreign threads enqueue chunks on the same request.
 * Returns 0 on success (*payload_out may stay NULL when the arguments carry no
 * bytes at all) and -1 when the payload could not be allocated. */
static int reply_collect_args(lua_State *L, int from, int top,
                              char **payload_out, size_t *len_out) {
    size_t total = 0;
    for (int i = from; i <= top; i++) {
        size_t len = 0;
        lua_tolstring(L, i, &len);
        total += len;
    }
    *payload_out = NULL;
    *len_out = 0;
    if (total == 0) {
        return 0;
    }
    char *payload = malloc(total);
    if (!payload) {
        return -1;
    }
    size_t off = 0;
    for (int i = from; i <= top; i++) {
        size_t len = 0;
        const char *part = lua_tolstring(L, i, &len);
        if (part && len > 0) {
            memcpy(payload + off, part, len);
            off += len;
        }
    }
    *payload_out = payload;
    *len_out = off;
    return 0;
}

LUA_API int lua_evhttp_request_reply_chunk(lua_State *L) {
    Request *request = request_from_table(L, 1);
    int top = lua_gettop(L);
    if (!reply_on_owner_thread(request)) {
        switch (reply_effective_status(request)) {
            case REPLY_STATUS_REPLYED:
                return luaL_error(L, "reply has completed already.");
            case REPLY_STATUS_NONE:
                return luaL_error(L, "reply has not started yet.");
            default:
                break;
        }
        if (top <= 1) {
            /* No payload: nothing to send, exactly like the inline path. */
            lua_settop(L, 1);
            return 1;
        }
        /* One call is one HTTP chunk on the wire — concatenate all arguments
         * into a single op so interleaved foreign senders cannot split it. */
        char *payload = NULL;
        size_t off = 0;
        if (reply_collect_args(L, 2, top, &payload, &off) != 0) {
            return luaL_error(L, "Failed to allocate marshaled chunk");
        }
        httpd_reply_op_t *op = httpd_reply_op_new(
            HTTPD_REPLY_OP_REPLY_CHUNK, 0, payload, off, NULL, 0);
        free(payload);
        if (!op) {
            return luaL_error(L, "Failed to allocate marshaled chunk");
        }
        if (reply_enqueue(L, request, op) != 0) {
            return luaL_error(L, "Failed to queue marshaled chunk");
        }
        lua_settop(L, 1);
        return 1;
    }

    if (!request->req) {
        request->reply_status = REPLY_STATUS_REPLYED;
        reply_note_status(request, REPLY_STATUS_REPLYED);
        return luaL_error(L, "connection closed by peer");
    }
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_NONE:
            return luaL_error(L, "reply has not started yet.");
        default:
            break;
    }

    if (top > 1) {
        struct evbuffer *buf = evbuffer_new();
        if (!buf) {
            return luaL_error(L, "Failed to create chunk buffer");
        }

        int i = 2;
        for (; i <= top; i++) {
            size_t responseBuffLen = 0;
            const char *responseBuff = lua_tolstring(L, i, &responseBuffLen);
            if (responseBuff && responseBuffLen > 0) {
                if (evbuffer_add(buf, responseBuff, responseBuffLen) < 0) {
                    evbuffer_free(buf);
                    return luaL_error(L, "Failed to add data to chunk buffer");
                }
            }
        }
        /* do_reply_chunk() consumes buf either way. */
        if (do_reply_chunk(request, buf) == 1) {
            return luaL_error(L, "connection closed by peer");
        }
    }

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_end(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!reply_on_owner_thread(request)) {
        switch (reply_effective_status(request)) {
            case REPLY_STATUS_REPLYED:
                return luaL_error(L, "reply has completed already.");
            case REPLY_STATUS_NONE:
                return luaL_error(L, "reply has not started yet.");
            default:
                break;
        }
        httpd_reply_op_t *op = httpd_reply_op_new(HTTPD_REPLY_OP_REPLY_END, 0, NULL, 0, NULL, 0);
        if (!op) {
            return luaL_error(L, "Failed to allocate marshaled reply_end");
        }
        if (reply_enqueue(L, request, op) != 0) {
            return luaL_error(L, "Failed to queue marshaled reply_end");
        }
        lua_settop(L, 1);
        return 1;
    }

    if (!request->req) {
        request->reply_status = REPLY_STATUS_REPLYED;
        reply_note_status(request, REPLY_STATUS_REPLYED);
        lua_settop(L, 1);
        return 1;
    }
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_NONE:
            return luaL_error(L, "reply has not started yet.");
        default:
            break;
    }

    do_reply_end(request);

    lua_settop(L, 1);
    return 1;
}
