#ifndef LUAMARIADB_STMT_SENDLONGDATA_H
#define LUAMARIADB_STMT_SENDLONGDATA_H

#include "luamariadb_common.h"

// Statement send long data functions
LUA_API int st_send_long_data(lua_State *L);
static void stmt_send_long_data_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_STMT_SENDLONGDATA_H