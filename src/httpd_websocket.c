// httpd_websocket.c — WebSocket frame parsing, send/recv, accept, cleanup

#include "httpd_internal.h"
#include <openssl/sha.h>
#include <openssl/evp.h>

// ============================================================
// WebSocket upgrade detection
// ============================================================

int is_websocket_upgrade_request(struct evhttp_request *req) {
    const char *upgrade = evhttp_find_header(req->input_headers, "Upgrade");
    const char *connection = evhttp_find_header(req->input_headers, "Connection");
    const char *ws_key = evhttp_find_header(req->input_headers, "Sec-WebSocket-Key");
    const char *ws_version = evhttp_find_header(req->input_headers, "Sec-WebSocket-Version");

    return (upgrade && strcasecmp(upgrade, "websocket") == 0 &&
            connection && strcasestr(connection, "upgrade") != NULL &&
            ws_key && strlen(ws_key) > 0 &&
            ws_version && strcmp(ws_version, "13") == 0);
}

// ============================================================
// WebSocket accept key generation (SHA-1 + base64)
// ============================================================

static char* generate_websocket_accept_key(const char* client_key) {
    if (!client_key) return NULL;

    char combined[WEBSOCKET_KEY_LEN + WEBSOCKET_GUID_LEN + 1];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WEBSOCKET_MAGIC_STRING);

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), hash);

    char *accept_key = malloc(((SHA_DIGEST_LENGTH + 2) / 3) * 4 + 1);
    if (accept_key) {
        int len = EVP_EncodeBlock((unsigned char *)accept_key, hash, SHA_DIGEST_LENGTH);
        accept_key[len] = '\0';
    }
    return accept_key;
}

// ============================================================
// Frame parsing and encoding
// ============================================================

static void websocket_mask_unmask(char *data, uint64_t len, uint32_t mask_key) {
    uint8_t *mask_bytes = (uint8_t *)&mask_key;
    for (uint64_t i = 0; i < len; i++) {
        data[i] ^= mask_bytes[i % 4];
    }
}

static int websocket_parse_frame(struct evbuffer *input, websocket_frame_t *frame) {
    size_t available = evbuffer_get_length(input);
    if (available < 2) {
        return 0;
    }

    unsigned char peek_buf[14];
    size_t peek_len = (available < 14) ? available : 14;
    evbuffer_copyout(input, peek_buf, peek_len);

    frame->fin = (peek_buf[0] & 0x80) != 0;
    frame->rsv1 = (peek_buf[0] & 0x40) != 0;
    frame->rsv2 = (peek_buf[0] & 0x20) != 0;
    frame->rsv3 = (peek_buf[0] & 0x10) != 0;
    frame->opcode = (websocket_opcode_t)(peek_buf[0] & 0x0F);
    frame->masked = (peek_buf[1] & 0x80) != 0;

    uint64_t payload_len = peek_buf[1] & 0x7F;
    size_t header_size = 2;

    if (payload_len == 126) {
        if (peek_len < 4) return 0;
        uint16_t len16;
        memcpy(&len16, peek_buf + 2, 2);
        frame->payload_len = ntohs(len16);
        header_size += 2;
    } else if (payload_len == 127) {
        if (peek_len < 10) return 0;
        uint64_t len64;
        memcpy(&len64, peek_buf + 2, 8);
        frame->payload_len = be64toh(len64);
        header_size += 8;
    } else {
        frame->payload_len = payload_len;
    }

    if (frame->masked) {
        header_size += 4;
    }

    if (available < header_size + frame->payload_len) {
        return 0;
    }

    if (frame->masked) {
        memcpy(&frame->mask_key, peek_buf + header_size - 4, 4);
    }

    evbuffer_drain(input, header_size);

    if (frame->payload_len > 0) {
        frame->payload = malloc(frame->payload_len);
        if (!frame->payload) {
            return -1;
        }
        evbuffer_remove(input, frame->payload, frame->payload_len);
        if (frame->masked) {
            websocket_mask_unmask(frame->payload, frame->payload_len, frame->mask_key);
        }
    } else {
        frame->payload = NULL;
    }

    return 1;
}

