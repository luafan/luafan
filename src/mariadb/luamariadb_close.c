LUA_API void conn_close_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int status = mysql_close_cont(conn, bag->status);
  if (status)
  {
    wait_for_status(L, bag->ctx, conn, status, conn_close_cont, bag->extra);
  }
  else
  {
    lua_pushboolean(L, 1);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(bag->ctx);
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
    ctx->closed = 1;

    int status = mysql_close_start(&ctx->my_conn);
    if (status)
    {
      REF_CO(ctx);
      wait_for_status(L, ctx, &ctx->my_conn, status, conn_close_cont, 0);
      return lua_yield(L, 0);
    }
    else
    {
      lua_pushboolean(L, 1);
      return 1;
    }
  }
}
