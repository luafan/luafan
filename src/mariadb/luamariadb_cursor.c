#include "luamariadb_cursor.h"

CURSOR_CTX *getcursor(lua_State *L)
{
  CURSOR_CTX *cur = (CURSOR_CTX *)luaL_checkudata(L, 1, MARIADB_CURSOR_METATABLE);
  luaL_argcheck(L, cur != NULL, 1, "cursor expected");
  luaL_argcheck(L, !cur->closed, 1, "cursor is closed");
  return cur;
}

static void pushvalue(lua_State *L, void *row, long int len, enum enum_field_types field_type)
{
  if (row == NULL) {
    lua_pushnil(L);
  } else {
    switch (field_type) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_YEAR:
        // Integer types - convert to Lua integer
        lua_pushinteger(L, strtoll((char*)row, NULL, 10));
        break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        // Floating point types - convert to Lua number
        lua_pushnumber(L, strtod((char*)row, NULL));
        break;
      default:
        // All other types (strings, dates, blobs, etc.) - keep as string
        lua_pushlstring(L, row, len);
        break;
    }
  }
}

static char *getcolumntype(enum enum_field_types type)
{
  switch (type)
  {
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
static void create_colinfo(lua_State *L, CURSOR_CTX *cur)
{
  MYSQL_FIELD *fields;
  char typename[50];
  int i;
  fields = mysql_fetch_fields(cur->my_res);
  lua_newtable(L); /* names */
  lua_newtable(L); /* types */
  for (i = 1; i <= cur->numcols; i++)
  {
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

static void free_result_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  CURSOR_CTX *cur = (CURSOR_CTX *)bag->data;
  lua_State *L = bag->L;

  int status = mysql_free_result_cont(cur->my_res, bag->status);
  if (status)
  {
    wait_for_status(L, bag->ctx, cur->my_res, status, free_result_cont,
                    bag->extra);
  }
  else
  {
    lua_pushboolean(L, true);
    FAN_RESUME(L, NULL, 1);
    UNREF_CO(cur);
  }

  event_free(bag->event);
  free(bag);
}

static int free_result_start(lua_State *L, CURSOR_CTX *cur)
{
  cur->closed = 1;

  luaL_unref(L, LUA_REGISTRYINDEX, cur->colnames);
  cur->colnames = LUA_NOREF;

  luaL_unref(L, LUA_REGISTRYINDEX, cur->coltypes);
  cur->coltypes = LUA_NOREF;

  DB_CTX *ctx = cur->ctx;
  cur->ctx = NULL;

  int status = mysql_free_result_start(cur->my_res);
  if (status)
  {
    wait_for_status(L, ctx, cur, status, free_result_cont, 0);
    return CONTINUE_YIELD;
  }
  else
  {
    return 0;
  }
}

static int fetch_row_result(lua_State *L, CURSOR_CTX *cur, MYSQL_ROW row)
{
  if (row == NULL)
  {
    if (free_result_start(L, cur) == CONTINUE_YIELD)
    {
      return CONTINUE_YIELD;
    }
    else
    {
      lua_pushnil(L);
      return 1;
    }
  }
  else
  {
    unsigned long *lengths = mysql_fetch_lengths(cur->my_res);
    MYSQL_FIELD *fields = mysql_fetch_fields(cur->my_res);

    int i;
    if (cur->colnames == LUA_NOREF)
    {
      create_colinfo(L, cur);
    }

    lua_settop(L, 0);
    lua_newtable(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, cur->colnames);

    for (i = 0; i < cur->numcols; i++)
    {
      lua_rawgeti(L, -1, i + 1);
      pushvalue(L, row[i], lengths[i], fields[i].type);
      lua_rawset(L, 1);
    }

    lua_pushvalue(L, 1);
    return 1;
  }
}

static void fetch_row_cont(int fd, short event, void *_userdata)
{
  DB_STATUS *bag = (DB_STATUS *)_userdata;
  lua_State *L = bag->L;
  CURSOR_CTX *cur = (CURSOR_CTX *)bag->data;

  MYSQL_ROW row = NULL;
  int status = mysql_fetch_row_cont(&row, cur->my_res, bag->status);

  if (status)
  {
    wait_for_status(L, cur->ctx, cur, status, fetch_row_cont, bag->extra);
  }
  else
  {
    int count = fetch_row_result(L, cur, row);
    if (count >= 0)
    {
      FAN_RESUME(L, NULL, count);
      UNREF_CO(cur);
    }
    else if (count == CONTINUE_YIELD)
    {
      // continue yield
    }
  }

  event_free(bag->event);
  free(bag);
}

LUA_API int fetch_row_start(lua_State *L)
{
  CURSOR_CTX *cur = getcursor(L);
  MYSQL_RES *res = cur->my_res;

  MYSQL_ROW row = NULL;
  int status = mysql_fetch_row_start(&row, res);

  if (status)
  {
    REF_CO(cur);
    wait_for_status(L, cur->ctx, cur, status, fetch_row_cont, 0);
    return lua_yield(L, 0);
  }
  else
  {
    int count = fetch_row_result(L, cur, row);
    if (count == CONTINUE_YIELD)
    {
      REF_CO(cur);
      return lua_yield(L, 0);
    }
    else
    {
      return count;
    }
  }
}

/*
** Cursor object collector function
*/
LUA_API int cur_gc(lua_State *L)
{
  CURSOR_CTX *cur = (CURSOR_CTX *)luaL_checkudata(L, 1, MARIADB_CURSOR_METATABLE);
  if (cur != NULL && !(cur->closed))
  {
    if (free_result_start(L, cur) == CONTINUE_YIELD)
    {
      REF_CO(cur);
      return lua_yield(L, 0);
    }
    else
    {
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
LUA_API int cur_close(lua_State *L)
{
  CURSOR_CTX *cur = (CURSOR_CTX *)luaL_checkudata(L, 1, MARIADB_CURSOR_METATABLE);
  luaL_argcheck(L, cur != NULL, 1, LUASQL_PREFIX "cursor expected");
  if (cur->closed)
  {
    lua_pushboolean(L, 0);
    return 1;
  }

  if (free_result_start(L, cur) < 0)
  {
    REF_CO(cur);
    return lua_yield(L, 0);
  }
  else
  {
    lua_pushboolean(L, 1);
    return 1;
  }
}

/*
** Pushes a column information table on top of the stack.
** If the table isn't built yet, call the creator function and stores
** a reference to it on the cursor structure.
*/
static void _pushtable(lua_State *L, CURSOR_CTX *cur, size_t off)
{
  int *ref = (int *)((char *)cur + off);

  /* If colnames or coltypes do not exist, create both. */
  if (*ref == LUA_NOREF)
    create_colinfo(L, cur);

  /* Pushes the right table (colnames or coltypes) */
  lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
}

#define pushtable(L, c, m) (_pushtable(L, c, offsetof(CURSOR_CTX, m)))

/*
** Return the list of field names.
*/
LUA_API int cur_getcolnames(lua_State *L)
{
  pushtable(L, getcursor(L), colnames);
  return 1;
}

/*
** Return the list of field types.
*/
LUA_API int cur_getcoltypes(lua_State *L)
{
  pushtable(L, getcursor(L), coltypes);
  return 1;
}

/*
** Push the number of rows.
*/
LUA_API int cur_numrows(lua_State *L)
{
  lua_pushnumber(L, (lua_Number)mysql_num_rows(getcursor(L)->my_res));
  return 1;
}

/*
** Create a new Cursor object and push it on top of the stack.
*/
int create_cursor(lua_State *L, DB_CTX *ctx, MYSQL_RES *result,
                  int cols)
{
  CURSOR_CTX *cur = (CURSOR_CTX *)lua_newuserdata(L, sizeof(CURSOR_CTX));
  luasql_setmeta(L, MARIADB_CURSOR_METATABLE);
  cur->coref = LUA_NOREF;

  /* fill in structure */
  cur->closed = 0;
  cur->numcols = cols;
  cur->colnames = LUA_NOREF;
  cur->coltypes = LUA_NOREF;
  cur->my_res = result;
  cur->ctx = ctx;

  return 1;
}