static struct evbuffer* websocket_create_frame(websocket_opcode_t opcode, const char *payload,
                                             uint64_t payload_len, int fin) {
    struct evbuffer *frame = evbuffer_new();
    if (!frame) return NULL;

    uint8_t first_byte = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    evbuffer_add(frame, &first_byte, 1);

    if (payload_len < 126) {
        uint8_t second_byte = (uint8_t)payload_len;
        evbuffer_add(frame, &second_byte, 1);
    } else if (payload_len <= 65535) {
        uint8_t second_byte = 126;
        evbuffer_add(frame, &second_byte, 1);
        uint16_t len16 = htons((uint16_t)payload_len);
        evbuffer_add(frame, &len16, 2);
    } else {
        uint8_t second_byte = 127;
        evbuffer_add(frame, &second_byte, 1);
        uint64_t len64 = htobe64(payload_len);
        evbuffer_add(frame, &len64, 8);
    }

    if (payload && payload_len > 0) {
        evbuffer_add(frame, payload, payload_len);
    }

    return frame;
}

static void websocket_frame_free(websocket_frame_t *frame) {
    if (frame && frame->payload) {
        free(frame->payload);
        frame->payload = NULL;
    }
}

// ============================================================
// Frame queue
// ============================================================

static void ws_frame_queue_push(Request *request, websocket_frame_t *frame) {
    if (request->frame_queue_len >= WS_MAX_QUEUED_FRAMES) {
        LOG_WARN_FMT("WebSocket frame queue full, dropping frame");
        websocket_frame_free(frame);
        return;
    }
    ws_frame_node_t *node = malloc(sizeof(ws_frame_node_t));
    if (!node) {
        websocket_frame_free(frame);
        return;
    }
    node->frame = *frame;
    node->next = NULL;
    if (request->frame_queue_tail) {
        request->frame_queue_tail->next = node;
    } else {
        request->frame_queue_head = node;
    }
    request->frame_queue_tail = node;
    request->frame_queue_len++;
}

static int ws_frame_queue_pop(Request *request, websocket_frame_t *frame) {
    ws_frame_node_t *node = request->frame_queue_head;
    if (!node) return 0;
    *frame = node->frame;
    request->frame_queue_head = node->next;
    if (!request->frame_queue_head) {
        request->frame_queue_tail = NULL;
    }
    request->frame_queue_len--;
    free(node);
    return 1;
}

static void ws_frame_queue_clear(Request *request) {
    ws_frame_node_t *node = request->frame_queue_head;
    while (node) {
        ws_frame_node_t *next = node->next;
        if (node->frame.payload) free(node->frame.payload);
        free(node);
        node = next;
    }
    request->frame_queue_head = NULL;
    request->frame_queue_tail = NULL;
    request->frame_queue_len = 0;
}

// ============================================================
// Connection cleanup
// ============================================================

static void ws_deferred_free_cb(evutil_socket_t fd, short what, void *ctx) {
    (void)fd; (void)what;
    Request *request = (Request *)ctx;

    if (request->ws_cleaning_up != 1) {
        return;
    }

    if (request->owns_request && request->req) {
        struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
        request->req = NULL;
        request->owns_request = 0;
        if (evcon) {
            evhttp_connection_free(evcon);
        }
    }
    request->req = NULL;
    request->owns_request = 0;
    if (request->self_ref != LUA_NOREF && request->mainthread) {
        CLEAR_REF(request->mainthread, request->self_ref);
    }
    if (request->prevent_gc_ref != LUA_NOREF && request->mainthread) {
        luaL_unref(request->mainthread, LUA_REGISTRYINDEX, request->prevent_gc_ref);
        request->prevent_gc_ref = LUA_NOREF;
    }
}

static void ws_schedule_free(Request *request, struct bufferevent *bev) {
    struct timeval tv = {0, 0};
    bufferevent_setcb(bev, NULL, NULL, NULL, NULL);
    event_base_once(bufferevent_get_base(bev),
                    -1, EV_TIMEOUT, ws_deferred_free_cb, request, &tv);
}

static void ws_flush_writecb(struct bufferevent *bev, void *ctx) {
    Request *request = (Request *)ctx;
    if (request->ws_cleaning_up) {
        return;
    }
    struct evbuffer *output = bufferevent_get_output(bev);
    if (evbuffer_get_length(output) == 0) {
        ws_schedule_free(request, bev);
    }
}

static void ws_flush_eventcb(struct bufferevent *bev, short what, void *ctx) {
    Request *request = (Request *)ctx;
    if (request->ws_cleaning_up) {
        return;
    }
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        ws_schedule_free(request, bev);
    }
}

