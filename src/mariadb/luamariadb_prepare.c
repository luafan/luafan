static void stmt_prepare_result(lua_State *L, st_data *st) {
  unsigned long param_count = mysql_stmt_param_count(st->my_stmt);

  size_t mysql_bind_size = param_count * sizeof(MYSQL_BIND);

  lua_rawgeti(L, LUA_REGISTRYINDEX, st->table);

  MYSQL_BIND *bind = lua_newuserdata(L, mysql_bind_size);
  memset(bind, 0, mysql_bind_size);
  st->nums = luaL_ref(L, -2);

  double *nums = lua_newuserdata(L, mysql_bind_size);
  memset(nums, 0, mysql_bind_size);
  st->bind = luaL_ref(L, -2);

  lua_pop(L, 1);

  st->rbind = LUA_NOREF;
  st->buffers = LUA_NOREF;
  st->bufferlens = LUA_NOREF;
  st->is_nulls = LUA_NOREF;
}

static void stmt_prepare_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  lua_State *L = ms->L;
  st_data *st = (st_data *)ms->data;

  int skip_unref = 0;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  } else {
    int ret = 0;
    int status = mysql_stmt_prepare_cont(&ret, st->my_stmt, ms->status);
    if (status) {
      wait_for_status(L, ms->conn_data, st, status, stmt_prepare_cont,
                      ms->extra);
      skip_unref = 1;
    } else if (ret == 0) {
      stmt_prepare_result(L, st);

      lua_rawgeti(L, LUA_REGISTRYINDEX, ms->extra);
      FAN_RESUME(L, NULL, 1);
      UNREF_CO(st);
    } else {
      FAN_RESUME(L, NULL, luamariadb_push_errno(L, st->conn_data));
      UNREF_CO(st);
    }
  }

  if (!skip_unref) {
    luaL_unref(L, LUA_REGISTRYINDEX, ms->extra);
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int stmt_prepare_start(lua_State *L) {
  conn_data *conn = getconnection(L);
  size_t st_len;
  const char *statement = luaL_checklstring(L, 2, &st_len);

  MYSQL_STMT *stmt = mysql_stmt_init(&conn->my_conn);

  st_data *st = (st_data *)lua_newuserdata(L, sizeof(st_data));
  memset(st, 0, sizeof(st_data));
  st->coref = LUA_NOREF;

  luasql_setmeta(L, LUASQL_STATEMENT_MYSQL);
  st->my_stmt = stmt;

  lua_newtable(L);
  st->conn_data = conn;

  st->table = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned long type = (unsigned long)CURSOR_TYPE_READ_ONLY;
  unsigned long prefetch_rows = 5;
  int rc = mysql_stmt_attr_set(stmt, STMT_ATTR_CURSOR_TYPE, (void *)&type);
  if (rc != 0) {
    printf("ATTR SET STMT_ATTR_CURSOR_TYPE %ld, rc=%d\n", type, rc);
  }
  /* ... check return value ... */
  rc = mysql_stmt_attr_set(stmt, STMT_ATTR_PREFETCH_ROWS,
                           (void *)&prefetch_rows);
  if (rc != 0) {
    printf("ATTR SET STMT_ATTR_PREFETCH_ROWS %ld, rc=%d\n", prefetch_rows, rc);
  }

  int ret = 0;
  int status = mysql_stmt_prepare_start(&ret, stmt, statement, st_len);
  if (status) {
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    REF_CO(st);
    wait_for_status(L, conn, st, status, stmt_prepare_cont, ref);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    stmt_prepare_result(L, st);
    // st_data on the top.
    return 1;
  } else {
    return luamariadb_push_errno(L, st->conn_data);
  }
}
