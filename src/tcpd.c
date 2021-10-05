
#include "utlua.h"
#ifdef __linux__
#include <limits.h>
#include <linux/netfilter_ipv4.h>
#endif

#include <net/if.h>

#define LUA_TCPD_CONNECTION_TYPE "<tcpd.connect>"
#define LUA_TCPD_SERVER_TYPE "<tcpd.bind %s %d>"
#define LUA_TCPD_ACCEPT_TYPE "<tcpd.accept %s %d>"

#if FAN_HAS_OPENSSL
typedef struct
{
  SSL_CTX *ssl_ctx;
  char *key;
  int retainCount;
} SSLCTX;
#endif

typedef struct
{
  struct bufferevent *buf;

#if FAN_HAS_OPENSSL
  SSLCTX *sslctx;

  int ssl_verifyhost;
  int ssl_verifypeer;
  const char *ssl_error;
#endif

  lua_State *mainthread;
  int onReadRef;
  int onSendReadyRef;

  int onDisconnectedRef;
  int onConnectedRef;

  char *host;
  char *ssl_host;
  int port;

  int send_buffer_size;
  int receive_buffer_size;

  int interface;

  lua_Number read_timeout;
  lua_Number write_timeout;
} Conn;

#if FAN_HAS_OPENSSL
#define VERIFY_DEPTH 5
static int conn_index = 0;
#endif

typedef struct
{
  struct evconnlistener *listener;
  lua_State *mainthread;

  int onAcceptRef;
  int onSSLHostNameRef;

  char *host;
  int port;

  int ipv6;

#if FAN_HAS_OPENSSL
  int ssl;
  SSL_CTX *ctx;
  EC_KEY *ecdh;
#endif

  int send_buffer_size;
  int receive_buffer_size;
} SERVER;

typedef struct
{
  struct bufferevent *buf;
  lua_State *mainthread;

  int onReadRef;
  int onSendReadyRef;

  int selfRef;

  char ip[INET6_ADDRSTRLEN];
  int port;

  int onDisconnectedRef;
} ACCEPT;

#define TCPD_ACCEPT_UNREF(accept)                          \
  CLEAR_REF(accept->mainthread, accept->onSendReadyRef)    \
  CLEAR_REF(accept->mainthread, accept->onReadRef)         \
  CLEAR_REF(accept->mainthread, accept->onDisconnectedRef) \
  CLEAR_REF(accept->mainthread, accept->selfRef)

LUA_API int lua_tcpd_server_close(lua_State *L)
{
  SERVER *serv = luaL_checkudata(L, 1, LUA_TCPD_SERVER_TYPE);
  CLEAR_REF(L, serv->onAcceptRef)
  CLEAR_REF(L, serv->onSSLHostNameRef)

  if (serv->host)
  {
    free(serv->host);
    serv->host = NULL;
  }

  if (event_mgr_base_current() && serv->listener)
  {
    evconnlistener_free(serv->listener);
    serv->listener = NULL;
  }

#if FAN_HAS_OPENSSL
  if (serv->ctx)
  {
    SSL_CTX_free(serv->ctx);
    EC_KEY_free(serv->ecdh);
    serv->ctx = NULL;
    serv->ecdh = NULL;
  }
#endif

  return 0;
}

LUA_API int lua_tcpd_server_gc(lua_State *L)
{
  return lua_tcpd_server_close(L);
}

LUA_API int lua_tcpd_accept_tostring(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  lua_pushfstring(L, LUA_TCPD_ACCEPT_TYPE, accept->ip, accept->port);
  return 1;
}

LUA_API int lua_tcpd_server_tostring(lua_State *L)
{
  SERVER *serv = luaL_checkudata(L, 1, LUA_TCPD_SERVER_TYPE);
  if (serv->listener)
  {
    char host[INET6_ADDRSTRLEN];
    regress_get_socket_host(evconnlistener_get_fd(serv->listener), host);
    lua_pushfstring(
        L, LUA_TCPD_SERVER_TYPE, host,
        regress_get_socket_port(evconnlistener_get_fd(serv->listener)));
  }
  else
  {
    lua_pushfstring(L, LUA_TCPD_SERVER_TYPE, 0);
  }

  return 1;
}