void ws_connection_cleanup(Request *request) {
    if (__sync_bool_compare_and_swap(&request->ws_cleaning_up, 0, 1) == 0) {
        return;
    }

    ws_frame_queue_clear(request);

    if (request->ws_bev) {
        struct bufferevent *bev = request->ws_bev;
        request->ws_bev = NULL;
        bufferevent_disable(bev, EV_READ);

        struct evbuffer *output = bufferevent_get_output(bev);
        if (evbuffer_get_length(output) == 0) {
            ws_schedule_free(request, bev);
        } else {
            bufferevent_setcb(bev, NULL, ws_flush_writecb,
                              ws_flush_eventcb, request);
        }
    } else if (request->owns_request && request->req) {
        struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
        request->req = NULL;
        request->owns_request = 0;
        if (evcon) {
            evhttp_connection_free(evcon);
        }
        if (request->self_ref != LUA_NOREF && request->mainthread) {
            CLEAR_REF(request->mainthread, request->self_ref);
        }
        if (request->prevent_gc_ref != LUA_NOREF && request->mainthread) {
            luaL_unref(request->mainthread, LUA_REGISTRYINDEX, request->prevent_gc_ref);
            request->prevent_gc_ref = LUA_NOREF;
        }
    }
}

// ============================================================
// Resume helpers
// ============================================================

static void ws_resume_with_error(Request *request, const char *errmsg) {
    lua_State *L = NULL;
    REF_STATE_GET(request, L);
    if (!L) return;
    REF_STATE_CLEAR(request);

    lua_pushnil(L);
    lua_pushstring(L, errmsg);

    int status = FAN_RESUME(L, NULL, 2);
    if (status == LUA_OK || status > LUA_YIELD) {
        ws_connection_cleanup(request);
    }
}

static void ws_resume_with_frame(Request *request, websocket_frame_t *frame) {
    lua_State *L = NULL;
    REF_STATE_GET(request, L);
    if (!L) {
        websocket_frame_free(frame);
        return;
    }
    REF_STATE_CLEAR(request);

    if (frame->payload && frame->payload_len > 0) {
        lua_pushlstring(L, frame->payload, frame->payload_len);
    } else {
        lua_pushliteral(L, "");
    }
    lua_pushinteger(L, frame->opcode);
    websocket_frame_free(frame);

    int status = FAN_RESUME(L, NULL, 2);
    if (status == LUA_OK || status > LUA_YIELD) {
        ws_connection_cleanup(request);
    }
}

// ============================================================
// Bufferevent callbacks
// ============================================================

void ws_readcb(struct bufferevent *bev, void *ctx) {
    Request *request = (Request *)ctx;
    struct evbuffer *input = bufferevent_get_input(bev);

    while (evbuffer_get_length(input) > 0) {
        websocket_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        int rc = websocket_parse_frame(input, &frame);
        if (rc == 0) break;
        if (rc < 0) {
            request->ws_state = WS_STATE_CLOSED;
            if (request->_ref_ != LUA_NOREF) {
                ws_resume_with_error(request, "frame parse error");
            } else {
                ws_connection_cleanup(request);
            }
            return;
        }

        switch (frame.opcode) {
            case WS_OPCODE_PING:
                if (request->ws_bev && request->ws_state == WS_STATE_OPEN) {
                    struct evbuffer *pong = websocket_create_frame(
                        WS_OPCODE_PONG, frame.payload, frame.payload_len, 1);
                    if (pong) {
                        bufferevent_write_buffer(request->ws_bev, pong);
                        evbuffer_free(pong);
                    }
                }
                websocket_frame_free(&frame);
                continue;

            case WS_OPCODE_PONG:
                websocket_frame_free(&frame);
                continue;

            case WS_OPCODE_CLOSE:
                request->ws_state = WS_STATE_CLOSED;
                if (request->ws_bev) {
                    struct evbuffer *close_resp = websocket_create_frame(
                        WS_OPCODE_CLOSE, frame.payload, frame.payload_len, 1);
                    if (close_resp) {
                        bufferevent_write_buffer(request->ws_bev, close_resp);
                        evbuffer_free(close_resp);
                    }
                }
                websocket_frame_free(&frame);
                if (request->_ref_ != LUA_NOREF) {
                    ws_resume_with_error(request, "closed");
                } else {
                    ws_connection_cleanup(request);
                }
                return;

            case WS_OPCODE_TEXT:
            case WS_OPCODE_BINARY:
                if (request->_ref_ != LUA_NOREF) {
                    ws_resume_with_frame(request, &frame);
                    if (!request->ws_bev) return;
                } else {
                    ws_frame_queue_push(request, &frame);
                }
                break;

            default:
                websocket_frame_free(&frame);
                break;
        }
    }
}

