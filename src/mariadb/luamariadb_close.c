LUA_API void conn_close_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int status = mysql_close_cont(conn, ms->status);
  if (status) {
    wait_for_status(L, ms->conn_data, conn, status, conn_close_cont, ms->extra);
  } else {
    lua_pushboolean(L, 1);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(ms->conn_data);
  }

  event_free(ms->event);
  free(ms);
}

/*
** Close a Connection object.
*/
LUA_API int conn_close_start(lua_State *L) {
  conn_data *conn = (conn_data *)luaL_checkudata(L, 1, LUASQL_CONNECTION_MYSQL);
  luaL_argcheck(L, conn != NULL, 1, LUASQL_PREFIX "connection expected");
  if (conn->closed) {
    lua_pushboolean(L, 0);
    return 1;
  } else {
    conn->closed = 1;

    int status = mysql_close_start(&conn->my_conn);
    if (status) {
      REF_CO(conn);
      wait_for_status(L, conn, &conn->my_conn, status, conn_close_cont, 0);
      return lua_yield(L, 0);
    } else {
      lua_pushboolean(L, 1);
      return 1;
    }
  }
}
