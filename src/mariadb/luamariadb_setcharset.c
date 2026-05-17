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
    wait_for_status(L, bag->ctx, conn, status, set_character_set_cont,
                    bag->extra);
  }
  else if (ret == 0)
  {
    lua_pushboolean(L, 1);
    UNREF_CO(bag->ctx);
    FAN_RESUME(L, NULL, 1);
  }
  else
  {
    int nresults = luamariadb_push_errno(L, bag->ctx);
    UNREF_CO(bag->ctx);
    FAN_RESUME(L, NULL, nresults);
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
    wait_for_status(L, ctx, &ctx->my_conn, status, set_character_set_cont, 0);
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