void ws_eventcb(struct bufferevent *bev, short what, void *ctx) {
    Request *request = (Request *)ctx;
    (void)bev;

    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        request->ws_state = WS_STATE_CLOSED;

        if (request->_ref_ != LUA_NOREF) {
            const char *msg = (what & BEV_EVENT_EOF) ? "connection closed" : "connection error";
            ws_resume_with_error(request, msg);
        } else {
            ws_connection_cleanup(request);
        }
    }
}

// ============================================================
// Lua API: WebSocket upgrade detection
// ============================================================

LUA_API int lua_evhttp_request_is_websocket_upgrade(lua_State *L) {
    Request *request = request_from_table(L, 1);
    struct evhttp_request *req = request->req;
    if (!req) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int is_upgrade = is_websocket_upgrade_request(req);
    lua_pushboolean(L, is_upgrade);
    return 1;
}

// ============================================================
// Lua API: WebSocket accept
// ============================================================

LUA_API int lua_evhttp_request_websocket_accept(lua_State *L) {
    Request *request = request_from_table(L, 1);
    struct evhttp_request *req = request->req;
    if (!req) {
        return luaL_error(L, "connection closed by peer");
    }

    if (request->reply_status != REPLY_STATUS_NONE) {
        return luaL_error(L, "Response already started");
    }

    if (!is_websocket_upgrade_request(req)) {
        return luaL_error(L, "Not a valid WebSocket upgrade request");
    }

    const char *ws_key = evhttp_find_header(req->input_headers, "Sec-WebSocket-Key");
    if (!ws_key) {
        return luaL_error(L, "Missing Sec-WebSocket-Key header");
    }

    char *accept_key = generate_websocket_accept_key(ws_key);
    if (!accept_key) {
        return luaL_error(L, "Failed to generate WebSocket accept key");
    }

    evhttp_request_own(req);
    request->owns_request = 1;

    struct evbuffer *response = evbuffer_new();
    if (!response) {
        free(accept_key);
        return luaL_error(L, "Failed to create response buffer");
    }

    evbuffer_add_printf(response,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_key);

    struct evhttp_connection *evcon = evhttp_request_get_connection(req);
    struct bufferevent *bev = evhttp_connection_get_bufferevent(evcon);

    if (bev) {
        bufferevent_write_buffer(bev, response);

        request->is_websocket = 1;
        request->ws_state = WS_STATE_OPEN;
        request->ws_bev = bev;
        request->reply_status = REPLY_STATUS_REPLYED;
        request->mainthread = utlua_mainthread(L);

        evhttp_connection_set_closecb(evcon, NULL, NULL);

        evhttp_connection_set_timeout(evcon, 0);
        bufferevent_setcb(bev, ws_readcb, NULL, ws_eventcb, request);
        bufferevent_enable(bev, EV_READ | EV_WRITE);

        lua_pushvalue(L, 1);
        request->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    evbuffer_free(response);
    free(accept_key);

    lua_pushboolean(L, bev != NULL);
    return 1;
}

// ============================================================
// Lua API: WebSocket send
// ============================================================

LUA_API int lua_evhttp_request_websocket_send(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket || request->ws_state != WS_STATE_OPEN) {
        return luaL_error(L, "WebSocket connection not open");
    }

    if (!request->ws_bev) {
        return luaL_error(L, "WebSocket connection not available");
    }

    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 2, &data_len);
    int opcode = luaL_optinteger(L, 3, WS_OPCODE_TEXT);
    int fin = luaL_optinteger(L, 4, 1);

    if (opcode < 0 || opcode > 0xF) {
        return luaL_error(L, "Invalid WebSocket opcode: %d", opcode);
    }

    struct evbuffer *frame = websocket_create_frame((websocket_opcode_t)opcode, data, data_len, fin);
    if (!frame) {
        return luaL_error(L, "Failed to create WebSocket frame");
    }

    int result = bufferevent_write_buffer(request->ws_bev, frame);
    evbuffer_free(frame);

    lua_pushboolean(L, result == 0);
    return 1;
}

// ============================================================
// Lua API: WebSocket ping
// ============================================================

