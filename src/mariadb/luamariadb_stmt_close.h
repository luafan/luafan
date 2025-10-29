#ifndef LUAMARIADB_STMT_CLOSE_H
#define LUAMARIADB_STMT_CLOSE_H

#include "luamariadb_common.h"

// Statement close functions
LUA_API int stmt_close_start(lua_State *L, STMT_CTX *st);
LUA_API int st_close(lua_State *L);
static void stmt_close_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_STMT_CLOSE_H