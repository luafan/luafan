#ifndef LUAMARIADB_ROLLBACK_H
#define LUAMARIADB_ROLLBACK_H

#include "luamariadb_common.h"

// Transaction rollback functions
LUA_API int conn_rollback_start(lua_State *L);
static void conn_rollback_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_ROLLBACK_H