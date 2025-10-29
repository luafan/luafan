#ifndef LUAMARIADB_COMMIT_H
#define LUAMARIADB_COMMIT_H

#include "luamariadb_common.h"

// Transaction commit functions
LUA_API int conn_commit_start(lua_State *L);
static void conn_commit_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_COMMIT_H