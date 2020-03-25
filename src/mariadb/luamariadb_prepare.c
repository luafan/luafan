static void stmt_prepare_result(lua_State *L, STMT_CTX *st)
{
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

static void stmt_prepare_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  lua_State *L = bag->L;
  STMT_CTX *st = (STMT_CTX *)bag->data;

  int skip_unref = 0;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode)
  {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  }
  else
  {
    int ret = 0;
    int status = mysql_stmt_prepare_cont(&ret, st->my_stmt, bag->status);
    if (status)
    {
      wait_for_status(L, bag->ctx, st, status, stmt_prepare_cont,
                      bag->extra);
      skip_unref = 1;
    }
    else if (ret == 0)
    {
      stmt_prepare_result(L, st);

      lua_rawgeti(L, LUA_REGISTRYINDEX, bag->extra);
      FAN_RESUME(L, NULL, 1);
      UNREF_CO(st);
    }
    else
    {
      FAN_RESUME(L, NULL, luamariadb_push_errno(L, st->ctx));
      UNREF_CO(st);
    }
  }

  if (!skip_unref)
  {
    luaL_unref(L, LUA_REGISTRYINDEX, bag->extra);
  }

  event_free(bag->event);
  free(bag);
}

LUA_API int stmt_prepare_start(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);
  size_t st_len;
  const char *statement = luaL_checklstring(L, 2, &st_len);

  MYSQL_STMT *stmt = mysql_stmt_init(&ctx->my_conn);

  STMT_CTX *st = (STMT_CTX *)lua_newuserdata(L, sizeof(STMT_CTX));
  memset(st, 0, sizeof(STMT_CTX));
  st->coref = LUA_NOREF;

  luasql_setmeta(L, MARIADB_STATEMENT_METATABLE);
  st->my_stmt = stmt;

  lua_newtable(L);
  st->ctx = ctx;

  st->table = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned long type = (unsigned long)CURSOR_TYPE_READ_ONLY;
  unsigned long prefetch_rows = 5;
  int rc = mysql_stmt_attr_set(stmt, STMT_ATTR_CURSOR_TYPE, (void *)&type);
  if (rc != 0)
  {
    printf("ATTR SET STMT_ATTR_CURSOR_TYPE %ld, rc=%d\n", type, rc);
  }
  /* ... check return value ... */
  rc = mysql_stmt_attr_set(stmt, STMT_ATTR_PREFETCH_ROWS,
                           (void *)&prefetch_rows);
  if (rc != 0)
  {
    printf("ATTR SET STMT_ATTR_PREFETCH_ROWS %ld, rc=%d\n", prefetch_rows, rc);
  }

  int ret = 0;
  int status = mysql_stmt_prepare_start(&ret, stmt, statement, st_len);
  if (status)
  {
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    REF_CO(st);
    wait_for_status(L, ctx, st, status, stmt_prepare_cont, ref);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    stmt_prepare_result(L, st);
    // STMT_CTX on the top.
    return 1;
  }
  else
  {
    return luamariadb_push_errno(L, st->ctx);
  }
}
