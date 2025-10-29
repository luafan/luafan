#ifndef LUAMARIADB_STMT_EXECUTE_H
#define LUAMARIADB_STMT_EXECUTE_H

#include "luamariadb_common.h"

// Statement execute functions
LUA_API int stmt_execute_start(lua_State *L);
static void stmt_execute_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_STMT_EXECUTE_H