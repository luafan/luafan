static void stmt_store_result_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  STMT_CTX *st = (STMT_CTX *)bag->data;
  lua_State *L = bag->L;

  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode)
  {
    FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
    UNREF_CO(st);
  }
  else
  {
    int ret = 0;
    int status = mysql_stmt_store_result_cont(&ret, st->my_stmt, bag->status);

    if (status)
    {
      wait_for_status(L, st->ctx, st, status, stmt_store_result_cont,
                      bag->extra);
    }
    else if (ret == 0)
    {
      lua_pushboolean(L, 1);
      FAN_RESUME(L, NULL, 1);
      UNREF_CO(st);
    }
    else
    {
      FAN_RESUME(L, NULL, luamariadb_push_stmt_error(L, st));
      UNREF_CO(st);
    }
  }
  event_free(bag->event);
  free(bag);
}

static int stmt_store_result_start(lua_State *L)
{
  STMT_CTX *st = getstatement(L);

  int ret = 0;
  int status = mysql_stmt_store_result_start(&ret, st->my_stmt);

  if (status)
  {
    REF_CO(st);
    wait_for_status(L, st->ctx, st, status, stmt_store_result_cont, 0);
    return lua_yield(L, 0);
  }
  else if (ret == 0)
  {
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    return luamariadb_push_stmt_error(L, st);
  }
}