static void tcpd_accept_eventcb(struct bufferevent *bev, short events,
                                void *arg)
{
  ACCEPT *accept = (ACCEPT *)arg;

  if (events & BEV_EVENT_ERROR || events & BEV_EVENT_EOF ||
      events & BEV_EVENT_TIMEOUT)
  {
    if (events & BEV_EVENT_ERROR)
    {
#if DEBUG
      printf("BEV_EVENT_ERROR %s\n",
             evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
#endif
    }
    bufferevent_free(bev);
    accept->buf = NULL;

    if (accept->onDisconnectedRef != LUA_NOREF)
    {
      lua_State *mainthread = accept->mainthread;
      lua_lock(mainthread);
      lua_State *co = lua_newthread(mainthread);
      PUSH_REF(mainthread);
      lua_unlock(mainthread);

      lua_rawgeti(co, LUA_REGISTRYINDEX, accept->onDisconnectedRef);

      if (events & BEV_EVENT_ERROR && EVUTIL_SOCKET_ERROR())
      {
        lua_pushstring(co,
                       evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
      }
      else if (events & BEV_EVENT_TIMEOUT)
      {
        lua_pushstring(co, "timeout");
      }
      else if (events & BEV_EVENT_EOF)
      {
        lua_pushstring(co, "client disconnected");
      }
      else
      {
        lua_pushnil(co);
      }

      CLEAR_REF(mainthread, accept->onDisconnectedRef)

      FAN_RESUME(co, mainthread, 1);
      POP_REF(mainthread);
    }

    TCPD_ACCEPT_UNREF(accept)
  }
  else
  {
  }
}

#define BUFLEN 1024

static void tcpd_accept_readcb(struct bufferevent *bev, void *ctx)
{
  ACCEPT *accept = (ACCEPT *)ctx;

  char buf[BUFLEN];
  int n;
  BYTEARRAY ba = {0};
  bytearray_alloc(&ba, BUFLEN * 2);
  struct evbuffer *input = bufferevent_get_input(bev);
  while ((n = evbuffer_remove(input, buf, sizeof(buf))) > 0)
  {
    bytearray_writebuffer(&ba, buf, n);
  }
  bytearray_read_ready(&ba);

  if (accept->onReadRef != LUA_NOREF)
  {
    lua_State *mainthread = accept->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, accept->onReadRef);
    lua_pushlstring(co, (const char *)ba.buffer, ba.total);
    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);
  }

  bytearray_dealloc(&ba);
}

static void tcpd_accept_writecb(struct bufferevent *bev, void *ctx)
{
  ACCEPT *accept = (ACCEPT *)ctx;

  if (evbuffer_get_length(bufferevent_get_output(bev)) == 0)
  {
    if (accept->onSendReadyRef != LUA_NOREF)
    {
      lua_State *mainthread = accept->mainthread;
      lua_lock(mainthread);
      lua_State *co = lua_newthread(mainthread);
      PUSH_REF(mainthread);
      lua_unlock(mainthread);

      lua_rawgeti(co, LUA_REGISTRYINDEX, accept->onSendReadyRef);
      FAN_RESUME(co, mainthread, 0);
      POP_REF(mainthread);
    }
  }
}

void connlistener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                     struct sockaddr *addr, int socklen, void *arg)
{
  SERVER *serv = (SERVER *)arg;

  if (serv->onAcceptRef != LUA_NOREF)
  {
    lua_State *mainthread = serv->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, serv->onAcceptRef);

    ACCEPT *accept = lua_newuserdata(co, sizeof(ACCEPT));
    memset(accept, 0, sizeof(ACCEPT));
    accept->buf = NULL;
    accept->mainthread = mainthread;
    accept->selfRef = LUA_NOREF;
    accept->onReadRef = LUA_NOREF;
    accept->onSendReadyRef = LUA_NOREF;
    accept->onDisconnectedRef = LUA_NOREF;

    luaL_getmetatable(co, LUA_TCPD_ACCEPT_TYPE);
    lua_setmetatable(co, -2);

    struct event_base *base = evconnlistener_get_base(listener);

    struct bufferevent *bev;

#if FAN_HAS_OPENSSL
    if (serv->ssl && serv->ctx)
    {
      bev = bufferevent_openssl_socket_new(
          base, fd, SSL_new(serv->ctx), BUFFEREVENT_SSL_ACCEPTING,
          BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    }
    else
    {
#endif
      bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
#if FAN_HAS_OPENSSL
    }
#endif

    bufferevent_setcb(bev, tcpd_accept_readcb, tcpd_accept_writecb,
                      tcpd_accept_eventcb, accept);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    if (serv->send_buffer_size)
    {
      setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &serv->send_buffer_size,
                 sizeof(serv->send_buffer_size));
    }
    if (serv->receive_buffer_size)
    {
      setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &serv->receive_buffer_size,
                 sizeof(serv->receive_buffer_size));
    }

    memset(accept->ip, 0, INET6_ADDRSTRLEN);
    if (addr->sa_family == AF_INET)
    {
      struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
      inet_ntop(addr_in->sin_family, (void *)&(addr_in->sin_addr), accept->ip,
                INET_ADDRSTRLEN);
      accept->port = ntohs(addr_in->sin_port);
    }
    else
    {
      struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)addr;
      inet_ntop(addr_in->sin6_family, (void *)&(addr_in->sin6_addr), accept->ip,
                INET6_ADDRSTRLEN);
      accept->port = ntohs(addr_in->sin6_port);
    }

    accept->buf = bev;

    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);
  }
}

LUA_API int tcpd_accept_bind(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

  luaL_checktype(L, 2, LUA_TTABLE);
  lua_settop(L, 2);

  lua_pushvalue(L, 1);
  accept->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

  SET_FUNC_REF_FROM_TABLE(L, accept->onReadRef, 2, "onread")
  SET_FUNC_REF_FROM_TABLE(L, accept->onSendReadyRef, 2, "onsendready")
  SET_FUNC_REF_FROM_TABLE(L, accept->onDisconnectedRef, 2, "ondisconnected")

  lua_pushstring(L, accept->ip);
  lua_pushinteger(L, accept->port);

  return 2;
}

