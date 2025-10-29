#ifndef LUAMARIADB_PREPARE_H
#define LUAMARIADB_PREPARE_H

#include "luamariadb_common.h"

// Statement preparation functions
LUA_API int stmt_prepare_start(lua_State *L);
static void stmt_prepare_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_PREPARE_H