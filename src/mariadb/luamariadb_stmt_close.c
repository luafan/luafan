#include "luamariadb_stmt_close.h"

static void stmt_close_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  lua_State *L = bag->L;
  STMT_CTX *st = (STMT_CTX *)bag->data;

  my_bool ret = 0;
  int status = mysql_stmt_close_cont(&ret, st->my_stmt, bag->status);
  if (status)
  {
    wait_for_status(L, st->ctx, st, status, stmt_close_cont, bag->extra);
  }
  else if (ret == 0)
  {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);

    lua_pushboolean(L, 1);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(st);
  }
  else
  {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, bag->ctx));
    UNREF_CO(st);
  }

  event_free(bag->event);
  free(bag);
}

LUA_API int stmt_close_start(lua_State *L, STMT_CTX *st)
{
  st->closed = 1;

  my_bool ret = 0;
  int status = mysql_stmt_close_start(&ret, st->my_stmt);

  if (status)
  {
    REF_CO(st);
    wait_for_status(L, st->ctx, st, status, stmt_close_cont, 0);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    return luamariadb_push_errno(L, st->ctx);
  }

  return 0;
}

LUA_API int st_close(lua_State *L)
{
  STMT_CTX *st = (STMT_CTX *)luaL_checkudata(L, 1, MARIADB_STATEMENT_METATABLE);
  if (st->closed)
  {
    lua_pushboolean(L, 0);
    return 1;
  }
  return stmt_close_start(L, st);
}
