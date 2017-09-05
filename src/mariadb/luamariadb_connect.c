static void real_connect_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int skip_unref = 0;

  MYSQL *ret = NULL;
  int status = mysql_real_connect_cont(&ret, conn, ms->status);
  if (status) {
    wait_for_status(L, ms->conn_data, conn, status, real_connect_cont,
                    ms->extra);
    skip_unref = 1;
  } else if (ret == conn) {
    char value = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &value);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ms->extra);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(ms->conn_data);
  } else {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
    UNREF_CO(ms->conn_data);
  }

  if (!skip_unref) {
    luaL_unref(L, LUA_REGISTRYINDEX, ms->extra);
  }
  event_free(ms->event);
  free(ms);
}

/*
** Connects to a data source.
**     param: one string for each connection parameter, said
**     datasource, username, password, host and port.
*/
LUA_API int real_connect_start(lua_State *L) {
  const char *sourcename = luaL_checkstring(L, 1);
  const char *username = luaL_optstring(L, 2, NULL);
  const char *password = luaL_optstring(L, 3, NULL);
  const char *host = luaL_optstring(L, 4, NULL);
  const int port = luaL_optinteger(L, 5, 0);
  MYSQL *ret;

  conn_data *cdata = (conn_data *)lua_newuserdata(L, sizeof(conn_data));
  memset(cdata, 0, sizeof(conn_data));
  cdata->coref = LUA_NOREF;

  luasql_setmeta(L, LUASQL_CONNECTION_MYSQL);

  mysql_init(&cdata->my_conn);
  char value = 1;
  mysql_options(&cdata->my_conn, MYSQL_OPT_NONBLOCK, 0);
  mysql_options(&cdata->my_conn, MYSQL_OPT_RECONNECT, &value);

  /* fill in structure */
  cdata->closed = 0;

  int status = mysql_real_connect_start(&ret, &cdata->my_conn, host, username,
                                        password, sourcename, port, NULL, 0);
  if (status) {
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    REF_CO(cdata);
    wait_for_status(L, cdata, &cdata->my_conn, status, real_connect_cont, ref);
    return lua_yield(L, 0);
  } else if (ret == &cdata->my_conn) {
    mysql_options(&cdata->my_conn, MYSQL_OPT_RECONNECT, &value);
    return 1;
  } else {
    return luamariadb_push_errno(L, cdata);
  }
}
