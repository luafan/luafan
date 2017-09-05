static int conn_ping_result(lua_State *L, conn_data *conn_data) {
  lua_pushboolean(L, true);
  return 1;
}

static void conn_ping_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int errorcode = mysql_errno(conn);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
    UNREF_CO(ms->conn_data);
  } else {
    int ret = 0;
    int status = mysql_ping_cont(&ret, conn, ms->status);

    if (status) {
      wait_for_status(L, ms->conn_data, conn, status, conn_ping_event,
                      ms->extra);
    } else if (ret == 0) {
      int count = conn_ping_result(L, ms->conn_data);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(ms->conn_data);
    } else {
      FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
      UNREF_CO(ms->conn_data);
    }
  }
  event_free(ms->event);
  free(ms);
}

LUA_API int conn_ping_start(lua_State *L) {
  conn_data *conn = getconnection(L);

  int ret = 0;
  int status = mysql_ping_start(&ret, &conn->my_conn);
  if (status) {
    REF_CO(conn);
    wait_for_status(L, conn, &conn->my_conn, status, conn_ping_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    return conn_ping_result(L, conn);
  } else {
    return luamariadb_push_errno(L, conn);
  }
}
