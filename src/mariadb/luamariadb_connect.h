#ifndef LUAMARIADB_CONNECT_H
#define LUAMARIADB_CONNECT_H

#include "luamariadb_common.h"

// Connection functions
LUA_API int real_connect_start(lua_State *L);
static void real_connect_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_CONNECT_H