#if FAN_HAS_OPENSSL

static int ssl_servername_cb(SSL *s, int *ad, void *arg)
{
  const char *hostname = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
  // if (hostname)
  //   printf("Hostname in TLS extension: \"%s\"\n", hostname);

  SERVER *serv = (SERVER *)arg;
  if (hostname && serv->onSSLHostNameRef != LUA_NOREF)
  {
    lua_State *mainthread = serv->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, serv->onSSLHostNameRef);
    lua_pushstring(co, hostname);
    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);
  }
  // if (!p->servername)
  //     return SSL_TLSEXT_ERR_NOACK;

  // if (servername) {
  //     if (strcasecmp(servername, p->servername))
  //         return p->extension_error;
  // }
  return SSL_TLSEXT_ERR_OK;
}

#endif

static void tcpd_server_rebind(lua_State *L, SERVER *serv)
{
  if (serv->listener)
  {
    evconnlistener_free(serv->listener);
    serv->listener = NULL;
  }
  if (serv->host)
  {
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", serv->port);

    struct evutil_addrinfo hints = {0};
    struct evutil_addrinfo *answer = NULL;
    hints.ai_family = serv->ipv6 ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
    int err = evutil_getaddrinfo(serv->host, portbuf, &hints, &answer);
    if (err < 0 || !answer)
    {
      luaL_error(L, "invaild bind address %s:%d", serv->host, serv->port);
    }

    serv->listener =
        evconnlistener_new_bind(event_mgr_base(), connlistener_cb, serv,
                                LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
                                answer->ai_addr, answer->ai_addrlen);
    evutil_freeaddrinfo(answer);
  }
  else
  {
    struct sockaddr *addr = NULL;
    size_t addr_size = 0;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;

    memset(&sin, 0, sizeof(sin));
    memset(&sin6, 0, sizeof(sin6));

    if (!serv->ipv6)
    {
      addr = (struct sockaddr *)&sin;
      addr_size = sizeof(sin);

      sin.sin_family = AF_INET;
      sin.sin_addr.s_addr = htonl(0);
      sin.sin_port = htons(serv->port);
    }
    else
    {
      addr = (struct sockaddr *)&sin6;
      addr_size = sizeof(sin6);

      sin6.sin6_family = AF_INET6;
      // sin6.sin6_addr.s6_addr
      sin6.sin6_port = htons(serv->port);
    }

    serv->listener = evconnlistener_new_bind(
        event_mgr_base(), connlistener_cb, serv,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, addr, addr_size);
  }
}

LUA_API int lua_tcpd_server_rebind(lua_State *L)
{
  SERVER *serv = luaL_checkudata(L, 1, LUA_TCPD_SERVER_TYPE);
  tcpd_server_rebind(L, serv);
  return 0;
}

LUA_API int tcpd_bind(lua_State *L)
{
  event_mgr_init();
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);

  SERVER *serv = lua_newuserdata(L, sizeof(SERVER));
  memset(serv, 0, sizeof(SERVER));
  luaL_getmetatable(L, LUA_TCPD_SERVER_TYPE);
  lua_setmetatable(L, -2);

  serv->mainthread = utlua_mainthread(L);

  SET_FUNC_REF_FROM_TABLE(L, serv->onAcceptRef, 1, "onaccept")
  SET_FUNC_REF_FROM_TABLE(L, serv->onSSLHostNameRef, 1, "onsslhostname")

  DUP_STR_FROM_TABLE(L, serv->host, 1, "host")
  SET_INT_FROM_TABLE(L, serv->port, 1, "port")

  lua_getfield(L, 1, "ssl");
  int ssl = lua_toboolean(L, -1);
  lua_pop(L, 1);

#if FAN_HAS_OPENSSL
  serv->ssl = ssl;

  if (serv->ssl)
  {
    lua_getfield(L, 1, "cert");
    const char *cert = lua_tostring(L, -1);
    lua_getfield(L, 1, "key");
    const char *key = lua_tostring(L, -1);

    if (cert && key)
    {
      SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
      SSL_CTX_set_tlsext_servername_callback(ctx, ssl_servername_cb);
      SSL_CTX_set_tlsext_servername_arg(ctx, serv);
      serv->ctx = ctx;
      SSL_CTX_set_options(ctx,
                          SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE |
                              0); // SSL_OP_NO_SSLv2

      serv->ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
      if (!serv->ecdh)
      {
        die_most_horribly_from_openssl_error("EC_KEY_new_by_curve_name");
      }

      if (1 != SSL_CTX_set_tmp_ecdh(ctx, serv->ecdh))
      {
        die_most_horribly_from_openssl_error("SSL_CTX_set_tmp_ecdh");
      }

      server_setup_certs(ctx, cert, key);
    }

    lua_pop(L, 2);
  }
#else
  if (ssl)
  {
    luaL_error(L, "ssl is not supported on micro version.");
  }
