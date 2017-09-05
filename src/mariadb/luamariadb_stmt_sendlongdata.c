static int stmt_send_long_data_result(lua_State *L, st_data *st) {
  lua_pushboolean(L, true);
  return 1;
}

static void stmt_send_long_data_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  st_data *st = (st_data *)ms->data;
  lua_State *L = ms->L;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  } else {
    my_bool ret = 0;
    int status = mysql_stmt_send_long_data_cont(&ret, st->my_stmt, ms->status);
    if (status) {
      wait_for_status(L, st->conn_data, st, status, stmt_send_long_data_event,
                      ms->extra);
    } else if (ret == 0) {
      int count = stmt_send_long_data_result(L, st);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(st);
    } else {
      FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
      UNREF_CO(st);
    }
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int st_send_long_data(lua_State *L) {
  st_data *st = getstatement(L);

  size_t size = 0;
  int num = luaL_checkinteger(L, 2);
  const char *data = luaL_checklstring(L, 3, &size);

  my_bool ret = 0;
  int status =
      mysql_stmt_send_long_data_start(&ret, st->my_stmt, num, data, size);
  if (status) {
    REF_CO(st);
    wait_for_status(L, st->conn_data, st, status, stmt_send_long_data_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    return stmt_send_long_data_result(L, st);
  } else {
    return luamariadb_push_stmt_error(L, st);
  }
}
