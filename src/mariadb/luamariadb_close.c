#include "luamariadb_close.h"

LUA_API void conn_close_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int status = mysql_close_cont(conn, bag->status);
  if (status)
  {
    if (wait_for_status(L, bag->ctx, conn, status, conn_close_cont, bag->extra) != 0)
    {
      int nresults = mariadb_push_wait_error(L);
      RESUME_AND_UNREF_CO(bag->ctx, nresults);
    }
  }
  else
  {
    lua_pushboolean(L, 1);
    RESUME_AND_UNREF_CO(bag->ctx, 1);
  }

  event_free(bag->event);
  free(bag);
}

/*
** Close a Connection object.
*/
LUA_API int conn_close_start(lua_State *L)
{
  DB_CTX *ctx = (DB_CTX *)luaL_checkudata(L, 1, MARIADB_CONNECTION_METATABLE);
  luaL_argcheck(L, ctx != NULL, 1, LUASQL_PREFIX "connection expected");
  if (ctx->closed)
  {
    lua_pushboolean(L, 0);
    return 1;
  }
  else
  {
    /* Marked closed under the pending context's mutex first, so wait_for_status()
     * refuses
     * to arm anything new on this connection while the pending list drains (and
     * can no longer be armed after the drain, see mariadb_close_begin). Abort
     * what is already parked *before* mysql_close_start() releases the
     * connection internals: otherwise libevent would still deliver those events
     * and their continuations would call mysql_*_cont() on a closed MYSQL
     * (use-after-free, see mariadb_cancel_pending_waits). */
    mariadb_close_begin(ctx);
    mariadb_cancel_pending_waits(ctx);

    int status = mysql_close_start(&ctx->my_conn);
    if (status)
    {
      REF_CO(ctx);
      if (wait_for_status(L, ctx, &ctx->my_conn, status, conn_close_cont, 0) != 0)
      {
        UNREF_CO(ctx);
        return mariadb_push_wait_error(L);
      }
      return lua_yield(L, 0);
    }
    else
    {
      lua_pushboolean(L, 1);
      return 1;
    }
  }
}
