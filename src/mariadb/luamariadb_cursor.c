static cur_data *getcursor(lua_State *L) {
  cur_data *cur = (cur_data *)luaL_checkudata(L, 1, LUASQL_CURSOR_MYSQL);
  luaL_argcheck(L, cur != NULL, 1, "cursor expected");
  luaL_argcheck(L, !cur->closed, 1, "cursor is closed");
  return cur;
}

static void pushvalue(lua_State *L, void *row, long int len) {
  if (row == NULL)
    lua_pushnil(L);
  else
    lua_pushlstring(L, row, len);
}

static char *getcolumntype(enum enum_field_types type) {
  switch (type) {
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING:
    return "string";
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_TINY:
    return "number";
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
    return "binary";
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
    return "date";
  case MYSQL_TYPE_DATETIME:
    return "datetime";
  case MYSQL_TYPE_TIME:
    return "time";
  case MYSQL_TYPE_TIMESTAMP:
    return "timestamp";
  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
    return "set";
  case MYSQL_TYPE_NULL:
    return "null";
  default:
    return "undefined";
  }
}

/*
** Creates the lists of fields names and fields types.
*/
static void create_colinfo(lua_State *L, cur_data *cur) {
  MYSQL_FIELD *fields;
  char typename[50];
  int i;
  fields = mysql_fetch_fields(cur->my_res);
  lua_newtable(L); /* names */
  lua_newtable(L); /* types */
  for (i = 1; i <= cur->numcols; i++) {
    lua_pushstring(L, fields[i - 1].name);
    lua_rawseti(L, -3, i);
    sprintf(typename, "%.20s(%ld)", getcolumntype(fields[i - 1].type),
            fields[i - 1].length);
    lua_pushstring(L, typename);
    lua_rawseti(L, -2, i);
  }
  /* Stores the references in the cursor structure */
  cur->coltypes = luaL_ref(L, LUA_REGISTRYINDEX);
  cur->colnames = luaL_ref(L, LUA_REGISTRYINDEX);
}

static void free_result_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  cur_data *cur = (cur_data *)ms->data;
  lua_State *L = ms->L;

  int status = mysql_free_result_cont(cur->my_res, ms->status);
  if (status) {
    wait_for_status(L, ms->conn_data, cur->my_res, status, free_result_cont,
                    ms->extra);
  } else {
    lua_pushboolean(L, true);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(cur);
  }

  event_free(ms->event);
  free(ms);
}

static int free_result_start(lua_State *L, cur_data *cur) {
  cur->closed = 1;

  luaL_unref(L, LUA_REGISTRYINDEX, cur->colnames);
  cur->colnames = LUA_NOREF;

  luaL_unref(L, LUA_REGISTRYINDEX, cur->coltypes);
  cur->coltypes = LUA_NOREF;

  conn_data *conn = cur->conn_data;
  cur->conn_data = NULL;

  int status = mysql_free_result_start(cur->my_res);
  if (status) {
    wait_for_status(L, conn, cur, status, free_result_cont, 0);
    return CONTINUE_YIELD;
  } else {
    return 0;
  }
}

static int fetch_row_result(lua_State *L, cur_data *cur, MYSQL_ROW row) {
  if (row == NULL) {
    if (free_result_start(L, cur) == CONTINUE_YIELD) {
      return CONTINUE_YIELD;
    } else {
      lua_pushnil(L);
      return 1;
    }
  } else {
    unsigned long *lengths = mysql_fetch_lengths(cur->my_res);

    int i;
    if (cur->colnames == LUA_NOREF) {
      create_colinfo(L, cur);
    }

    lua_settop(L, 0);
    lua_newtable(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, cur->colnames);

    for (i = 0; i < cur->numcols; i++) {
      lua_rawgeti(L, -1, i + 1);
      pushvalue(L, row[i], lengths[i]);
      lua_rawset(L, 1);
    }

    lua_pushvalue(L, 1);
    return 1;
  }
}

