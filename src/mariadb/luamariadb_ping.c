#include "luamariadb_ping.h"

static int conn_ping_result(lua_State *L, DB_CTX *ctx)
{
  lua_pushboolean(L, true);
  return 1;
}

static void conn_ping_event(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_errno(conn);
  if (errorcode)
  {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, bag->ctx));
    UNREF_CO(bag->ctx);
  }
  else
  {
    int ret = 0;
    int status = mysql_ping_cont(&ret, conn, bag->status);

    if (status)
    {
      wait_for_status(L, bag->ctx, conn, status, conn_ping_event,
                      bag->extra);
    }
    else if (ret == 0)
    {
      int count = conn_ping_result(L, bag->ctx);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(bag->ctx);
    }
    else
    {
      FAN_RESUME(L, NULL, luamariadb_push_errno(L, bag->ctx));
      UNREF_CO(bag->ctx);
    }
  }
  event_free(bag->event);
  free(bag);
}

LUA_API int conn_ping_start(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);

  int ret = 0;
  int status = mysql_ping_start(&ret, &ctx->my_conn);
  if (status)
  {
    REF_CO(ctx);
    wait_for_status(L, ctx, &ctx->my_conn, status, conn_ping_event, 0);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    return conn_ping_result(L, ctx);
  }
  else
  {
    return luamariadb_push_errno(L, ctx);
  }
}
