static void stmt_store_result_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  st_data *st = (st_data *)ms->data;
  lua_State *L = ms->L;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  } else {
    int ret = 0;
    int status = mysql_stmt_store_result_cont(&ret, st->my_stmt, ms->status);

    if (status) {
      wait_for_status(L, st->conn_data, st, status, stmt_store_result_cont,
                      ms->extra);
    } else if (ret == 0) {
      lua_pushboolean(L, 1);
      FAN_RESUME(L, NULL, 1);
      UNREF_CO(st);
    } else {
      FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
      UNREF_CO(st);
    }
  }
  event_free(ms->event);
  free(ms);
}

static int stmt_store_result_start(lua_State *L, st_data *st) {
  int ret = 0;
  int status = mysql_stmt_store_result_start(&ret, st->my_stmt);

  if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_store_result_cont, 0);
    return CONTINUE_YIELD;
  } else if (ret == 0) {
    lua_pushboolean(L, 1);
    return 1;
  } else {
    return luamariadb_push_stmt_error(L, st);
  }
}
