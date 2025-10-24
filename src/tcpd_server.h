#ifndef TCPD_SERVER_H
#define TCPD_SERVER_H

#include "tcpd_common.h"

// Server structure definition
typedef struct tcpd_server {
    struct evconnlistener *listener;
    lua_State *mainthread;

    int onAcceptRef;
    int onSSLHostNameRef;

    char *host;
    int port;
    int ipv6;

    tcpd_config_t config;
    tcpd_ssl_context_t *ssl_ctx;
} tcpd_server_t;

// Server management functions
void tcpd_server_rebind(lua_State *L, tcpd_server_t *server);
LUA_API int tcpd_bind(lua_State *L);
LUA_API int lua_tcpd_server_rebind(lua_State *L);
LUA_API int lua_tcpd_server_localinfo(lua_State *L);

// Server listener callback
void tcpd_server_listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                            struct sockaddr *addr, int socklen, void *arg);

// Accept connection functions
LUA_API int tcpd_accept_bind(lua_State *L);
LUA_API int tcpd_accept_send(lua_State *L);
LUA_API int tcpd_accept_close(lua_State *L);
LUA_API int tcpd_accept_read_pause(lua_State *L);
LUA_API int tcpd_accept_read_resume(lua_State *L);
LUA_API int tcpd_accept_getsockname(lua_State *L);
LUA_API int tcpd_accept_getpeername(lua_State *L);

// Metatable registration
void tcpd_server_register_metatables(lua_State *L);

#endif // TCPD_SERVER_H