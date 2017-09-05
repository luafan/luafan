static void set_character_set_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int status = mysql_set_character_set_cont(&ret, conn, ms->status);
  if (status) {
    wait_for_status(L, ms->conn_data, conn, status, set_character_set_cont,
                    ms->extra);
  } else if (ret == 0) {
    lua_pushboolean(L, 1);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(ms->conn_data);
  } else {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, ms->conn_data));
    UNREF_CO(ms->conn_data);
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int set_character_set_start(lua_State *L) {
  conn_data *conn = getconnection(L);
  const char *charset = luaL_checkstring(L, 2);

  int ret = 0;
  int status = mysql_set_character_set_start(&ret, &conn->my_conn, charset);
  if (status) {
    REF_CO(conn);
    wait_for_status(L, conn, &conn->my_conn, status, set_character_set_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    lua_pushboolean(L, 1);
    return 1;
  } else {
    return luamariadb_push_errno(L, conn);
  }
}
