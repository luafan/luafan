static void conn_commit_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int errorcode = mysql_errno(conn);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
    UNREF_CO(ms->conn_data);
  } else {
    my_bool ret = 0;
    int status = mysql_commit_cont(&ret, conn, ms->status);

    if (status) {
      wait_for_status(L, ms->conn_data, conn, status, conn_commit_event,
                      ms->extra);
    } else if (ret == 0) {
      lua_pushboolean(L, true);
      FAN_RESUME(L, NULL, 1);
      UNREF_CO(ms->conn_data);
    } else {
      FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
      UNREF_CO(ms->conn_data);
    }
  }
  event_free(ms->event);
  free(ms);
}

LUA_API int conn_commit_start(lua_State *L) {
  conn_data *conn = getconnection(L);

  my_bool ret = 0;
  int status = mysql_commit_start(&ret, &conn->my_conn);
  if (status) {
    REF_CO(conn);
    wait_for_status(L, conn, &conn->my_conn, status, conn_commit_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    return 1;
  } else {
    return luamariadb_push_errno(L, conn);
  }
}
