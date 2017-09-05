static int real_query_result(lua_State *L, conn_data *conn_data) {
  MYSQL_RES *res = mysql_store_result(&conn_data->my_conn);
  unsigned int num_cols = mysql_field_count(&conn_data->my_conn);

  if (res) {
    return create_cursor(L, conn_data, res, num_cols);
  } else {
    if (num_cols == 0) {
      lua_pushnumber(L, mysql_affected_rows(&conn_data->my_conn));
      return 1;
    } else {
      return luamariadb_push_errno(L, conn_data);
    }
  }
}

static void real_query_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int errorcode = mysql_errno(conn);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
    UNREF_CO(ms->conn_data);
  } else {
    int ret = 0;
    int status = mysql_real_query_cont(&ret, conn, ms->status);
    if (status) {
      wait_for_status(L, ms->conn_data, conn, status, real_query_cont,
                      ms->extra);
    } else if (ret == 0) {
      int count = real_query_result(L, ms->conn_data);
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

LUA_API int real_query_start(lua_State *L) {
  conn_data *conn = getconnection(L);
  size_t st_len;
  const char *statement = luaL_checklstring(L, 2, &st_len);

  int ret = 0;
  int status = mysql_real_query_start(&ret, &conn->my_conn, statement, st_len);

  if (status) {
    REF_CO(conn);
    wait_for_status(L, conn, &conn->my_conn, status, real_query_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    return real_query_result(L, conn);
  } else {
    return luamariadb_push_errno(L, conn);
  }
}
