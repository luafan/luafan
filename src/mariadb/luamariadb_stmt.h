#ifndef LUAMARIADB_STMT_H
#define LUAMARIADB_STMT_H

#include "luamariadb_common.h"

// Statement utility functions (declared in common.h)
// STMT_CTX *getstatement(lua_State *L);
// void *get_or_create_ud(lua_State *L, int tableidx, int *ref, size_t size);
// int luamariadb_push_stmt_error(lua_State *L, STMT_CTX *st);

// Statement binding functions
LUA_API int _st_bind(lua_State *L, int cache_bind);
LUA_API int st_bind_param(lua_State *L);
LUA_API int st_bind(lua_State *L);

// Statement management functions
LUA_API int st_gc(lua_State *L);
LUA_API int st_pairs(lua_State *L);

// Include sub-module headers
#include "luamariadb_stmt_close.h"
#include "luamariadb_stmt_fetch.h"
#include "luamariadb_stmt_storeresult.h"
#include "luamariadb_stmt_sendlongdata.h"
#include "luamariadb_stmt_execute.h"

#endif // LUAMARIADB_STMT_H