static void stmt_close_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  lua_State *L = ms->L;
  st_data *st = (st_data *)ms->data;

  my_bool ret = 0;
  int status = mysql_stmt_close_cont(&ret, st->my_stmt, ms->status);
  if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_close_cont, ms->extra);
  } else if (ret == 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);

    lua_pushboolean(L, 1);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(st);
  } else {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
    UNREF_CO(st);
  }
}

LUA_API int stmt_close_start(lua_State *L, st_data *st) {
  st->closed = 1;

  my_bool ret = 0;
  int status = mysql_stmt_close_start(&ret, st->my_stmt);
  if (status) {
    REF_CO(st);
    wait_for_status(L, st->conn_data, st, status, stmt_close_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);
    lua_pushboolean(L, 1);
    return 1;
  } else {
    return luamariadb_push_errno(L, st->conn_data);
  }

  return 0;
}


LUA_API int st_close(lua_State *L) {
  st_data *st = (st_data *)luaL_checkudata(L, 1, LUASQL_STATEMENT_MYSQL);
  if (st->closed) {
    lua_pushboolean(L, 0);
    return 1;
  }
  return stmt_close_start(L, st);
}