#endif

  SET_INT_FROM_TABLE(L, serv->send_buffer_size, 1, "send_buffer_size")
  SET_INT_FROM_TABLE(L, serv->receive_buffer_size, 1, "receive_buffer_size")

  lua_getfield(L, 1, "ipv6");
  serv->ipv6 = lua_toboolean(L, -1);
  lua_pop(L, 1);

  tcpd_server_rebind(L, serv);

  if (!serv->listener)
  {
    return 0;
  }
  else
  {
    if (!serv->port)
    {
      serv->port = regress_get_socket_port(evconnlistener_get_fd(serv->listener));
    }

    lua_pushinteger(L, serv->port);
    return 2;
  }
}

static void tcpd_conn_readcb(struct bufferevent *bev, void *ctx)
{
  Conn *conn = (Conn *)ctx;

  char buf[BUFLEN];
  int n;
  BYTEARRAY ba;
  bytearray_alloc(&ba, BUFLEN * 2);
  struct evbuffer *input = bufferevent_get_input(bev);
  while ((n = evbuffer_remove(input, buf, sizeof(buf))) > 0)
  {
    bytearray_writebuffer(&ba, buf, n);
  }
  bytearray_read_ready(&ba);

  if (conn->onReadRef != LUA_NOREF)
  {
    lua_State *mainthread = conn->mainthread;
    lua_lock(mainthread);
    lua_rawgeti(mainthread, LUA_REGISTRYINDEX, conn->onReadRef);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_xmove(mainthread, co, 1);
    lua_unlock(mainthread);

    lua_pushlstring(co, (const char *)ba.buffer, ba.total);
    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);
  }

  bytearray_dealloc(&ba);
}

static void tcpd_conn_writecb(struct bufferevent *bev, void *ctx)
{
  Conn *conn = (Conn *)ctx;

  if (evbuffer_get_length(bufferevent_get_output(bev)) == 0)
  {
    if (conn->onSendReadyRef != LUA_NOREF)
    {
      lua_State *mainthread = conn->mainthread;
      lua_lock(mainthread);
      lua_State *co = lua_newthread(mainthread);
      PUSH_REF(mainthread);
      lua_unlock(mainthread);

      lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onSendReadyRef);
      FAN_RESUME(co, mainthread, 0);
      POP_REF(mainthread);
    }
  }
}

static void tcpd_conn_eventcb(struct bufferevent *bev, short events,
                              void *arg)
{
  Conn *conn = (Conn *)arg;

  if (events & BEV_EVENT_CONNECTED)
  {
    //        printf("tcp connected.\n");

    if (conn->onConnectedRef != LUA_NOREF)
    {
      lua_State *mainthread = conn->mainthread;
      lua_lock(mainthread);
      lua_State *co = lua_newthread(mainthread);
      PUSH_REF(mainthread);
      lua_unlock(mainthread);

      lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onConnectedRef);
      FAN_RESUME(co, mainthread, 0);
      POP_REF(mainthread);
    }
  }
  else if (events & BEV_EVENT_ERROR || events & BEV_EVENT_EOF ||
           events & BEV_EVENT_TIMEOUT)
  {
#if FAN_HAS_OPENSSL
    SSL *ssl = bufferevent_openssl_get_ssl(bev);
    if (ssl)
    {
      SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
      SSL_shutdown(ssl);
    }
#endif
    bufferevent_free(bev);
    conn->buf = NULL;

    if (conn->onDisconnectedRef != LUA_NOREF)
    {
      lua_State *mainthread = conn->mainthread;
      lua_lock(mainthread);
      lua_State *co = lua_newthread(mainthread);
      PUSH_REF(mainthread);
      lua_unlock(mainthread);

      lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onDisconnectedRef);
      if (events & BEV_EVENT_TIMEOUT)
      {
        if (events & BEV_EVENT_READING)
        {
          lua_pushliteral(co, "read timeout");
        }
        else if (events & BEV_EVENT_WRITING)
        {
          lua_pushliteral(co, "write timeout");
        }
        else
        {
          lua_pushliteral(co, "unknown timeout");
        }
      }
      else if (events & BEV_EVENT_ERROR)
      {
#if FAN_HAS_OPENSSL
        if (conn->ssl_error)
        {
          lua_pushfstring(co, "SSLError: %s", conn->ssl_error);
        }
        else
        {
#endif
          int err = bufferevent_socket_get_dns_error(bev);

          if (err)
          {
            lua_pushstring(co, evutil_gai_strerror(err));
          }
          else if (EVUTIL_SOCKET_ERROR())
          {
            lua_pushstring(
                co, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
          }
          else
          {
            lua_pushnil(co);
          }
#if FAN_HAS_OPENSSL
        }
#endif
      }
      else if (events & BEV_EVENT_EOF)
      {
        lua_pushliteral(co, "server disconnected");
      }
      else
      {
        lua_pushnil(co);
      }
      FAN_RESUME(co, mainthread, 1);
      POP_REF(mainthread);
    }
  }
}

#if FAN_HAS_OPENSSL

