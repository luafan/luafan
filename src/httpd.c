
#include "utlua.h"

typedef struct
{
  lua_State *mainthread;

  struct evhttp *httpd;
  struct evhttp_bound_socket *boundsocket;

  char *host;
  int port;

#if FAN_HAS_OPENSSL
  SSL_CTX *ctx;
#endif
  int onServiceRef;
} LuaServer;

#define HTTP_POST_BODY_LIMIT 100 * 1024 * 1024

#define REPLY_STATUS_NONE 0        // not reply yet.
#define REPLY_STATUS_REPLYED 1     // replied
#define REPLY_STATUS_REPLY_START 2 // processing reply chunk, but not ended.

typedef struct
{
  struct evhttp_request *req;

  int reply_status;
} Request;

#define LUA_EVHTTP_REQUEST_TYPE "EVHTTP_REQUEST_TYPE"
#define LUA_EVHTTP_SERVER_TYPE "EVHTTP_SERVER_TYPE"

static Request *request_from_table(lua_State *L, int idx)
{
  lua_rawgeti(L, idx, 1);

  Request *request = (Request *)lua_touserdata(L, -1);
  lua_pop(L, 1);

  return request;
}

static void newtable_from_req(lua_State *L, struct evhttp_request *req)
{
  lua_newtable(L);
  Request *request = (Request *)lua_newuserdata(L, sizeof(Request));
  request->reply_status = REPLY_STATUS_NONE;
  request->req = req;
  lua_rawseti(L, -2, 1);
}

static void httpd_handler_cgi_bin(struct evhttp_request *req,
                                  LuaServer *server)
{
  lua_State *mainthread = server->mainthread;
  lua_lock(mainthread);
  lua_State *co = lua_newthread(mainthread);
  PUSH_REF(mainthread);
  lua_unlock(mainthread);

  lua_rawgeti(co, LUA_REGISTRYINDEX, server->onServiceRef);

  newtable_from_req(co, req);

  luaL_getmetatable(co, LUA_EVHTTP_REQUEST_TYPE);
  lua_setmetatable(co, -2);

  lua_pushvalue(co, -1); // duplicate for req,resp

  FAN_RESUME(co, mainthread, 2);
  POP_REF(mainthread);
}

static void request_push_body(lua_State *L, int idx)
{
  if (idx < 0)
  {
    idx = lua_gettop(L) + idx + 1;
  }
  lua_rawgetp(L, idx, "body");
  if (lua_isnil(L, -1))
  {
    lua_pop(L, 1);

    struct evhttp_request *req = request_from_table(L, idx)->req;
    struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
    if (bodybuf)
    {
      size_t len = evbuffer_get_length(bodybuf);
      if (len < HTTP_POST_BODY_LIMIT)
      {
        char *data = calloc(1, len + 1);
        int read = 0;
        char *ptrdata = data;
        while ((read = evbuffer_remove(bodybuf, ptrdata, len)) > 0)
        {
          ptrdata += read;
          len -= read;
        }

        lua_pushlstring(L, data, ptrdata - data);

        free(data);
#if (LUA_VERSION_NUM >= 502)
        lua_pushvalue(L, -1);
        lua_rawsetp(L, idx, "body");
#else
        lua_pushliteral(L, "body");
        lua_pushvalue(L, -2);
        lua_rawset(L, idx);
#endif
        return;
      }
    }

    lua_pushnil(L);
  }
}

LUA_API int lua_evhttp_request_available(lua_State *L)
{
  struct evhttp_request *req = request_from_table(L, 1)->req;
  struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
  if (bodybuf)
  {
    size_t len = evbuffer_get_length(bodybuf);
    lua_pushinteger(L, len);
  }
  else
  {
    lua_pushinteger(L, 0);
  }

  return 1;
}

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

