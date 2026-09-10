#include "luamariadb_stmt_storeresult.h"

static void stmt_store_result_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  STMT_CTX *st = (STMT_CTX *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode)
  {
    int nresults = luamariadb_push_stmt_error(L, st);
    UNREF_CO(st);
    FAN_RESUME(L, NULL, nresults);
  }
  else
  {
    int ret = 0;
    int status = mysql_stmt_store_result_cont(&ret, st->my_stmt, bag->status);

    if (status)
    {
      if (wait_for_status(L, st->ctx, st, status, stmt_store_result_cont,
                          bag->extra) != 0)
      {
        int nresults = mariadb_push_wait_error(L);
        UNREF_CO(st);
        FAN_RESUME(L, NULL, nresults);
      }
    }
    else if (ret == 0)
    {
      lua_pushboolean(L, 1);
      UNREF_CO(st);
      FAN_RESUME(L, NULL, 1);
    }
    else
    {
      int nresults = luamariadb_push_stmt_error(L, st);
      UNREF_CO(st);
      FAN_RESUME(L, NULL, nresults);
    }
  }
  event_free(bag->event);
  free(bag);
}

LUA_API int stmt_store_result_start(lua_State *L)
{
  STMT_CTX *st = getstatement(L);

  int ret = 0;
  int status = mysql_stmt_store_result_start(&ret, st->my_stmt);

  if (status)
  {
    REF_CO(st);
    if (wait_for_status(L, st->ctx, st, status, stmt_store_result_cont, 0) != 0)
    {
      UNREF_CO(st);
      return mariadb_push_wait_error(L);
    }
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    return luamariadb_push_stmt_error(L, st);
  }
}
