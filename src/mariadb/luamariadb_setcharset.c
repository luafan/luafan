#include "luamariadb_setcharset.h"

static void set_character_set_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int ret = 0;
  int status = mysql_set_character_set_cont(&ret, conn, bag->status);
  if (status)
  {
    if (wait_for_status(L, bag->ctx, conn, status, set_character_set_cont,
                        bag->extra) != 0)
    {
      int nresults = mariadb_push_wait_error(L);
      RESUME_AND_UNREF_CO(bag->ctx, nresults);
    }
  }
  else if (ret == 0)
  {
    lua_pushboolean(L, 1);
    RESUME_AND_UNREF_CO(bag->ctx, 1);
  }
  else
  {
    int nresults = luamariadb_push_errno(L, bag->ctx);
    RESUME_AND_UNREF_CO(bag->ctx, nresults);
  }

  event_free(bag->event);
  free(bag);
}

LUA_API int set_character_set_start(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);
  const char *charset = luaL_checkstring(L, 2);

  int ret = 0;
  int status = mysql_set_character_set_start(&ret, &ctx->my_conn, charset);
  if (status)
  {
    REF_CO(ctx);
    if (wait_for_status(L, ctx, &ctx->my_conn, status, set_character_set_cont, 0) != 0)
    {
      UNREF_CO(ctx);
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
    return luamariadb_push_errno(L, ctx);
  }
}
