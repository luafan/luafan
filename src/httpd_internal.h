#ifndef HTTPD_INTERNAL_H
#define HTTPD_INTERNAL_H

#include "utlua.h"
#include "platform.h"
#include "event_mgr.h"
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http_struct.h>
#include <zlib.h>

#if FAN_HAS_OPENSSL
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#endif

// ============================================================
// Constants
// ============================================================

#define HTTP_POST_BODY_LIMIT (100 * 1024 * 1024)
#define MAX_READ_BUFFER_SIZE (1024 * 1024)

#define REPLY_STATUS_NONE 0
#define REPLY_STATUS_REPLYED 1
#define REPLY_STATUS_REPLY_START 2

#define WEBSOCKET_MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WEBSOCKET_GUID_LEN 36
#define WEBSOCKET_KEY_LEN 24
#define WS_MAX_QUEUED_FRAMES 64

#define LUA_EVHTTP_REQUEST_TYPE "EVHTTP_REQUEST_TYPE"
#define LUA_EVHTTP_REQUEST_DATA_TYPE "EVHTTP_REQUEST_DATA_TYPE"
#define LUA_EVHTTP_SERVER_TYPE "EVHTTP_SERVER_TYPE"

// ============================================================
// Enumerations
// ============================================================

typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT = 0x1,
    WS_OPCODE_BINARY = 0x2,
    WS_OPCODE_CLOSE = 0x8,
    WS_OPCODE_PING = 0x9,
    WS_OPCODE_PONG = 0xA
} websocket_opcode_t;

typedef enum {
    WS_STATE_CONNECTING = 0,
    WS_STATE_OPEN = 1,
    WS_STATE_CLOSING = 2,
    WS_STATE_CLOSED = 3
} websocket_state_t;

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} log_level_t;

// ============================================================
// Structures
// ============================================================

typedef struct {
    int fin;
    int rsv1, rsv2, rsv3;
    websocket_opcode_t opcode;
    int masked;
    uint64_t payload_len;
    uint32_t mask_key;
    char *payload;
} websocket_frame_t;

/* Server lifecycle state machine (see docs/threading-fix-plan.md Phase 1).
 * LIVE      — accepting + serving
 * DRAINING  — destroy requested; owner-thread teardown jobs in progress
 * GONE      — native resources released; __gc is idempotent
 */
typedef enum {
    HTTPD_LIVE = 0,
    HTTPD_DRAINING = 1,
    HTTPD_GONE = 2
} httpd_life_state_t;

typedef struct ws_frame_node {
    websocket_frame_t frame;
    struct ws_frame_node *next;
} ws_frame_node_t;

typedef struct httpd_worker_instance {
    struct evhttp *httpd;
    struct evhttp_bound_socket *boundsocket;
} httpd_worker_instance_t;

/* Forward declaration: LuaServer embeds an intrusive list of live
 * WebSocket Requests (Request is defined below). */
typedef struct Request Request;

typedef struct {
    lua_State *mainthread;
    struct evhttp *httpd;
    struct evhttp_bound_socket *boundsocket;
    char *host;
    int port;
    int worker_id;
    int worker_specified;
    int distribute_connections;
    httpd_worker_instance_t *workers;
    int worker_count;
    _Atomic unsigned int next_worker;
    pthread_mutex_t accept_mutex;
    unsigned int pending_accepts;
    int accepting;
    int accept_high_water;           /* backpressure threshold (accept_mutex); see fix-plan Phase 2 */
    int listener_paused;             /* main listener disabled above high-water (accept_mutex) */
    int pending_resume;              /* worker requested a resume; cleared on main base (accept_mutex) */
    _Atomic int life_state;          /* httpd_life_state_t (see fix-plan Phase 1) */
    unsigned int teardown_remaining; /* evhttp instances not yet freed (accept_mutex) */
    Request *ws_list;                /* live WebSocket requests, intrusive (accept_mutex) */
    unsigned int *instance_ws;       /* ws count per owner, idx = worker_id+1 (accept_mutex) */
    unsigned int *instance_accepts;  /* in-flight accept jobs per owner, idx = worker_id+1 (accept_mutex) */
    int instance_ws_len;
    int self_ref;
#if FAN_HAS_OPENSSL
    SSL_CTX *ctx;
#endif
    int onServiceRef;
    int enable_keep_alive;
    int keep_alive_timeout;
    int max_keep_alive_requests;
    size_t max_body_size;
    int bind_errno;
} LuaServer;