LUA_API int lua_evhttp_request_read(lua_State *L)
{
  struct evhttp_request *req = request_from_table(L, 1)->req;
  struct evbuffer *bodybuf = evhttp_request_get_input_buffer(req);
  size_t evbuffer_length = evbuffer_get_length(bodybuf);

  if (evbuffer_length)
  {
    lua_Integer buff_len =
        luaL_optinteger(L, 2, MIN(READ_BUFF_LEN, evbuffer_length));

    char *data = malloc(buff_len);
    int read = evbuffer_remove(bodybuf, data, buff_len);
    lua_pushlstring(L, data, read);
    free(data);
  }
  else
  {
    lua_pushnil(L);
  }

  return 1;
}

LUA_API int lua_evhttp_request_reply(lua_State *L)
{
  Request *request = request_from_table(L, 1);
  switch (request->reply_status)
  {
  case REPLY_STATUS_REPLYED:
    return luaL_error(L, "reply has completed already.");
  case REPLY_STATUS_REPLY_START:
    evhttp_send_reply_end(request->req);
    request->reply_status = REPLY_STATUS_REPLYED;
    lua_settop(L, 1);
    return 1;
  default:
    break;
  }

  int responseCode = (int)lua_tointeger(L, 2);
  const char *responseMessage = lua_tostring(L, 3);

  size_t responseBuffLen = 0;
  const char *responseBuff = lua_tolstring(L, 4, &responseBuffLen);

  evhttp_add_header(request->req->output_headers, "Connection", "close");

  struct evbuffer *buf = evbuffer_new();
  evbuffer_add(buf, responseBuff, responseBuffLen);

  evhttp_send_reply(request->req, responseCode, responseMessage, buf);
  evbuffer_free(buf);

  request->reply_status = REPLY_STATUS_REPLYED;

  return 0;
}

