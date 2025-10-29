#ifndef LUAMARIADB_CURSOR_H
#define LUAMARIADB_CURSOR_H

#include "luamariadb_common.h"

// Cursor utility functions (some declared in common.h)
// CURSOR_CTX *getcursor(lua_State *L);
// int create_cursor(lua_State *L, DB_CTX *ctx, MYSQL_RES *result, int cols);
static void pushvalue(lua_State *L, void *row, long int len, enum enum_field_types field_type);
static char *getcolumntype(enum enum_field_types type);
static void create_colinfo(lua_State *L, CURSOR_CTX *cur);
static void _pushtable(lua_State *L, CURSOR_CTX *cur, size_t off);

// Cursor result handling functions
static void free_result_cont(int fd, short event, void *_userdata);
static int free_result_start(lua_State *L, CURSOR_CTX *cur);
static int fetch_row_result(lua_State *L, CURSOR_CTX *cur, MYSQL_ROW row);
static void fetch_row_cont(int fd, short event, void *_userdata);

// Cursor API functions
LUA_API int fetch_row_start(lua_State *L);
LUA_API int cur_gc(lua_State *L);
LUA_API int cur_close(lua_State *L);
LUA_API int cur_getcolnames(lua_State *L);
LUA_API int cur_getcoltypes(lua_State *L);
LUA_API int cur_numrows(lua_State *L);

// Macro for table pushing
#define pushtable(L, c, m) (_pushtable(L, c, offsetof(CURSOR_CTX, m)))

#endif // LUAMARIADB_CURSOR_H