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
    wait_for_status(L, bag->ctx, conn, status, real_connect_cont,
                    bag->extra);
    skip_unref = 1;
  }
  else if (ret == conn)
  {
    char value = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &value);

    lua_rawgeti(L, LUA_REGISTRYINDEX, bag->extra);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(bag->ctx);
  }
  else
  {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, bag->ctx));
    UNREF_CO(bag->ctx);
  }

  if (!skip_unref)
  {
    luaL_unref(L, LUA_REGISTRYINDEX, bag->extra);
  }
  event_free(bag->event);
  free(bag);
}

/*
** Connects to a data source.
**     param: one string for each connection parameter, said
**     datasource, username, password, host and port.
*/
LUA_API int real_connect_start(lua_State *L)
{
  const char *sourcename = luaL_checkstring(L, 1);
  const char *username = luaL_optstring(L, 2, NULL);
  const char *password = luaL_optstring(L, 3, NULL);
  const char *host = luaL_optstring(L, 4, NULL);
  const int port = luaL_optinteger(L, 5, 0);
  MYSQL *ret;

  DB_CTX *ctx = (DB_CTX *)lua_newuserdata(L, sizeof(DB_CTX));
  memset(ctx, 0, sizeof(DB_CTX));
  ctx->coref = LUA_NOREF;

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
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    REF_CO(ctx);
    wait_for_status(L, ctx, &ctx->my_conn, status, real_connect_cont, ref);
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