static int ssl_verifypeer_cb(int preverify_ok, X509_STORE_CTX *ctx)
{
  if (!preverify_ok)
  {
    SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    Conn *conn = SSL_get_ex_data(ssl, conn_index);

    int err = X509_STORE_CTX_get_error(ctx);
    conn->ssl_error = strdup(X509_verify_cert_error_string(err));
  }

  return preverify_ok;
}

#endif

static void luatcpd_reconnect(Conn *conn)
{
  if (conn->buf)
  {
    bufferevent_free(conn->buf);
    conn->buf = NULL;
  }
#if FAN_HAS_OPENSSL
  conn->ssl_error = 0;

  if (conn->sslctx)
  {
    SSL *ssl = SSL_new(conn->sslctx->ssl_ctx);
    SSL_set_ex_data(ssl, conn_index, conn);

    if (conn->ssl_verifyhost && (conn->ssl_host ?: conn->host))
    {
      SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
      if (!SSL_set1_host(ssl, conn->ssl_host ?: conn->host))
      {
        printf("SSL_set1_host '%s' failed!\n", conn->ssl_host ?: conn->host);
      }
    }

    /* Enable peer verification (with a non-null callback if desired) */
    if (conn->ssl_verifypeer)
    {
      SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
    }
    else
    {
      SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
    }

    SSL_set_tlsext_host_name(ssl, conn->ssl_host ?: conn->host);
    conn->buf = bufferevent_openssl_socket_new(
        event_mgr_base(), -1, ssl, BUFFEREVENT_SSL_CONNECTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
#ifdef EVENT__NUMERIC_VERSION
#if (EVENT__NUMERIC_VERSION >= 0x02010500)
    bufferevent_openssl_set_allow_dirty_shutdown(conn->buf, 1);
#endif
#endif
  }
  else
  {
#endif
    conn->buf = bufferevent_socket_new(
        event_mgr_base(), -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
#if FAN_HAS_OPENSSL
  }
#endif

  int rc = bufferevent_socket_connect_hostname(conn->buf, event_mgr_dnsbase(),
                                               AF_UNSPEC,
                                               conn->host, conn->port);

  evutil_socket_t fd = bufferevent_getfd(conn->buf);
  if (conn->send_buffer_size)
  {
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &conn->send_buffer_size,
               sizeof(conn->send_buffer_size));
  }
  if (conn->receive_buffer_size)
  {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &conn->receive_buffer_size,
               sizeof(conn->receive_buffer_size));
  }

#ifdef IP_BOUND_IF
  if (conn->interface)
  {
    setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &conn->interface, sizeof(conn->interface));
  }
#endif

  if (rc < 0)
  {
    LOGE("could not connect to %s:%d %s", conn->host, conn->port,
         evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    bufferevent_free(conn->buf);
    conn->buf = NULL;
    return;
  }

  bufferevent_enable(conn->buf, EV_WRITE | EV_READ);
  bufferevent_setcb(conn->buf, tcpd_conn_readcb, tcpd_conn_writecb,
                    tcpd_conn_eventcb, conn);
}

LUA_API int tcpd_connect(lua_State *L)
{
  event_mgr_init();
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);

  Conn *conn = lua_newuserdata(L, sizeof(Conn));
  memset(conn, 0, sizeof(Conn));
  luaL_getmetatable(L, LUA_TCPD_CONNECTION_TYPE);
  lua_setmetatable(L, -2);

  conn->mainthread = utlua_mainthread(L);

  conn->buf = NULL;
#if FAN_HAS_OPENSSL
  conn->sslctx = NULL;
  conn->ssl_error = 0;
#endif
  conn->send_buffer_size = 0;
  conn->receive_buffer_size = 0;

  SET_FUNC_REF_FROM_TABLE(L, conn->onReadRef, 1, "onread")
  SET_FUNC_REF_FROM_TABLE(L, conn->onSendReadyRef, 1, "onsendready")
  SET_FUNC_REF_FROM_TABLE(L, conn->onDisconnectedRef, 1, "ondisconnected")
  SET_FUNC_REF_FROM_TABLE(L, conn->onConnectedRef, 1, "onconnected")

  DUP_STR_FROM_TABLE(L, conn->host, 1, "host")
  SET_INT_FROM_TABLE(L, conn->port, 1, "port")

  lua_getfield(L, 1, "ssl");
  int ssl = lua_toboolean(L, -1);
  lua_pop(L, 1);