struct Request {
    struct evhttp_request *req;
    LuaServer *server;
    int worker_id;
    int reply_status;
    int response_code;
    int metrics_finished;
    int is_websocket;
    websocket_state_t ws_state;
    struct bufferevent *ws_bev;
    struct bufferevent *ws_deferred_bev;
    pthread_mutex_t ws_mutex;
    int ws_mutex_initialized;
    unsigned int ws_pending_sends;
    lua_State *mainthread;
    int _ref_;
    int self_ref;
    int prevent_gc_ref;
    ws_frame_node_t *frame_queue_head;
    ws_frame_node_t *frame_queue_tail;
    int frame_queue_len;
    int owns_request;
    volatile int ws_cleaning_up;
    int close_cb_running;
    int ws_cleanup_requested;
    /* Intrusive live-WebSocket list (LuaServer.ws_list), guarded by
     * server->accept_mutex; see fix-plan Phase 1. */
    struct Request *ws_next;
    struct Request *ws_prev;
    int ws_linked;
    /* RFC 7692 permessage-deflate (0 = not negotiated) */
    int ws_pmd;
    int ws_pmd_server_no_context_takeover;
    int ws_pmd_client_no_context_takeover;
    int ws_pmd_server_max_window_bits; /* 8..15 */
    int ws_pmd_client_max_window_bits; /* 8..15 */
    int ws_inflate_ok;
    int ws_deflate_ok;
    z_stream ws_inflate;
    z_stream ws_deflate;
};

typedef struct {
    char *name;
    enum evhttp_cmd_type cmd;
} MethodMap;

// ============================================================
// Shared globals (defined in httpd.c)
// ============================================================

extern const MethodMap methodMap[];

// ============================================================
// Inline helpers
// ============================================================

static inline Request *request_from_table(lua_State *L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_rawgeti(L, idx, 1);
    Request *request = (Request *)luaL_checkudata(L, -1, LUA_EVHTTP_REQUEST_DATA_TYPE);
    lua_pop(L, 1);
    return request;
}

// ============================================================
// Logging (defined in httpd.c)
// ============================================================

void httpd_log(log_level_t level, const char* format, ...);

#define LOG_ERROR_FMT(fmt, ...) httpd_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_WARN_FMT(fmt, ...) httpd_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_INFO_FMT(fmt, ...) httpd_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_FMT(fmt, ...) httpd_log(LOG_DEBUG, fmt, ##__VA_ARGS__)

// ============================================================
// httpd.c exports
// ============================================================

void httpd_release_conn_guard(Request *request);
void httpd_finish_metrics(Request *request, int status_code, size_t bytes_sent);
void newtable_from_req(lua_State *L, struct evhttp_request *req, LuaServer *server);
void set_connection_header(struct evhttp_request *req, LuaServer *server);
int httpd_server_rebind(LuaServer *server);
void httpd_server_begin_destroy(LuaServer *server);
int httpd_server_ws_attach(Request *request);
void httpd_server_ws_detach(Request *request);

// ============================================================
// httpd_metrics.c exports
// ============================================================

void metrics_init(void);
void metrics_update_request_start(const char *method);
void metrics_update_request_end(int status_code, size_t bytes_sent);
void metrics_update_connection(void);

// Metrics endpoint callbacks
void metrics_request_cb(struct evhttp_request *req, void *arg);
void smoke_request_cb(struct evhttp_request *req, void *arg);

// ============================================================
// httpd_request.c exports
// ============================================================

int request_push_body(lua_State *L, int idx);

LUA_API int lua_evhttp_request_available(lua_State *L);
LUA_API int lua_evhttp_request_read(lua_State *L);
LUA_API int lua_evhttp_request_reply(lua_State *L);
LUA_API int lua_evhttp_request_reply_addheader(lua_State *L);
LUA_API int lua_evhttp_request_reply_start(lua_State *L);
LUA_API int lua_evhttp_request_reply_chunk(lua_State *L);
LUA_API int lua_evhttp_request_reply_end(lua_State *L);

// ============================================================
// httpd_websocket.c exports
// ============================================================

int is_websocket_upgrade_request(struct evhttp_request *req);
void ws_connection_cleanup(Request *request);
void ws_readcb(struct bufferevent *bev, void *arg);
void ws_eventcb(struct bufferevent *bev, short events, void *arg);

LUA_API int lua_evhttp_request_is_websocket_upgrade(lua_State *L);
LUA_API int lua_evhttp_request_websocket_accept(lua_State *L);
LUA_API int lua_evhttp_request_websocket_send(lua_State *L);
LUA_API int lua_evhttp_request_websocket_ping(lua_State *L);
LUA_API int lua_evhttp_request_websocket_pong(lua_State *L);
LUA_API int lua_evhttp_request_websocket_close(lua_State *L);
LUA_API int lua_evhttp_request_websocket_state(lua_State *L);
LUA_API int lua_evhttp_request_websocket_receive(lua_State *L);

#endif // HTTPD_INTERNAL_H