static void fetch_row_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  lua_State *L = ms->L;
  cur_data *cur = (cur_data *)ms->data;

  MYSQL_ROW row = NULL;
  int status = mysql_fetch_row_cont(&row, cur->my_res, ms->status);

  if (status) {
    wait_for_status(L, cur->conn_data, cur, status, fetch_row_cont, ms->extra);
  } else {
    int count = fetch_row_result(L, cur, row);
    if (count >= 0) {
      FAN_RESUME(L, NULL, count);
      UNREF_CO(cur);
    } else if (count == CONTINUE_YIELD) {
      // continue yield
    }
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int fetch_row_start(lua_State *L) {
  cur_data *cur = getcursor(L);
  MYSQL_RES *res = cur->my_res;

  MYSQL_ROW row = NULL;
  int status = mysql_fetch_row_start(&row, res);

  if (status) {
    REF_CO(cur);
    wait_for_status(L, cur->conn_data, cur, status, fetch_row_cont, 0);
    return lua_yield(L, 0);
  } else {
    int count = fetch_row_result(L, cur, row);
    if (count == CONTINUE_YIELD) {
      REF_CO(cur);
      return lua_yield(L, 0);
    } else {
      return count;
    }
  }
}

/*
** Cursor object collector function
*/
LUA_API int cur_gc(lua_State *L) {
  cur_data *cur = (cur_data *)luaL_checkudata(L, 1, LUASQL_CURSOR_MYSQL);
  if (cur != NULL && !(cur->closed)) {
    if (free_result_start(L, cur) == CONTINUE_YIELD) {
      REF_CO(cur);
      return lua_yield(L, 0);
    } else {
      lua_pushboolean(L, true);
      return 1;
    }
  }
  return 0;
}

/*
** Close the cursor on top of the stack.
** Return 1
*/
LUA_API int cur_close(lua_State *L) {
  cur_data *cur = (cur_data *)luaL_checkudata(L, 1, LUASQL_CURSOR_MYSQL);
  luaL_argcheck(L, cur != NULL, 1, LUASQL_PREFIX "cursor expected");
  if (cur->closed) {
    lua_pushboolean(L, 0);
    return 1;
  }

  if (free_result_start(L, cur) < 0) {
    REF_CO(cur);
    return lua_yield(L, 0);
  } else {
    lua_pushboolean(L, 1);
    return 1;
  }
}

/*
** Pushes a column information table on top of the stack.
** If the table isn't built yet, call the creator function and stores
** a reference to it on the cursor structure.
*/
static void _pushtable(lua_State *L, cur_data *cur, size_t off) {
  int *ref = (int *)((char *)cur + off);

  /* If colnames or coltypes do not exist, create both. */
  if (*ref == LUA_NOREF)
    create_colinfo(L, cur);

  /* Pushes the right table (colnames or coltypes) */
  lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
}

#define pushtable(L, c, m) (_pushtable(L, c, offsetof(cur_data, m)))

/*
** Return the list of field names.
*/
LUA_API int cur_getcolnames(lua_State *L) {
  pushtable(L, getcursor(L), colnames);
  return 1;
}

/*
** Return the list of field types.
*/
LUA_API int cur_getcoltypes(lua_State *L) {
  pushtable(L, getcursor(L), coltypes);
  return 1;
}

/*
** Push the number of rows.
*/
LUA_API int cur_numrows(lua_State *L) {
  lua_pushnumber(L, (lua_Number)mysql_num_rows(getcursor(L)->my_res));
  return 1;
}

/*
** Create a new Cursor object and push it on top of the stack.
*/
static int create_cursor(lua_State *L, conn_data *conn_data, MYSQL_RES *result,
                         int cols) {
  cur_data *cur = (cur_data *)lua_newuserdata(L, sizeof(cur_data));
  luasql_setmeta(L, LUASQL_CURSOR_MYSQL);
  cur->coref = LUA_NOREF;

  /* fill in structure */
  cur->closed = 0;
  cur->numcols = cols;
  cur->colnames = LUA_NOREF;
  cur->coltypes = LUA_NOREF;
  cur->my_res = result;
  cur->conn_data = conn_data;

  return 1;
}