#if FAN_HAS_OPENSSL
  lua_getfield(L, 1, "ssl_verifyhost");
  conn->ssl_verifyhost = (int)luaL_optinteger(L, -1, 1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "ssl_verifypeer");
  conn->ssl_verifypeer = (int)luaL_optinteger(L, -1, 1);
  lua_pop(L, 1);

  DUP_STR_FROM_TABLE(L, conn->ssl_host, 1, "ssl_host")

  if (ssl)
  {
    lua_getfield(L, 1, "cainfo");
    const char *cainfo = luaL_optstring(L, -1, NULL);
    lua_pop(L, 1);

    lua_getfield(L, 1, "capath");
    const char *capath = luaL_optstring(L, -1, NULL);
    lua_pop(L, 1);

    lua_getfield(L, 1, "pkcs12.path");
    const char *p12path = luaL_optstring(L, -1, NULL);
    lua_pop(L, 1);

    lua_getfield(L, 1, "pkcs12.password");
    const char *p12password = luaL_optstring(L, -1, NULL);
    lua_pop(L, 1);

    if (!cainfo && !capath)
    {
      cainfo = "cert.pem";
    }

    BYTEARRAY ba = {0};
    bytearray_alloc(&ba, BUFLEN);
    bytearray_writebuffer(&ba, "SSL_CTX:", strlen("SSL_CTX_"));

    if (cainfo)
    {
      bytearray_writebuffer(&ba, cainfo, strlen(cainfo));
    }
    if (capath)
    {
      bytearray_writebuffer(&ba, capath, strlen(capath));
    }

    if (p12path)
    {
      bytearray_writebuffer(&ba, p12path, strlen(p12path));
    }
    if (p12password)
    {
      bytearray_writebuffer(&ba, p12password, strlen(p12password));
    }

    bytearray_write8(&ba, 0);
    bytearray_read_ready(&ba);

    char *cache_key = strdup((const char *)ba.buffer);

    bytearray_dealloc(&ba);

    lua_getfield(L, LUA_REGISTRYINDEX, cache_key);
    if (lua_isnil(L, -1))
    {
      SSLCTX *sslctx = lua_newuserdata(L, sizeof(SSLCTX));
      sslctx->key = strdup(cache_key);
      sslctx->ssl_ctx = SSL_CTX_new(TLS_method());
      conn->sslctx = sslctx;
      sslctx->retainCount = 1;
      lua_setfield(L, LUA_REGISTRYINDEX, cache_key);

      if (!SSL_CTX_load_verify_locations(sslctx->ssl_ctx, cainfo, capath))
      {
        printf("SSL_CTX_load_verify_locations failed: cainfo=%s capath=%s\n", cainfo, capath);
      }
#ifdef SSL_MODE_RELEASE_BUFFERS
      SSL_CTX_set_mode(sslctx->ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
#endif
      SSL_CTX_set_options(sslctx->ssl_ctx, SSL_OP_NO_COMPRESSION);
      SSL_CTX_set_verify(sslctx->ssl_ctx, SSL_VERIFY_PEER, ssl_verifypeer_cb);

      while (p12path)
      {
        FILE *fp = NULL;
        EVP_PKEY *pkey = NULL;
        X509 *cert = NULL;
        STACK_OF(X509) *ca = NULL;
        PKCS12 *p12 = NULL;

        if ((fp = fopen(p12path, "rb")) == NULL)
        {
          fprintf(stderr, "Error opening file %s\n", p12path);
          break;
        }
        p12 = d2i_PKCS12_fp(fp, NULL);
        fclose(fp);

        if (!p12)
        {
          fprintf(stderr, "Error reading PKCS#12 file\n");
          ERR_print_errors_fp(stderr);
          break;
        }

        if (!PKCS12_parse(p12, p12password, &pkey, &cert, &ca))
        {
          fprintf(stderr, "Error parsing PKCS#12 file\n");
          ERR_print_errors_fp(stderr);
        }
        else
        {
          SSL_CTX_use_certificate(sslctx->ssl_ctx, cert);
          if (ca && sk_X509_num(ca))
          {
            int i = 0;
            for (i = 0; i < sk_X509_num(ca); i++)
            {
              SSL_CTX_use_certificate(sslctx->ssl_ctx, sk_X509_value(ca, i));
            }
          }
          SSL_CTX_use_PrivateKey(sslctx->ssl_ctx, pkey);

          sk_X509_pop_free(ca, X509_free);
          X509_free(cert);
          EVP_PKEY_free(pkey);
        }

        PKCS12_free(p12);
        p12 = NULL;

        break;
      }
    }
    else
    {
      SSLCTX *sslctx = lua_touserdata(L, -1);
      sslctx->retainCount++;
      conn->sslctx = sslctx;
    }
    lua_pop(L, 1);

    FREE_STR(cache_key);
  }
#else
  if (ssl)
  {
    luaL_error(L, "ssl is not supported on micro version.");
  }
#endif

  SET_INT_FROM_TABLE(L, conn->send_buffer_size, 1, "send_buffer_size")
  SET_INT_FROM_TABLE(L, conn->receive_buffer_size, 1, "receive_buffer_size")

  lua_getfield(L, 1, "read_timeout");
  lua_Number read_timeout = (int)luaL_optnumber(L, -1, 0);
  conn->read_timeout = read_timeout;
  lua_pop(L, 1);

  lua_getfield(L, 1, "write_timeout");
  lua_Number write_timeout = (int)luaL_optnumber(L, -1, 0);
  conn->write_timeout = write_timeout;
  lua_pop(L, 1);

  lua_getfield(L, 1, "interface");
  if (lua_type(L, -1) == LUA_TSTRING)
  {
    const char *interface = lua_tostring(L, -1);
    conn->interface = if_nametoindex(interface);
  }
  lua_pop(L, 1);

  luatcpd_reconnect(conn);
  return 1;
}

