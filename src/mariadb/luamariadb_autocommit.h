#ifndef LUAMARIADB_AUTOCOMMIT_H
#define LUAMARIADB_AUTOCOMMIT_H

#include "luamariadb_common.h"

// Autocommit functions
LUA_API int conn_autocommit_start(lua_State *L);
static void conn_autocommit_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_AUTOCOMMIT_H