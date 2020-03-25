static int real_query_result(lua_State *L, DB_CTX *ctx)
{
  MYSQL_RES *res = mysql_store_result(&ctx->my_conn);
  unsigned int num_cols = mysql_field_count(&ctx->my_conn);

  if (res)
  {
    return create_cursor(L, ctx, res, num_cols);
  }
  else
  {
    if (num_cols == 0)
    {
      lua_pushnumber(L, mysql_affected_rows(&ctx->my_conn));
      return 1;
    }
    else
    {
      return luamariadb_push_errno(L, ctx);
    }
  }
}

static void real_query_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  MYSQL *conn = (MYSQL *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_errno(conn);
  if (errorcode)
  {
    FAN_RESUME(L, NULL, luamariadb_push_errno(L, bag->ctx));
    UNREF_CO(bag->ctx);
  }
  else
  {
    int ret = 0;
    int status = mysql_real_query_cont(&ret, conn, bag->status);
    if (status)
    {
      wait_for_status(L, bag->ctx, conn, status, real_query_cont,
                      bag->extra);
    }
    else if (ret == 0)
    {
      int count = real_query_result(L, bag->ctx);
      FAN_RESUME(L, NULL, count);
      UNREF_CO(bag->ctx);
    }
    else
    {
      FAN_RESUME(L, NULL, luamariadb_push_errno(L, bag->ctx));
      UNREF_CO(bag->ctx);
    }
  }
  event_free(bag->event);
  free(bag);
}

LUA_API int real_query_start(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);
  size_t st_len;
  const char *statement = luaL_checklstring(L, 2, &st_len);

  int ret = 0;
  int status = mysql_real_query_start(&ret, &ctx->my_conn, statement, st_len);

  if (status)
  {
    REF_CO(ctx);
    wait_for_status(L, ctx, &ctx->my_conn, status, real_query_cont, 0);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    return real_query_result(L, ctx);
  }
  else
  {
    return luamariadb_push_errno(L, ctx);
  }
}
