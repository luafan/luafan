#include "luamariadb_connect.h"

static void real_connect_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int skip_unref = 0;

  MYSQL *ret = NULL;
  int status = mysql_real_connect_cont(&ret, conn, bag->status);
  if (status)
  {
    if (wait_for_status(L, bag->ctx, conn, status, real_connect_cont,
                        bag->extra) == 0)
    {
      skip_unref = 1;
    }
    else
    {
      int nresults = mariadb_push_wait_error(L);
      UNREF_CO(bag->ctx);
      FAN_RESUME(L, NULL, nresults);
      /* skip_unref stays 0: bag->extra is released below. */
    }
  }
  else if (ret == conn)
  {
    char value = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &value);

    lua_lock(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, bag->extra);
    UNREF_CO(bag->ctx);
    lua_unlock(L);
    FAN_RESUME(L, NULL, 1);
  }
  else
  {
    lua_lock(L);
    int nresults = luamariadb_push_errno(L, bag->ctx);
    UNREF_CO(bag->ctx);
    lua_unlock(L);
    FAN_RESUME(L, NULL, nresults);
  }

  if (!skip_unref)
  {
    lua_lock(L);
    luaL_unref(L, LUA_REGISTRYINDEX, bag->extra);
    lua_unlock(L);
  }
  event_free(bag->event);
  free(bag);
}

/*
** Connects to a data source.
**     param: one string for each connection parameter, said
**     datasource, username, password, host and port. An optional sixth
**     integer selects the event worker; omitted selects round-robin and -1
**     explicitly keeps the connection on the main event base.
*/
LUA_API int real_connect_start(lua_State *L)
{
  const char *sourcename = luaL_checkstring(L, 1);
  const char *username = luaL_optstring(L, 2, NULL);
  const char *password = luaL_optstring(L, 3, NULL);
  const char *host = luaL_optstring(L, 4, NULL);
  const int port = luaL_optinteger(L, 5, 0);
  const int requested_worker = lua_isnoneornil(L, 6)
                                   ? event_mgr_next_worker()
                                   : luaL_checkinteger(L, 6);
  if (requested_worker < -1 ||
      (requested_worker >= 0 && requested_worker >= event_mgr_worker_count())) {
    return luaL_error(L, "mariadb worker is unavailable");
  }
  MYSQL *ret;

  DB_CTX *ctx = (DB_CTX *)lua_newuserdata(L, sizeof(DB_CTX));
  memset(ctx, 0, sizeof(DB_CTX));
  ctx->coref = LUA_NOREF;
  ctx->worker_id = requested_worker;

  luasql_setmeta(L, MARIADB_CONNECTION_METATABLE);

  mysql_init(&ctx->my_conn);
  char value = 1;
  mysql_options(&ctx->my_conn, MYSQL_OPT_NONBLOCK, 0);
  mysql_options(&ctx->my_conn, MYSQL_OPT_RECONNECT, &value);

  /* fill in structure */
  ctx->closed = 0;

  int status = mysql_real_connect_start(&ret, &ctx->my_conn, host, username,
                                        password, sourcename, port, NULL, 0);
  if (status)
  {
    lua_lock(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_unlock(L);
    REF_CO(ctx);
    if (wait_for_status(L, ctx, &ctx->my_conn, status, real_connect_cont, ref) != 0)
    {
      lua_lock(L);
      luaL_unref(L, LUA_REGISTRYINDEX, ref);
      lua_unlock(L);
      UNREF_CO(ctx);
      return mariadb_push_wait_error(L);
    }
    return lua_yield(L, 0);
  }
  else if (ret == &ctx->my_conn)
  {
    mysql_options(&ctx->my_conn, MYSQL_OPT_RECONNECT, &value);
    return 1;
  }
  else
  {
    return luamariadb_push_errno(L, ctx);
  }
}
