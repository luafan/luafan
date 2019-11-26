static st_data *getstatement(lua_State *L) {
  st_data *st = (st_data *)luaL_checkudata(L, 1, LUASQL_STATEMENT_MYSQL);
  luaL_argcheck(L, st != NULL, 1, "statement expected");
  luaL_argcheck(L, !st->closed, 1, "statement is closed");
  return st;
}

static void *get_or_create_ud(lua_State *L, int tableidx, int *ref,
                              size_t size) {
  void *ud = NULL;
  if (*ref == LUA_NOREF || *ref == 0) {
    ud = lua_newuserdata(L, size);
    *ref = luaL_ref(L, tableidx);
    memset(ud, 0, size);
  } else {
    lua_rawgeti(L, tableidx, *ref);
    ud = lua_touserdata(L, -1);
    lua_pop(L, 1);
  }

  return ud;
}

static int luamariadb_push_stmt_error(lua_State *L, st_data *st) {
  lua_pushnil(L);
  lua_pushfstring(L, "stmt_error: %d: %s",
	  mysql_stmt_errno(st->my_stmt), mysql_stmt_error(st->my_stmt));

  return 2;
}

LUA_API int _st_bind(lua_State *L, int cache_bind) {
  st_data *st = getstatement(L);

  unsigned long param_count = mysql_stmt_param_count(st->my_stmt);
  if (lua_gettop(L) != param_count + 1) {
    luaL_error(L, "parameters number does not match, excepted %d, got %d",
               param_count, lua_gettop(L) - 1);
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, st->table);
  int tableidx = lua_gettop(L);

  lua_rawgeti(L, tableidx, st->bind);
  MYSQL_BIND *bind = lua_touserdata(L, -1);

  lua_rawgeti(L, tableidx, st->nums);
  double *nums = lua_touserdata(L, -1);

  lua_pop(L, 2);

  int i = 0;
  for (; i < param_count; i++) {
    int idx = i + 2;
    switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
      size_t sz = 0;
      const char *str = lua_tolstring(L, idx, &sz);
      lua_pushvalue(L, idx);
      lua_rawseti(L, tableidx, i + 1000);
      MYSQL_SET_VARSTRING(&bind[i], (void *)str, sz);
    } break;
    case LUA_TNUMBER: {
      nums[i] = lua_tonumber(L, idx);
      MYSQL_SET_DOUBLE(&bind[i], &nums[i]);
    } break;
    case LUA_TBOOLEAN: {
      nums[i] = lua_toboolean(L, idx);
      MYSQL_SET_TINYINT(&bind[i], &nums[i]);
    } break;
    case LUA_TLIGHTUSERDATA: {
      void *data = lua_touserdata(L, idx);
      if (data == &LONG_DATA) {
        memset(&bind[i], 0, sizeof(bind[i]));
        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].length = (unsigned long *)&nums[i];
        bind[i].is_null = 0;
      }
    } break;
    case LUA_TNIL: {
      bind[i].buffer_type = MYSQL_TYPE_NULL;
    } break;
    default:
      printf("unknown type %d, idx=%d\n", lua_type(L, idx), idx);
      break;
    }
  }

  lua_pop(L, 1);

  if (!cache_bind || !st->has_bind_param) {
    mysql_stmt_bind_param(st->my_stmt, bind);
    st->has_bind_param = 1;
  }

  lua_pushvalue(L, 1);

  return 1;
}

#include "luamariadb_stmt_close.c"
#include "luamariadb_stmt_fetch.c"
#include "luamariadb_stmt_storeresult.c"
#include "luamariadb_stmt_sendlongdata.c"
#include "luamariadb_stmt_execute.c"

LUA_API int st_gc(lua_State *L) {
  st_data *st = (st_data *)luaL_checkudata(L, 1, LUASQL_STATEMENT_MYSQL);
  if (st != NULL && !st->closed) {
    return stmt_close_start(L, st);
  }

  return 0;
}

LUA_API int st_pairs(lua_State *L) {
  getstatement(L);
  lua_pushcfunction(L, stmt_fetch_start);
  lua_pushvalue(L, 1);

  return 2;
}

LUA_API int st_bind_param(lua_State *L) { return _st_bind(L, 0); }

LUA_API int st_bind(lua_State *L) { return _st_bind(L, 1); }
