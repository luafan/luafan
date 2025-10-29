#ifndef LUAMARIADB_PING_H
#define LUAMARIADB_PING_H

#include "luamariadb_common.h"

// Connection ping functions
LUA_API int conn_ping_start(lua_State *L);
static void conn_ping_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_PING_H