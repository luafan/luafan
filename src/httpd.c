// httpd.c — Core HTTP server: Lua bindings, dispatch, server lifecycle

#include "httpd_internal.h"

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

static void httpd_conn_close_cb(struct evhttp_connection *evcon, void *arg) {
    (void)evcon;
    Request *request = (Request *)arg;
    if (request->req && request->reply_status == REPLY_STATUS_REPLY_START) {
        evhttp_send_reply_end(request->req);
    }
    request->req = NULL;
    request->reply_status = REPLY_STATUS_REPLYED;
    if (request->prevent_gc_ref != LUA_NOREF && request->mainthread) {
        luaL_unref(request->mainthread, LUA_REGISTRYINDEX, request->prevent_gc_ref);
        request->prevent_gc_ref = LUA_NOREF;
    }
}

void httpd_release_conn_guard(Request *request) {
    if (request->req) {
        struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
        if (evcon) {
            evhttp_connection_set_closecb(evcon, NULL, NULL);
        }
    }
    if (request->prevent_gc_ref != LUA_NOREF && request->mainthread) {
        luaL_unref(request->mainthread, LUA_REGISTRYINDEX, request->prevent_gc_ref);
        request->prevent_gc_ref = LUA_NOREF;
    }
}

void newtable_from_req(lua_State *L, struct evhttp_request *req) {
    lua_newtable(L);
    Request *request = (Request *)lua_newuserdata(L, sizeof(Request));
    request->reply_status = REPLY_STATUS_NONE;
    request->req = req;
    request->is_websocket = 0;
    request->ws_state = WS_STATE_CONNECTING;
    request->ws_bev = NULL;
    request->mainthread = utlua_mainthread(L);
    request->_ref_ = LUA_NOREF;
    request->self_ref = LUA_NOREF;
    request->prevent_gc_ref = LUA_NOREF;
    request->frame_queue_head = NULL;
    request->frame_queue_tail = NULL;
    request->frame_queue_len = 0;
    request->owns_request = 0;
    request->ws_cleaning_up = 0;
    lua_rawseti(L, -2, 1);

    lua_pushvalue(L, -1);
    request->prevent_gc_ref = luaL_ref(L, LUA_REGISTRYINDEX);

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

    int major = 1, minor = 1;

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
// Main request dispatch
// ============================================================

static void httpd_handler_cgi_bin(struct evhttp_request *req, LuaServer *server) {
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
        lua_unlock(mainthread);
        evhttp_send_error(req, 500, "Internal Server Error");
        return;
    }

    if (!lua_isfunction(cbs.co, -1)) {
        LOGE("httpd gen_cb: onServiceRef=%d resolved to %s, expected function\n",
             server->onServiceRef, luaL_typename(cbs.co, -1));
        lua_pop(cbs.co, 1);
        lua_unlock(mainthread);
        FAN_CB_CLEANUP(mainthread, cbs);
        evhttp_send_error(req, 500, "Internal Server Error");
        return;
    }

    newtable_from_req(cbs.co, req);

    luaL_getmetatable(cbs.co, LUA_EVHTTP_REQUEST_TYPE);
    lua_setmetatable(cbs.co, -2);

    lua_rawgeti(cbs.co, -1, 1);
    Request *request = (Request *)lua_touserdata(cbs.co, -1);
    lua_pop(cbs.co, 1);

    lua_pushvalue(cbs.co, -1);

    lua_unlock(mainthread);
    int status = FAN_RESUME(cbs.co, mainthread, 2);
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

LUA_API int lua_evhttp_server_gc(lua_State *L) {
    LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
    CLEAR_REF(L, server->onServiceRef)

    if (server->httpd) {
        if (server->boundsocket) {
            evhttp_del_accept_socket(server->httpd, server->boundsocket);
            server->boundsocket = NULL;
        }

        if (server->httpd) {
            evhttp_free(server->httpd);
            server->httpd = NULL;
        }
    }

#if FAN_HAS_OPENSSL
    if (server->ctx) {
        SSL_CTX_free(server->ctx);
    }
#endif
    lua_pop(L, 1);

    return 0;
}

// ============================================================
// Request __index metamethod
// ============================================================

LUA_API int lua_evhttp_request_lookup(lua_State *L) {
    Request *request = request_from_table(L, 1);
    const char *p = lua_tostring(L, 2);

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
    } else if (strcmp(p, "headers") == 0) {
        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);

        lua_newtable(L);
        struct evkeyval *item;
        TAILQ_FOREACH(item, headers, next) {
            lua_pushstring(L, item->value);
            lua_setfield(L, -2, item->key);
        }
        return 1;
    } else if (strcmp(p, "params") == 0) {
        struct evkeyvalq params;
        evhttp_parse_query_str(evhttp_uri_get_query(req->uri_elems), &params);

        lua_newtable(L);
        struct evkeyval *item;
        TAILQ_FOREACH(item, &params, next) {
            lua_pushstring(L, item->value);
            lua_setfield(L, -2, item->key);
        }
        evhttp_clear_headers(&params);

        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
        const char *contentType = evhttp_find_header(headers, "Content-Type");
        if (contentType && strstr(contentType, "application/x-www-form-urlencoded") == contentType) {
            request_push_body(L, 1);
            if (lua_type(L, -1) == LUA_TSTRING) {
                const char *data = lua_tostring(L, -1);
                struct evkeyvalq params;
                evhttp_parse_query_str(data, &params);
                lua_pop(L, 1);

                TAILQ_FOREACH(item, &params, next) {
                    lua_pushstring(L, item->value);
                    lua_setfield(L, -2, item->key);
                }
                evhttp_clear_headers(&params);
            }
        }
        return 1;
    } else if (strcmp(p, "body") == 0) {
        request_push_body(L, 1);
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

void httpd_server_rebind(lua_State *L, LuaServer *server) {
    struct evhttp_bound_socket *boundsocket = evhttp_bind_socket_with_handle(server->httpd, server->host, server->port);

    server->boundsocket = boundsocket;
    if (boundsocket) {
        server->port = regress_get_socket_port(evhttp_bound_socket_get_fd(boundsocket));
    } else {
        server->port = 0;
    }
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
        return luaL_error(L, "Invalid server configuration parameters");
    }

    struct evhttp *httpd = evhttp_new(event_mgr_base());

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
        server->ctx = ctx;
        SSL_CTX_set_options(ctx,
                            SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE |
                            SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                            SSL_OP_NO_COMPRESSION |
                            SSL_OP_CIPHER_SERVER_PREFERENCE);

        SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS");

        EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!ecdh) {
            die_most_horribly_from_openssl_error(L, "EC_KEY_new_by_curve_name");
        }

        if (1 != SSL_CTX_set_tmp_ecdh(ctx, ecdh)) {
            die_most_horribly_from_openssl_error(L, "SSL_CTX_set_tmp_ecdh");
        }

        server_setup_certs(L, ctx, cert, key);

        evhttp_set_bevcb(httpd, bevcb, ctx);
    }

    lua_pop(L, 2);
