#ifndef LUAMARIADB_QUERY_H
#define LUAMARIADB_QUERY_H

#include "luamariadb_common.h"

// Query execution functions
LUA_API int real_query_start(lua_State *L);
static void real_query_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_QUERY_H