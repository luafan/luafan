#ifndef LUAMARIADB_CLOSE_H
#define LUAMARIADB_CLOSE_H

#include "luamariadb_common.h"

// Connection close functions
LUA_API int conn_close_start(lua_State *L);
LUA_API void conn_close_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_CLOSE_H