static const luaL_Reg tcpdlib[] = {
    {"bind", tcpd_bind}, {"connect", tcpd_connect}, {NULL, NULL}};

LUA_API int tcpd_conn_close(lua_State *L)
{
  Conn *conn = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);
  if (event_mgr_base_current() && conn->buf)
  {
    bufferevent_free(conn->buf);
    conn->buf = NULL;
  }

  CLEAR_REF(L, conn->onReadRef)
  CLEAR_REF(L, conn->onSendReadyRef)
  CLEAR_REF(L, conn->onDisconnectedRef)
  CLEAR_REF(L, conn->onConnectedRef)

  FREE_STR(conn->host)
  FREE_STR(conn->ssl_host)

#if FAN_HAS_OPENSSL
  if (conn->sslctx)
  {
    conn->sslctx->retainCount--;
    if (conn->sslctx->retainCount <= 0)
    {
      lua_pushnil(L);
      lua_setfield(L, LUA_REGISTRYINDEX, conn->sslctx->key);

      SSL_CTX_free(conn->sslctx->ssl_ctx);
      free(conn->sslctx->key);
    }
    conn->sslctx = NULL;
  }
#endif
  return 0;
}

LUA_API int tcpd_conn_gc(lua_State *L) { return tcpd_conn_close(L); }

LUA_API int tcpd_accept_remote(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);

  lua_newtable(L);
  lua_pushstring(L, accept->ip);
  lua_setfield(L, -2, "ip");

  lua_pushinteger(L, accept->port);
  lua_setfield(L, -2, "port");

  return 1;
}

#ifdef __linux__
LUA_API int tcpd_accept_original_dst(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  evutil_socket_t fd = bufferevent_getfd(accept->buf);

  struct sockaddr_storage ss;
  socklen_t len = sizeof(struct sockaddr_storage);
  if (getsockopt(fd, SOL_IP, SO_ORIGINAL_DST, &ss, &len))
  {
    lua_pushnil(L);
    lua_pushfstring(L, "getsockopt: %s", strerror(errno));
    return 2;
  }

  char host[INET6_ADDRSTRLEN];
  int port = 0;
  if (ss.ss_family == AF_INET)
  {
    struct sockaddr_in *addr_in = (struct sockaddr_in *)&ss;
    port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    inet_ntop(addr_in->sin_family, (void *)&(addr_in->sin_addr), host,
              INET_ADDRSTRLEN);
  }
  else if (ss.ss_family == AF_INET6)
  {
    struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)&ss;
    port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    inet_ntop(addr_in->sin6_family, (void *)&(addr_in->sin6_addr), host,
              INET6_ADDRSTRLEN);
  }

  lua_pushstring(L, host);
  lua_pushinteger(L, port);
  return 2;
}

#endif

LUA_API int tcpd_accept_getsockname(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  evutil_socket_t fd = bufferevent_getfd(accept->buf);

  struct sockaddr_storage ss;
  socklen_t len = sizeof(struct sockaddr_storage);
  if (getsockname(fd, (struct sockaddr *)&ss, &len))
  {
    lua_pushnil(L);
    lua_pushfstring(L, "getsockname: %s", strerror(errno));
    return 2;
  }

  char host[INET6_ADDRSTRLEN];
  int port = 0;
  if (ss.ss_family == AF_INET)
  {
    struct sockaddr_in *addr_in = (struct sockaddr_in *)&ss;
    port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    inet_ntop(addr_in->sin_family, (void *)&(addr_in->sin_addr), host,
              INET_ADDRSTRLEN);
  }
  else if (ss.ss_family == AF_INET6)
  {
    struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)&ss;
    port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    inet_ntop(addr_in->sin6_family, (void *)&(addr_in->sin6_addr), host,
              INET6_ADDRSTRLEN);
  }

  lua_pushstring(L, host);
  lua_pushinteger(L, port);
  return 2;
}

LUA_API int tcpd_accept_close(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  if (event_mgr_base_current() && accept->buf)
  {
    bufferevent_free(accept->buf);
    accept->buf = NULL;
  }
  TCPD_ACCEPT_UNREF(accept)
  return 0;
}

LUA_API int lua_tcpd_accept_gc(lua_State *L) { return tcpd_accept_close(L); }

LUA_API int tcpd_accept_read_pause(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  if (accept->buf)
  {
    bufferevent_disable(accept->buf, EV_READ);
  }
  return 0;
}

LUA_API int tcpd_accept_read_resume(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  if (accept->buf)
  {
    bufferevent_enable(accept->buf, EV_READ);
  }
  return 0;
}

LUA_API int tcpd_conn_read_pause(lua_State *L)
{
  Conn *conn = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);
  if (conn->buf)
  {
    bufferevent_disable(conn->buf, EV_READ);
  }
  return 0;
}

LUA_API int tcpd_conn_read_resume(lua_State *L)
{
  Conn *conn = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);
  if (conn->buf)
  {
    bufferevent_enable(conn->buf, EV_READ);
  }
  return 0;
}

