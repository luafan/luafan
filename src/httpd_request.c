// httpd_request.c — Body reading, reply functions, chunked transfer

#include "httpd_internal.h"
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/http_struct.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// ============================================================
// Body reading
// ============================================================

int request_push_body(lua_State *L, int idx) {
    if (idx < 0) {
        idx = lua_gettop(L) + idx + 1;
    }
    lua_rawgetp(L, idx, "body");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);

        struct evhttp_request *req = request_from_table(L, idx)->req;
        if (!req) {
            lua_pushnil(L);
            return 1;
        }
        struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
        if (bodybuf) {
            size_t len = evbuffer_get_length(bodybuf);

            size_t body_limit = HTTP_POST_BODY_LIMIT;
            lua_getfield(L, LUA_REGISTRYINDEX, "httpd_server");
            if (!lua_isnil(L, -1)) {
                LuaServer *server = (LuaServer *)lua_touserdata(L, -1);
                if (server) {
                    body_limit = server->max_body_size;
                }
            }
            lua_pop(L, 1);

            if (len > 0 && len < body_limit) {
                char *data = calloc(1, len + 1);
                if (!data) {
                    LOG_ERROR_FMT("Memory allocation failed for request body: %zu bytes", len + 1);
                    lua_pushnil(L);
                    return 1;
                }

                size_t total_read = 0;
                char *ptrdata = data;
                while (total_read < len) {
                    int read = evbuffer_remove(bodybuf, ptrdata, len - total_read);
                    if (read <= 0) break;
                    ptrdata += read;
                    total_read += read;
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
    struct evhttp_request *req = request_from_table(L, 1)->req;
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
    struct evhttp_request *req = request_from_table(L, 1)->req;
    if (!req) {
        lua_pushnil(L);
        return 1;
    }
    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
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

LUA_API int lua_evhttp_request_reply(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!request->req) {
        return luaL_error(L, "connection closed by peer");
    }
    switch (request->reply_status) {
        case REPLY_STATUS_REPLYED:
            return luaL_error(L, "reply has completed already.");
        case REPLY_STATUS_REPLY_START:
            evhttp_send_reply_end(request->req);
            request->reply_status = REPLY_STATUS_REPLYED;
            httpd_release_conn_guard(request);
            lua_settop(L, 1);
            return 1;
        default:
            break;
    }

    int responseCode = (int)lua_tointeger(L, 2);
    const char *responseMessage = lua_tostring(L, 3);

    size_t responseBuffLen = 0;
    const char *responseBuff = lua_tolstring(L, 4, &responseBuffLen);

    LuaServer *server = NULL;
    lua_getfield(L, LUA_REGISTRYINDEX, "httpd_server");
    if (!lua_isnil(L, -1)) {
        server = (LuaServer *)lua_touserdata(L, -1);
    }
    lua_pop(L, 1);

    if (server) {
        set_connection_header(request->req, server);
    } else {
        evhttp_add_header(request->req->output_headers, "Connection", "close");
    }

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        LOG_ERROR_FMT("Failed to create response buffer for request");
        return luaL_error(L, "Failed to create response buffer");
    }

    if (responseBuff && responseBuffLen > 0) {
        if (evbuffer_add(buf, responseBuff, responseBuffLen) < 0) {
            evbuffer_free(buf);
            LOG_ERROR_FMT("Failed to add %zu bytes to response buffer", responseBuffLen);
            return luaL_error(L, "Failed to add data to response buffer");
        }
    }

    evhttp_send_reply(request->req, responseCode, responseMessage, buf);
    evbuffer_free(buf);

    request->reply_status = REPLY_STATUS_REPLYED;
    httpd_release_conn_guard(request);

    metrics_update_request_end(responseCode, responseBuffLen);

    return 0;
}

LUA_API int lua_evhttp_request_reply_addheader(lua_State *L) {
    Request *request = request_from_table(L, 1);
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

    evhttp_add_header(request->req->output_headers, key, value);

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_start(lua_State *L) {
    Request *request = request_from_table(L, 1);
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
    evhttp_send_reply_start(request->req, responseCode, responseMessage);
    request->reply_status = REPLY_STATUS_REPLY_START;

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_chunk(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!request->req) {
        request->reply_status = REPLY_STATUS_REPLYED;
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

    struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
    if (!evcon) {
        request->reply_status = REPLY_STATUS_REPLYED;
        return luaL_error(L, "connection closed by peer");
    }

    struct bufferevent *bev = evhttp_connection_get_bufferevent(evcon);
    if (bev) {
        evutil_socket_t fd = bufferevent_getfd(bev);
        if (fd >= 0) {
            char peek_buf[1];
            ssize_t n = recv(fd, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                request->reply_status = REPLY_STATUS_REPLYED;
                return luaL_error(L, "connection closed by peer");
            }
        }
    }

    int top = lua_gettop(L);
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
        evhttp_send_reply_chunk(request->req, buf);
        evbuffer_free(buf);
    }

    lua_settop(L, 1);
    return 1;
}

LUA_API int lua_evhttp_request_reply_end(lua_State *L) {
    Request *request = request_from_table(L, 1);
    if (!request->req) {
        request->reply_status = REPLY_STATUS_REPLYED;
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

    struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
    if (evcon) {
        evhttp_send_reply_end(request->req);
    }
    request->reply_status = REPLY_STATUS_REPLYED;
    httpd_release_conn_guard(request);

    lua_settop(L, 1);
    return 1;
}