LUA_API int lua_evhttp_request_websocket_ping(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket || request->ws_state != WS_STATE_OPEN) {
        return luaL_error(L, "WebSocket connection not open");
    }

    if (!request->ws_bev) {
        return luaL_error(L, "WebSocket connection not available");
    }

    size_t payload_len = 0;
    const char *payload = lua_tolstring(L, 2, &payload_len);

    if (payload_len > 125) {
        return luaL_error(L, "Ping payload too large (max 125 bytes)");
    }

    struct evbuffer *frame = websocket_create_frame(WS_OPCODE_PING, payload, payload_len, 1);
    if (!frame) {
        return luaL_error(L, "Failed to create ping frame");
    }

    int result = bufferevent_write_buffer(request->ws_bev, frame);
    evbuffer_free(frame);

    lua_pushboolean(L, result == 0);
    return 1;
}

// ============================================================
// Lua API: WebSocket pong
// ============================================================

LUA_API int lua_evhttp_request_websocket_pong(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket || request->ws_state != WS_STATE_OPEN) {
        return luaL_error(L, "WebSocket connection not open");
    }

    if (!request->ws_bev) {
        return luaL_error(L, "WebSocket connection not available");
    }

    size_t payload_len = 0;
    const char *payload = lua_tolstring(L, 2, &payload_len);

    if (payload_len > 125) {
        return luaL_error(L, "Pong payload too large (max 125 bytes)");
    }

    struct evbuffer *frame = websocket_create_frame(WS_OPCODE_PONG, payload, payload_len, 1);
    if (!frame) {
        return luaL_error(L, "Failed to create pong frame");
    }

    int result = bufferevent_write_buffer(request->ws_bev, frame);
    evbuffer_free(frame);

    lua_pushboolean(L, result == 0);
    return 1;
}

// ============================================================
// Lua API: WebSocket close
// ============================================================

LUA_API int lua_evhttp_request_websocket_close(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket) {
        return luaL_error(L, "Not a WebSocket connection");
    }

    if (request->ws_state == WS_STATE_CLOSED) {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (!request->ws_bev) {
        request->ws_state = WS_STATE_CLOSED;
        lua_pushboolean(L, 1);
        return 1;
    }

    int close_code = luaL_optinteger(L, 2, 1000);
    size_t reason_len = 0;
    const char *reason = lua_tolstring(L, 3, &reason_len);

    char close_payload[127];
    size_t close_payload_len = 0;

    if (close_code >= 1000 && close_code <= 4999) {
        close_payload[0] = (close_code >> 8) & 0xFF;
        close_payload[1] = close_code & 0xFF;
        close_payload_len = 2;

        if (reason && reason_len > 0) {
            size_t max_reason = sizeof(close_payload) - 2;
            if (reason_len > max_reason) reason_len = max_reason;
            memcpy(close_payload + 2, reason, reason_len);
            close_payload_len += reason_len;
        }
    }

    struct evbuffer *frame = websocket_create_frame(WS_OPCODE_CLOSE,
                                                   close_payload_len > 0 ? close_payload : NULL,
                                                   close_payload_len, 1);
    if (frame) {
        bufferevent_write_buffer(request->ws_bev, frame);
        evbuffer_free(frame);
    }

    request->ws_state = WS_STATE_CLOSING;

    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// Lua API: WebSocket state query
// ============================================================

LUA_API int lua_evhttp_request_websocket_state(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket) {
        lua_pushstring(L, "not_websocket");
        return 1;
    }

    const char *state_names[] = {"connecting", "open", "closing", "closed"};
    lua_pushstring(L, state_names[request->ws_state]);
    return 1;
}

// ============================================================
// Lua API: WebSocket receive (yields coroutine)
// ============================================================

LUA_API int lua_evhttp_request_websocket_receive(lua_State *L) {
    Request *request = request_from_table(L, 1);

    if (!request->is_websocket) {
        return luaL_error(L, "Not a WebSocket connection");
    }

    if (request->ws_state == WS_STATE_CLOSED) {
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }

    if (request->ws_state != WS_STATE_OPEN) {
        lua_pushnil(L);
        lua_pushliteral(L, "not open");
        return 2;
    }

    websocket_frame_t frame;
    if (ws_frame_queue_pop(request, &frame)) {
        if (frame.payload && frame.payload_len > 0) {
            lua_pushlstring(L, frame.payload, frame.payload_len);
        } else {
            lua_pushliteral(L, "");
        }
        lua_pushinteger(L, frame.opcode);
        websocket_frame_free(&frame);
        return 2;
    }

    if (request->_ref_ != LUA_NOREF) {
        return luaL_error(L, "Another coroutine is already waiting on this WebSocket");
    }

    REF_STATE_SET(request, L);
    return lua_yield(L, 0);
}
