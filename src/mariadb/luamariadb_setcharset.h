#ifndef LUAMARIADB_SETCHARSET_H
#define LUAMARIADB_SETCHARSET_H

#include "luamariadb_common.h"

// Character set functions
LUA_API int set_character_set_start(lua_State *L);
static void set_character_set_cont(int fd, short event, void *_userdata);

#endif // LUAMARIADB_SETCHARSET_H