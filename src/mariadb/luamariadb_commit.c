#include "luamariadb_commit.h"

static void conn_commit_event(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_errno(conn);
  if (errorcode)
  {
    int nresults = luamariadb_push_errno(L, bag->ctx);
    RESUME_AND_UNREF_CO(bag->ctx, nresults);
  }
  else
  {
    my_bool ret = 0;
    int status = mysql_commit_cont(&ret, conn, bag->status);

    if (status)
    {
      if (wait_for_status(L, bag->ctx, conn, status, conn_commit_event,
                          bag->extra) != 0)
      {
        int nresults = mariadb_push_wait_error(L);
        RESUME_AND_UNREF_CO(bag->ctx, nresults);
      }
    }
    else if (ret == 0)
    {
      lua_pushboolean(L, true);
      RESUME_AND_UNREF_CO(bag->ctx, 1);
    }
    else
    {
      int nresults = luamariadb_push_errno(L, bag->ctx);
      RESUME_AND_UNREF_CO(bag->ctx, nresults);
    }
  }
  event_free(bag->event);
  free(bag);
}

LUA_API int conn_commit_start(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);

  my_bool ret = 0;
  int status = mysql_commit_start(&ret, &ctx->my_conn);
  if (status)
  {
    REF_CO(ctx);
    if (wait_for_status(L, ctx, &ctx->my_conn, status, conn_commit_event, 0) != 0)
    {
      UNREF_CO(ctx);
      return mariadb_push_wait_error(L);
    }
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    lua_pushboolean(L, true);
    return 1;
  }
  else
  {
    return luamariadb_push_errno(L, ctx);
  }
}
