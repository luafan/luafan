#ifndef LUAMARIADB_STMT_FETCH_H
#define LUAMARIADB_STMT_FETCH_H

#include "luamariadb_common.h"

// Statement fetch functions
LUA_API int stmt_fetch_start(lua_State *L);
static void stmt_fetch_cont(int fd, short event, void *_userdata);
static int stmt_fetch_result(lua_State *L, STMT_CTX *st);

#endif // LUAMARIADB_STMT_FETCH_H