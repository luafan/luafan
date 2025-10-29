#ifndef LUAMARIADB_STMT_STORERESULT_H
#define LUAMARIADB_STMT_STORERESULT_H

#include "luamariadb_common.h"

// Statement store result functions
LUA_API int stmt_store_result_start(lua_State *L);
static void stmt_store_result_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_STMT_STORERESULT_H