LUA_API int tcpd_conn_send(lua_State *L)
{
  Conn *conn = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);

  if (data && len > 0 && conn->buf)
  {
    if (conn->read_timeout > 0)
    {
      struct timeval tv1;
      d2tv(conn->read_timeout, &tv1);
      if (conn->write_timeout > 0)
      {
        struct timeval tv2;
        d2tv(conn->write_timeout, &tv2);
        bufferevent_set_timeouts(conn->buf, &tv1, &tv2);
      }
      else
      {
        bufferevent_set_timeouts(conn->buf, &tv1, NULL);
      }
    }
    else
    {
      if (conn->write_timeout > 0)
      {
        struct timeval tv2;
        d2tv(conn->write_timeout, &tv2);
        bufferevent_set_timeouts(conn->buf, NULL, &tv2);
      }
    }
    bufferevent_write(conn->buf, data, len);

    size_t total = evbuffer_get_length(bufferevent_get_output(conn->buf));
    lua_pushinteger(L, total);
  }
  else
  {
    lua_pushinteger(L, -1);
  }

  return 1;
}

LUA_API int tcpd_conn_reconnect(lua_State *L)
{
  Conn *conn = luaL_checkudata(L, 1, LUA_TCPD_CONNECTION_TYPE);
  luatcpd_reconnect(conn);
  return 0;
}

LUA_API int tcpd_accept_flush(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  int mode = luaL_optinteger(L, 2, BEV_NORMAL);
  lua_pushinteger(L, bufferevent_flush(accept->buf, EV_WRITE, mode));
  return 1;
}

LUA_API int tcpd_accept_send(lua_State *L)
{
  ACCEPT *accept = luaL_checkudata(L, 1, LUA_TCPD_ACCEPT_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);

  if (data && len > 0 && accept->buf)
  {
    bufferevent_write(accept->buf, data, len);
    size_t total = evbuffer_get_length(bufferevent_get_output(accept->buf));
    lua_pushinteger(L, total);
  }
  else
  {
    lua_pushinteger(L, -1);
  }

  return 1;
}

LUA_API int luaopen_fan_tcpd(lua_State *L)
{
#if FAN_HAS_OPENSSL
  conn_index = SSL_get_ex_new_index(0, "conn_index", NULL, NULL, NULL);
#endif

  luaL_newmetatable(L, LUA_TCPD_CONNECTION_TYPE);

  lua_pushcfunction(L, &tcpd_conn_send);
  lua_setfield(L, -2, "send");

  lua_pushcfunction(L, &tcpd_conn_read_pause);
  lua_setfield(L, -2, "pause_read");

  lua_pushcfunction(L, &tcpd_conn_read_resume);
  lua_setfield(L, -2, "resume_read");

  lua_pushcfunction(L, &tcpd_conn_close);
  lua_setfield(L, -2, "close");

  lua_pushcfunction(L, &tcpd_conn_reconnect);
  lua_setfield(L, -2, "reconnect");

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, &tcpd_conn_gc);
  lua_rawset(L, -3);
  lua_pop(L, 1);

  luaL_newmetatable(L, LUA_TCPD_ACCEPT_TYPE);

  lua_pushcfunction(L, &tcpd_accept_send);
  lua_setfield(L, -2, "send");

  lua_pushcfunction(L, &tcpd_accept_flush);
  lua_setfield(L, -2, "flush");

  lua_pushcfunction(L, &tcpd_accept_close);
  lua_setfield(L, -2, "close");

  lua_pushcfunction(L, &tcpd_accept_read_pause);
  lua_setfield(L, -2, "pause_read");

  lua_pushcfunction(L, &tcpd_accept_read_resume);
  lua_setfield(L, -2, "resume_read");

  lua_pushcfunction(L, &tcpd_accept_bind);
  lua_setfield(L, -2, "bind");

  lua_pushcfunction(L, &tcpd_accept_remote);
  lua_setfield(L, -2, "remoteinfo");

  lua_pushcfunction(L, &tcpd_accept_getsockname);
  lua_setfield(L, -2, "getsockname");

#ifdef __linux__
  lua_pushcfunction(L, &tcpd_accept_original_dst);
  lua_setfield(L, -2, "original_dst");
#endif

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pushstring(L, "__tostring");
  lua_pushcfunction(L, &lua_tcpd_accept_tostring);
  lua_rawset(L, -3);

  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, &lua_tcpd_accept_gc);
  lua_rawset(L, -3);

  lua_pop(L, 1);

  luaL_newmetatable(L, LUA_TCPD_SERVER_TYPE);
  lua_pushstring(L, "close");
  lua_pushcfunction(L, &lua_tcpd_server_close);
  lua_rawset(L, -3);

  lua_pushcfunction(L, &lua_tcpd_server_rebind);
  lua_setfield(L, -2, "rebind");

  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, &lua_tcpd_server_gc);
  lua_rawset(L, -3);

  lua_pushstring(L, "__tostring");
  lua_pushcfunction(L, &lua_tcpd_server_tostring);
  lua_rawset(L, -3);

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pop(L, 1);

  lua_newtable(L);
  luaL_register(L, "tcpd", tcpdlib);

  return 1;
}