LUA_API int lua_evhttp_request_reply_addheader(lua_State *L)
{
  Request *request = request_from_table(L, 1);
  switch (request->reply_status)
  {
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

LUA_API int lua_evhttp_request_reply_start(lua_State *L)
{
  Request *request = request_from_table(L, 1);
  switch (request->reply_status)
  {
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

LUA_API int lua_evhttp_request_reply_chunk(lua_State *L)
{
  Request *request = request_from_table(L, 1);
  switch (request->reply_status)
  {
  case REPLY_STATUS_REPLYED:
    return luaL_error(L, "reply has completed already.");
  case REPLY_STATUS_NONE:
    return luaL_error(L, "reply has not started yet.");
  default:
    break;
  }

  int top = lua_gettop(L);
  if (top > 1)
  {
    struct evbuffer *buf = evbuffer_new();
    int i = 2;
    for (; i <= top; i++)
    {
      size_t responseBuffLen = 0;
      const char *responseBuff = lua_tolstring(L, i, &responseBuffLen);
      evbuffer_add(buf, responseBuff, responseBuffLen);
    }
    evhttp_send_reply_chunk(request->req, buf);
    evbuffer_free(buf);
  }

  lua_settop(L, 1);
  return 1;
}

LUA_API int lua_evhttp_request_reply_end(lua_State *L)
{
  Request *request = request_from_table(L, 1);
  switch (request->reply_status)
  {
  case REPLY_STATUS_REPLYED:
    return luaL_error(L, "reply has completed already.");
  case REPLY_STATUS_NONE:
    return luaL_error(L, "reply has not started yet.");
  default:
    break;
  }

  evhttp_send_reply_end(request->req);
  request->reply_status = REPLY_STATUS_REPLYED;

  lua_settop(L, 1);
  return 1;
}

static const struct luaL_Reg evhttp_request_lib[] = {
    {"read", lua_evhttp_request_read},
    {"available", lua_evhttp_request_available},

    {"addheader", lua_evhttp_request_reply_addheader},

    {"reply", lua_evhttp_request_reply},
    {"reply_start", lua_evhttp_request_reply_start},
    {"reply_chunk", lua_evhttp_request_reply_chunk},
    {"reply_end", lua_evhttp_request_reply_end},
    {NULL, NULL},
};

typedef struct
{
  char *name;
  enum evhttp_cmd_type cmd;
} MethodMap;

static const MethodMap methodMap[] = {
    {"GET", EVHTTP_REQ_GET}, {"POST", EVHTTP_REQ_POST}, {"HEAD", EVHTTP_REQ_HEAD}, {"PUT", EVHTTP_REQ_PUT}, {"DELETE", EVHTTP_REQ_DELETE}, {"OPTIONS", EVHTTP_REQ_OPTIONS}, {"TRACE", EVHTTP_REQ_TRACE}, {"CONNECT", EVHTTP_REQ_CONNECT}, {"PATCH", EVHTTP_REQ_PATCH}, {NULL, EVHTTP_REQ_GET}};

LUA_API int lua_evhttp_server_gc(lua_State *L)
{
  LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
  if (server->onServiceRef != LUA_NOREF)
  {
    luaL_unref(L, LUA_REGISTRYINDEX, server->onServiceRef);
    server->onServiceRef = LUA_NOREF;
  }

  if (event_mgr_base_current() && server->httpd)
  {
    if (server->boundsocket)
    {
      evhttp_del_accept_socket(server->httpd, server->boundsocket);
      server->boundsocket = NULL;
    }

    if (server->httpd)
    {
      evhttp_free(server->httpd);
      server->httpd = NULL;
    }
  }

#if FAN_HAS_OPENSSL
  if (server->ctx)
  {
    SSL_CTX_free(server->ctx);
  }
#endif
  lua_pop(L, 1);

  return 0;
}

LUA_API int lua_evhttp_request_lookup(lua_State *L)
{
  struct evhttp_request *req = request_from_table(L, 1)->req;
  const char *p = lua_tostring(L, 2);

  const luaL_Reg *lib;

  for (lib = evhttp_request_lib; lib->func; lib++)
  {
    if (strcmp(p, lib->name) == 0)
    {
      lua_pushcfunction(L, lib->func);
      return 1;
    }
  }

  if (strcmp(p, "path") == 0)
  {
    lua_pushstring(L, evhttp_uri_get_path(req->uri_elems));
    return 1;
  }
  else if (strcmp(p, "query") == 0)
  {
    lua_pushstring(L, evhttp_uri_get_query(req->uri_elems));
    return 1;
  }
  else if (strcmp(p, "method") == 0)
  {
    const MethodMap *method;
    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    for (method = methodMap; method->name; method++)
    {
      if (cmd == method->cmd)
      {
        lua_pushstring(L, method->name);
        return 1;
      }
    }
  }
  else if (strcmp(p, "headers") == 0)
  {
    struct evkeyvalq *headers = evhttp_request_get_input_headers(req);

    lua_newtable(L);
    struct evkeyval *item;
    TAILQ_FOREACH(item, headers, next)
    {
      lua_pushstring(L, item->value);
      lua_setfield(L, -2, item->key);
    }
    return 1;
  }
  else if (strcmp(p, "params") == 0)
  {
    struct evkeyvalq params;
    evhttp_parse_query_str(evhttp_uri_get_query(req->uri_elems), &params);

    lua_newtable(L);
    struct evkeyval *item;
    TAILQ_FOREACH(item, &params, next)
    {
      lua_pushstring(L, item->value);
      lua_setfield(L, -2, item->key);
    }
      evhttp_clear_headers(&params);

    struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
    const char *contentType = evhttp_find_header(headers, "Content-Type");
    if (contentType &&
        strstr(contentType, "application/x-www-form-urlencoded") ==
            contentType)
    {
      request_push_body(L, 1);
      if (lua_type(L, -1) == LUA_TSTRING)
      {
        const char *data = lua_tostring(L, -1);
        struct evkeyvalq params;
        evhttp_parse_query_str(data, &params);
        lua_pop(L, 1);

        TAILQ_FOREACH(item, &params, next)
        {
          lua_pushstring(L, item->value);
          lua_setfield(L, -2, item->key);
        }
          evhttp_clear_headers(&params);
      }
    }
    return 1;
  }
  else if (strcmp(p, "body") == 0)
  {
    request_push_body(L, 1);
    return 1;
  }
  else if (strcmp(p, "remoteip") == 0)
  {
    char *address = NULL;
    ev_uint16_t port = 0;
    evhttp_connection_get_peer(req->evcon, &address, &port);

    lua_pushstring(L, address);

    return 1;
  }
  else if (strcmp(p, "remoteport") == 0)
  {
    char *address = NULL;
    ev_uint16_t port = 0;
    evhttp_connection_get_peer(req->evcon, &address, &port);

    lua_pushinteger(L, port);

    return 1;
  }

  return 0;
}

#if FAN_HAS_OPENSSL

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
static struct bufferevent *bevcb(struct event_base *base, void *arg)
{
  struct bufferevent *r;
  SSL_CTX *ctx = (SSL_CTX *)arg;

  r = bufferevent_openssl_socket_new(
      base, -1, SSL_new(ctx), BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
  return r;
}
#endif
#endif

#endif

static void smoke_request_cb(struct evhttp_request *req, void *arg)
{
  evhttp_send_reply(req, 200, "OK", NULL);
}

void httpd_server_rebind(lua_State *L, LuaServer *server)
{
  struct evhttp_bound_socket *boundsocket =
      evhttp_bind_socket_with_handle(server->httpd, server->host, server->port);

  server->boundsocket = boundsocket;
  if (boundsocket)
  {
    server->port = regress_get_socket_port(evhttp_bound_socket_get_fd(boundsocket));
  }
  else
  {
    server->port = 0;
  }
}

LUA_API int utd_bind(lua_State *L)
{
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

  struct evhttp *httpd = evhttp_new(event_mgr_base());

#if FAN_HAS_OPENSSL
  server->ctx = NULL;

#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
  lua_getfield(L, 1, "cert");
  const char *cert = lua_tostring(L, -1);
  lua_getfield(L, 1, "key");
  const char *key = lua_tostring(L, -1);

  if (cert && key)
  {
    SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
    server->ctx = ctx;
    SSL_CTX_set_options(ctx,
                        SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE |
                            0); // SSL_OP_NO_SSLv2

    EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!ecdh)
    {
      die_most_horribly_from_openssl_error("EC_KEY_new_by_curve_name");
    }

    if (1 != SSL_CTX_set_tmp_ecdh(ctx, ecdh))
    {
      die_most_horribly_from_openssl_error("SSL_CTX_set_tmp_ecdh");
    }

    server_setup_certs(ctx, cert, key);

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

  if (!server->boundsocket)
  {
#if FAN_HAS_OPENSSL
    if (server->ctx)
    {
      SSL_CTX_free(server->ctx);
      server->ctx = NULL;
    }
#endif
    evhttp_free(httpd);
    server->httpd = NULL;
    return 0;
  }

  SET_FUNC_REF_FROM_TABLE(L, server->onServiceRef, 1, "onService")

  evhttp_set_timeout(httpd, 120);
  evhttp_set_cb(httpd, "/smoketest", smoke_request_cb, NULL);
  evhttp_set_gencb(
      httpd, (void (*)(struct evhttp_request *, void *))httpd_handler_cgi_bin,
      server);

  lua_newtable(L);
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, "serv");

  lua_pushinteger(L, server->port);
  lua_setfield(L, -2, "port");

  if (server->host)
  {
    lua_getfield(L, 1, "host");
  }
  else
  {
    lua_pushliteral(L, "0.0.0.0");
  }
  lua_setfield(L, -2, "host");

  return 1;
}

LUA_API int lua_evhttp_server_rebind(lua_State *L)
{
  LuaServer *server = (LuaServer *)luaL_checkudata(L, 1, LUA_EVHTTP_SERVER_TYPE);
  httpd_server_rebind(L, server);
  return 0;
}

static const luaL_Reg utdlib[] = {{"bind", utd_bind}, {NULL, NULL}};

LUA_API int luaopen_fan_httpd_core(lua_State *L)
{
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