#endif
#endif

#endif

    DUP_STR_FROM_TABLE(L, server->host, 1, "host")
    SET_INT_FROM_TABLE(L, server->port, 1, "port")

    server->httpd = httpd;

    httpd_server_rebind(L, server);

    if (!server->boundsocket) {
#if FAN_HAS_OPENSSL
        if (server->ctx) {
            SSL_CTX_free(server->ctx);
            server->ctx = NULL;
        }
#endif
        evhttp_free(httpd);
        server->httpd = NULL;
        return 0;
    }

    SET_FUNC_REF_FROM_TABLE(L, server->onServiceRef, 1, "onService")

    evhttp_set_timeout(httpd, server->keep_alive_timeout + 30);
    evhttp_set_cb(httpd, "/smoketest", smoke_request_cb, NULL);
    evhttp_set_cb(httpd, "/metrics", metrics_request_cb, NULL);
    evhttp_set_gencb(httpd, (void (*)(struct evhttp_request *, void *))httpd_handler_cgi_bin, server);

    metrics_init();

    lua_pushlightuserdata(L, server);
    lua_setfield(L, LUA_REGISTRYINDEX, "httpd_server");

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
    httpd_server_rebind(L, server);
    return 0;
}

// ============================================================
// Lua module registration
// ============================================================

static const luaL_Reg utdlib[] = {{"bind", utd_bind}, {NULL, NULL}};

LUA_API int luaopen_fan_httpd_core(lua_State *L) {
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

    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    lua_newtable(L);
    luaL_register(L, "httpd", utdlib);

    return 1;
}
