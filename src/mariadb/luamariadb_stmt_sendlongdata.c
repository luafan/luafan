#include "luamariadb_stmt_sendlongdata.h"

static int stmt_send_long_data_result(lua_State *L, STMT_CTX *st)
{
  lua_pushboolean(L, true);
  return 1;
}

static void stmt_send_long_data_event(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  STMT_CTX *st = (STMT_CTX *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode)
  {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  }
  else
  {
    my_bool ret = 0;
    int status = mysql_stmt_send_long_data_cont(&ret, st->my_stmt, bag->status);
    if (status)
    {
      wait_for_status(L, st->ctx, st, status, stmt_send_long_data_event,
                      bag->extra);
    }
    else if (ret == 0)
    {
      int count = stmt_send_long_data_result(L, st);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(st);
    }
    else
    {
      FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
      UNREF_CO(st);
    }
  }

  event_free(bag->event);
  free(bag);
}

LUA_API int st_send_long_data(lua_State *L)
{
  STMT_CTX *st = getstatement(L);

  size_t size = 0;
  int num = luaL_checkinteger(L, 2);
  const char *data = luaL_checklstring(L, 3, &size);

  my_bool ret = 0;
  int status =
      mysql_stmt_send_long_data_start(&ret, st->my_stmt, num, data, size);
  if (status)
  {
    REF_CO(st);
    wait_for_status(L, st->ctx, st, status, stmt_send_long_data_event, 0);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    return stmt_send_long_data_result(L, st);
  }
  else
  {
    return luamariadb_push_stmt_error(L, st);
  }
}
