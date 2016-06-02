#define true 1
#define false 0

#include "utlua.h"

#include "luasql.h"
#include <mysql/mysql.h>

#define MYSQL_SET_VARSTRING(bind, buff, length)                                \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_VAR_STRING;                               \
    (bind)->buffer = (buff);                                                   \
    (bind)->buffer_length = (length);                                          \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_ULONGLONG(bind, buff)                                        \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_LONGLONG;                                 \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = true;                                                \
    (bind)->buffer_length = sizeof(uint64_t);                                  \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_DOUBLE(bind, buff)                                           \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_DOUBLE;                                   \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = false;                                               \
    (bind)->buffer_length = sizeof(double);                                    \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_LONG(bind, buff)                                             \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_LONG;                                     \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = false;                                               \
    (bind)->buffer_length = sizeof(long);                                      \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_TIMESTAMP(bind, buff)                                        \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_TIMESTAMP;                                \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = true;                                                \
    (bind)->buffer_length = sizeof(MYSQL_TIME);                                \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_ULONG(bind, buff)                                            \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_LONG;                                     \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = true;                                                \
    (bind)->buffer_length = sizeof(uint32_t);                                  \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_UINT(bind, buff)                                             \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_LONG;                                     \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = true;                                                \
    (bind)->buffer_length = sizeof(unsigned int);                              \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_SHORT(bind, buff)                                            \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_SHORT;                                    \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = false;                                               \
    (bind)->buffer_length = sizeof(short);                                     \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_USHORT(bind, buff)                                           \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_SHORT;                                    \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = true;                                                \
    (bind)->buffer_length = sizeof(unsigned short);                            \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_TINYINT(bind, buff)                                          \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_TINY;                                     \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = false;                                               \
    (bind)->buffer_length = sizeof(int8_t);                                    \
    (bind)->is_null = 0;                                                       \
  }

#define MYSQL_SET_UTINYINT(bind, buff)                                         \
  {                                                                            \
    (bind)->buffer_type = MYSQL_TYPE_TINY;                                     \
    (bind)->buffer = (buff);                                                   \
    (bind)->is_unsigned = true;                                                \
    (bind)->buffer_length = sizeof(uint8_t);                                   \
    (bind)->is_null = 0;                                                       \
  }

LUA_API int luaopen_mariadb(lua_State *L);

typedef struct {
  short closed;
  MYSQL *my_conn;
} conn_data;

struct maria_status {
  lua_State *L;
  void *data;
  int status;
  struct event *event;
  conn_data *conn_data;
  int extra;
};

typedef struct {
  short closed;
  int conn;               /* reference to connection */
  int numcols;            /* number of columns */
  int colnames, coltypes; /* reference to column information tables */
  MYSQL_RES *my_res;
  conn_data *conn_data;
} cur_data;

typedef struct {
  short closed;
  int table;
  int conn; /* reference to connection */
  int bind;
  int nums;
  int has_bind_param;

  int rbind;
  int buffers;
  int bufferlens;
  int is_nulls;

  MYSQL_STMT *my_stmt;
  conn_data *conn_data;
} st_data;

static void wait_for_status(lua_State *L, conn_data *conn, void *data,
                            int status, event_callback_fn callback, int extra);

#define LUASQL_ENVIRONMENT_MYSQL "MySQL environment"
#define LUASQL_CONNECTION_MYSQL "MySQL connection"
#define LUASQL_STATEMENT_MYSQL "MySQL prepared statement"
#define LUASQL_CURSOR_MYSQL "MySQL cursor"

#define CONTINUE_YIELD -1

static void wait_for_status(lua_State *L, conn_data *cdata, void *data,
                            int status, event_callback_fn callback, int extra) {
  struct maria_status *ms = malloc(sizeof(struct maria_status));
  ms->data = data;
  ms->L = L;
  ms->status = status;
  ms->conn_data = cdata;
  ms->extra = extra;

  short wait_event = 0;
  struct timeval tv, *ptv;
  int fd;

  if (status & MYSQL_WAIT_READ) {
    wait_event |= EV_READ;
  }
  if (status & MYSQL_WAIT_WRITE) {
    wait_event |= EV_WRITE;
  }
  if (wait_event)
    fd = mysql_get_socket(cdata->my_conn);
  else
    fd = -1;
  if (status & MYSQL_WAIT_TIMEOUT) {
    tv.tv_sec = mysql_get_timeout_value(cdata->my_conn);
    tv.tv_usec = 0;
    ptv = &tv;
  } else
    ptv = NULL;

  ms->event = event_new(event_mgr_base(), fd, wait_event, callback, ms);
  event_add(ms->event, ptv);
}

static conn_data *getconnection(lua_State *L) {
  conn_data *conn = (conn_data *)luaL_checkudata(L, 1, LUASQL_CONNECTION_MYSQL);
  luaL_argcheck(L, conn != NULL, 1, "connection expected");
  luaL_argcheck(L, !conn->closed, 1, "connection is closed");
  return conn;
}

static cur_data *getcursor(lua_State *L) {
  cur_data *cur = (cur_data *)luaL_checkudata(L, 1, LUASQL_CURSOR_MYSQL);
  luaL_argcheck(L, cur != NULL, 1, "cursor expected");
  luaL_argcheck(L, !cur->closed, 1, "cursor is closed");
  return cur;
}

static st_data *getstatement(lua_State *L) {
  st_data *st = (st_data *)luaL_checkudata(L, 1, LUASQL_STATEMENT_MYSQL);
  luaL_argcheck(L, st != NULL, 1, "statement expected");
  luaL_argcheck(L, !st->closed, 1, "statement is closed");
  return st;
}

static int luamariadb_close(lua_State *L, conn_data *conn_data) {
  if (conn_data && !conn_data->closed) {
    int errorcode = mysql_errno(conn_data->my_conn);
    int count = 0;
    if (errorcode) {
      lua_pushnil(L);
      lua_pushstring(L, mysql_error(conn_data->my_conn));
      count = 2;
    }

    conn_data->closed = 1;
    mysql_close(conn_data->my_conn);
    return count;
  } else {
    lua_pushnil(L);
    lua_pushstring(L, "luamariadb_close on null or closed connection.");
    return 2;
  }
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
    luaL_unref(L, LUA_REGISTRYINDEX, cur->conn);
    luaL_unref(L, LUA_REGISTRYINDEX, cur->colnames);
    luaL_unref(L, LUA_REGISTRYINDEX, cur->coltypes);

    cur->conn_data = NULL;

    lua_pushboolean(L, true);
    utlua_resume(L, NULL, 1);
  }

  event_free(ms->event);
  free(ms);
}

static int free_result_start(lua_State *L, cur_data *cur) {
  cur->closed = 1;

  conn_data *conn = cur->conn_data;
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
      utlua_resume(L, NULL, count);
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
    wait_for_status(L, cur->conn_data, cur, status, fetch_row_cont, 0);
    return lua_yield(L, 0);
  } else {
    int count = fetch_row_result(L, cur, row);
    if (count == CONTINUE_YIELD) {
      return lua_yield(L, 0);
    } else {
      return count;
    }
  }
}

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
    utlua_resume(L, NULL, 1);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }
}

LUA_API int stmt_close_start(lua_State *L, st_data *st) {
  st->closed = 1;

  my_bool ret = 0;
  int status = mysql_stmt_close_start(&ret, st->my_stmt);
  if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_close_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, st->table);
    lua_pushboolean(L, 1);
    return 1;
  } else {
    return luamariadb_close(L, st->conn_data);
  }

  return 0;
}

LUA_API int st_gc(lua_State *L) {
  st_data *st = (st_data *)luaL_checkudata(L, 1, LUASQL_STATEMENT_MYSQL);
  if (st != NULL && !st->closed) {
    return stmt_close_start(L, st);
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

static int stmt_fetch_result(lua_State *L, st_data *st) {
  MYSQL_RES *prepare_meta_result = mysql_stmt_result_metadata(st->my_stmt);

  unsigned int field_count = mysql_num_fields(prepare_meta_result);
  MYSQL_FIELD *fields = mysql_fetch_fields(prepare_meta_result);

  mysql_free_result(prepare_meta_result);

  lua_rawgeti(L, LUA_REGISTRYINDEX, st->table);
  int tableidx = lua_gettop(L); // never pop

  int *buffers =
      get_or_create_ud(L, tableidx, &st->buffers, field_count * sizeof(int));
  unsigned long *bufferlens = get_or_create_ud(
      L, tableidx, &st->bufferlens, field_count * sizeof(unsigned long));
  my_bool *is_nulls = get_or_create_ud(L, tableidx, &st->is_nulls,
                                       field_count * sizeof(my_bool));

  lua_pushboolean(L, true);

  int i = 0;
  for (; i < field_count; i++) {
    if (is_nulls[i]) {
      lua_pushnil(L);
      continue;
    }

    switch (fields[i].type) {
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING: {
      void *buffer =
          get_or_create_ud(L, tableidx, &buffers[i], fields[i].length);
      lua_pushlstring(L, buffer, bufferlens[i]);
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TINY: {
      void *buffer = get_or_create_ud(L, tableidx, &buffers[i], sizeof(double));
      lua_pushnumber(L, *((double *)buffer));
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP: {
      MYSQL_TIME *buffer = (MYSQL_TIME *)get_or_create_ud(
          L, tableidx, &buffers[i], sizeof(MYSQL_TIME));

      if (buffer->time_type > 0) {
        lua_newtable(L);

        lua_pushinteger(L, buffer->year);
        lua_setfield(L, -2, "year");

        lua_pushinteger(L, buffer->month);
        lua_setfield(L, -2, "month");

        lua_pushinteger(L, buffer->day);
        lua_setfield(L, -2, "day");

        lua_pushinteger(L, buffer->hour);
        lua_setfield(L, -2, "hour");

        lua_pushinteger(L, buffer->minute);
        lua_setfield(L, -2, "minute");

        lua_pushinteger(L, buffer->second);
        lua_setfield(L, -2, "second");

        lua_pushinteger(L, buffer->second_part);
        lua_setfield(L, -2, "second_part");
      } else {
        lua_pushnil(L);
      }
    } break;

    default:
      lua_pushnil(L);
      break;
    }
  }

  return field_count + 1;
}

static void stmt_fetch_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  st_data *st = (st_data *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int status = mysql_stmt_fetch_cont(&ret, st->my_stmt, ms->status);
  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    utlua_resume(L, NULL, 2);
  } else if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_fetch_cont, ms->extra);
  } else if (ret == 0) {
    int count = stmt_fetch_result(L, st);
    utlua_resume(L, NULL, count);
  } else {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    utlua_resume(L, NULL, 2);
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int stmt_fetch_start(lua_State *L) {
  st_data *st = getstatement(L);

  int ret = 0;
  int status = mysql_stmt_fetch_start(&ret, st->my_stmt);
  if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_fetch_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    int count = stmt_fetch_result(L, st);
    return count;
  } else if (ret == MYSQL_NO_DATA) {
    return 0;
  } else {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    return 2;
  }
}

LUA_API int st_pairs(lua_State *L) {
  getstatement(L);
  lua_pushcfunction(L, stmt_fetch_start);
  lua_pushvalue(L, 1);

  return 2;
}

static void stmt_store_result_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  st_data *st = (st_data *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int status = mysql_stmt_store_result_cont(&ret, st->my_stmt, ms->status);
  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    utlua_resume(L, NULL, 2);
  } else if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_store_result_cont,
                    ms->extra);
  } else if (ret == 0) {
    utlua_resume(L, NULL, 0);
  } else {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));
    utlua_resume(L, NULL, 2);
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
    return 0;
  } else {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));
    return 2;
  }
}

static int stmt_execute_result(lua_State *L, st_data *st) {
  MYSQL_RES *prepare_meta_result = mysql_stmt_result_metadata(st->my_stmt);

  if (!prepare_meta_result) {
    my_ulonglong affected_rows = mysql_stmt_affected_rows(st->my_stmt);
    lua_pushinteger(L, affected_rows);
    return 1;
  } else {
    unsigned int field_count = mysql_num_fields(prepare_meta_result);
    MYSQL_FIELD *fields = mysql_fetch_fields(prepare_meta_result);
    lua_rawgeti(L, LUA_REGISTRYINDEX, st->table);
    int tableidx = lua_gettop(L);

    MYSQL_BIND *rbind = get_or_create_ud(L, tableidx, &st->rbind,
                                         field_count * sizeof(MYSQL_BIND));
    int *buffers =
        get_or_create_ud(L, tableidx, &st->buffers, field_count * sizeof(int));
    unsigned long *bufferlens = get_or_create_ud(
        L, tableidx, &st->bufferlens, field_count * sizeof(unsigned long));
    my_bool *is_nulls = get_or_create_ud(L, tableidx, &st->is_nulls,
                                         field_count * sizeof(my_bool));

    mysql_free_result(prepare_meta_result);

    int i = 0;
    for (; i < field_count; i++) {
      switch (fields[i].type) {
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING: {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], fields[i].length);
        MYSQL_SET_VARSTRING(&rbind[i], buffer, fields[i].length);
        rbind[i].length = &bufferlens[i];
        rbind[i].is_null = &is_nulls[i];
      } break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_TINY: {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], sizeof(double));
        MYSQL_SET_DOUBLE(&rbind[i], buffer);
        rbind[i].length = &bufferlens[i];
        rbind[i].is_null = &is_nulls[i];
      } break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP: {
        void *buffer =
            get_or_create_ud(L, tableidx, &buffers[i], sizeof(MYSQL_TIME));
        MYSQL_SET_TIMESTAMP(&rbind[i], buffer);
        rbind[i].length = &bufferlens[i];
        rbind[i].is_null = &is_nulls[i];
      } break;

      default:
        break;
      }
    }

    lua_pop(L, 1); // pop table

    if (mysql_stmt_bind_result(st->my_stmt, rbind)) {
      lua_pushnil(L);
      lua_pushstring(L, mysql_stmt_error(st->my_stmt));
      return 2;
    }

    return stmt_store_result_start(L, st);
  }
}

static void stmt_execute_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  st_data *st = (st_data *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int status = mysql_stmt_execute_cont(&ret, st->my_stmt, ms->status);
  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    utlua_resume(L, NULL, 2);
  } else if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_execute_cont, ms->extra);
  } else if (ret == 0) {
    int count = stmt_execute_result(L, st);
    if (count != CONTINUE_YIELD) {
      utlua_resume(L, NULL, count);
    }
  } else {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    utlua_resume(L, NULL, 2);
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int stmt_execute_start(lua_State *L) {
  st_data *st = getstatement(L);

  int ret = 0;
  int status = mysql_stmt_execute_start(&ret, st->my_stmt);

  if (status) {
    wait_for_status(L, st->conn_data, st, status, stmt_execute_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    int count = stmt_execute_result(L, st);
    if (count >= 0) {
      return count;
    } else {
      return lua_yield(L, 0);
    }
  } else {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));
    return 2;
  }
}

LUA_API int st_bind_param(lua_State *L) {
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

  lua_pop(L, 3);

  int i = 0;
  for (; i < param_count; i++) {
    int idx = i + 2;
    switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
      size_t sz = 0;
      const char *str = lua_tolstring(L, idx, &sz);
      MYSQL_SET_VARSTRING(&bind[i], (void *)str, sz);
    } break;
    case LUA_TNUMBER: {
      nums[i] = lua_tonumber(L, idx);
      MYSQL_SET_DOUBLE(&bind[i], &nums[i]);
    } break;
    case LUA_TBOOLEAN: {
      nums[i] = lua_toboolean(L, idx);
      MYSQL_SET_TINYINT(&bind[i], &nums[i]);
      break;
    }
    default:
      break;
    }
  }

  mysql_stmt_bind_param(st->my_stmt, bind);

  lua_pushvalue(L, 1);

  return 1;
}

LUA_API int st_bind(lua_State *L) {
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

  lua_pop(L, 3);

  int i = 0;
  for (; i < param_count; i++) {
    int idx = i + 2;
    switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
      size_t sz = 0;
      const char *str = lua_tolstring(L, idx, &sz);
      MYSQL_SET_VARSTRING(&bind[i], (void *)str, sz);
    } break;
    case LUA_TNUMBER: {
      nums[i] = lua_tonumber(L, idx);
      MYSQL_SET_DOUBLE(&bind[i], &nums[i]);
    } break;
    case LUA_TBOOLEAN: {
      nums[i] = lua_toboolean(L, idx);
      MYSQL_SET_TINYINT(&bind[i], &nums[i]);
      break;
    }
    default:
      break;
    }
  }

  if (!st->has_bind_param) {
    mysql_stmt_bind_param(st->my_stmt, bind);
    st->has_bind_param = 1;
  }

  lua_pushvalue(L, 1);

  return 1;
}

/*
** Cursor object collector function
*/
LUA_API int cur_gc(lua_State *L) {
  cur_data *cur = (cur_data *)luaL_checkudata(L, 1, LUASQL_CURSOR_MYSQL);
  if (cur != NULL && !(cur->closed)) {
    if (free_result_start(L, cur) == CONTINUE_YIELD) {
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
static int create_cursor(lua_State *L, int conn, MYSQL_RES *result, int cols) {
  cur_data *cur = (cur_data *)lua_newuserdata(L, sizeof(cur_data));
  luasql_setmeta(L, LUASQL_CURSOR_MYSQL);

  /* fill in structure */
  cur->closed = 0;
  cur->conn = LUA_NOREF;
  cur->numcols = cols;
  cur->colnames = LUA_NOREF;
  cur->coltypes = LUA_NOREF;
  cur->my_res = result;
  lua_pushvalue(L, conn);
  cur->conn_data = lua_touserdata(L, -1);
  cur->conn = luaL_ref(L, LUA_REGISTRYINDEX);

  return 1;
}

LUA_API int conn_gc(lua_State *L) {
  conn_data *conn = (conn_data *)luaL_checkudata(L, 1, LUASQL_CONNECTION_MYSQL);

  if (conn != NULL && !(conn->closed)) {
    conn->closed = 1;
    mysql_close(conn->my_conn);
  }
  return 0;
}

/*
** Close a Connection object.
*/
LUA_API int conn_close(lua_State *L) {
  conn_data *conn = (conn_data *)luaL_checkudata(L, 1, LUASQL_CONNECTION_MYSQL);
  luaL_argcheck(L, conn != NULL, 1, LUASQL_PREFIX "connection expected");
  if (conn->closed) {
    lua_pushboolean(L, 0);
    return 1;
  } else {
    conn_gc(L);
    lua_pushboolean(L, 1);
    return 1;
  }
}

LUA_API int escape_string(lua_State *L) {
  size_t size, new_size;
  conn_data *conn = getconnection(L);
  const char *from = luaL_checklstring(L, 2, &size);
  char *to;
  to = (char *)malloc(sizeof(char) * (2 * size + 1));
  if (to) {
    new_size = mysql_real_escape_string(conn->my_conn, to, from, size);
    lua_pushlstring(L, to, new_size);
    free(to);
    return 1;
  }
  luaL_error(L, "could not allocate escaped string");
  return 0;
}

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

  int ret = 0;
  int errorcode = mysql_stmt_errno(st->my_stmt);
  if (errorcode) {
    lua_pushnil(L);
    lua_pushstring(L, mysql_stmt_error(st->my_stmt));

    utlua_resume(L, NULL, 2);
  } else {
    int status = mysql_stmt_prepare_cont(&ret, st->my_stmt, ms->status);
    if (status) {
      wait_for_status(L, ms->conn_data, st, status, stmt_prepare_cont,
                      ms->extra);
      skip_unref = 1;
    } else if (ret == 0) {
      stmt_prepare_result(L, st);

      lua_rawgeti(L, LUA_REGISTRYINDEX, ms->extra);
      utlua_resume(L, NULL, 1);
    } else {
      if (ret) {
        lua_pushnil(L);
        lua_pushstring(L, mysql_error(st->my_stmt->mysql));

        utlua_resume(L, NULL, 2);
      } else {
        utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
      }
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

  MYSQL_STMT *stmt = mysql_stmt_init(conn->my_conn);

  st_data *st = (st_data *)lua_newuserdata(L, sizeof(st_data));
  memset(st, 0, sizeof(st_data));

  luasql_setmeta(L, LUASQL_STATEMENT_MYSQL);
  st->my_stmt = stmt;

  lua_newtable(L);

  lua_pushvalue(L, 1);
  st->conn = luaL_ref(L, -2);
  st->conn_data = conn;

  st->table = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned long type = (unsigned long)CURSOR_TYPE_READ_ONLY;
  unsigned long prefetch_rows = 5;
  int rc = mysql_stmt_attr_set(stmt, STMT_ATTR_CURSOR_TYPE, (void *)&type);
  /* ... check return value ... */
  rc = mysql_stmt_attr_set(stmt, STMT_ATTR_PREFETCH_ROWS,
                           (void *)&prefetch_rows);

  int ret = 0;
  int status = mysql_stmt_prepare_start(&ret, stmt, statement, st_len);
  if (status) {
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    wait_for_status(L, conn, st, status, stmt_prepare_cont, ref);
    return lua_yield(L, 0);
  } else {
    stmt_prepare_result(L, st);
    return 1;
  }
}

static int conn_ping_result(lua_State *L, conn_data *conn_data) {
  lua_pushboolean(L, true);
  return 1;
}

static void conn_ping_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int status = mysql_ping_cont(&ret, conn, ms->status);
  int errorcode = mysql_errno(conn);
  if (errorcode) {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  } else if (status) {
    wait_for_status(L, ms->conn_data, conn, status, conn_ping_event, ms->extra);
  } else if (ret == 0) {
    int count = conn_ping_result(L, ms->conn_data);
    utlua_resume(L, NULL, count);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int conn_ping_start(lua_State *L) {
  conn_data *conn = getconnection(L);

  int ret = 0;
  int status = mysql_ping_start(&ret, conn->my_conn);
  if (status) {
    wait_for_status(L, conn, conn->my_conn, status, conn_ping_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    return conn_ping_result(L, conn);
  } else {
    return luamariadb_close(L, conn);
  }
}

static int real_query_result(lua_State *L, conn_data *conn_data) {
  MYSQL_RES *res = mysql_store_result(conn_data->my_conn);
  unsigned int num_cols = mysql_field_count(conn_data->my_conn);

  if (res) {
    int count = create_cursor(L, 1, res, num_cols);
    return count;
  } else {
    if (num_cols == 0) {
      lua_pushnumber(L, mysql_affected_rows(conn_data->my_conn));
      return 1;
    } else {
      return luamariadb_close(L, conn_data);
    }
  }
}

static void real_query_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int ret = 0;
  int status = mysql_real_query_cont(&ret, conn, ms->status);
  int errorcode = mysql_errno(conn);
  if (errorcode) {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  } else if (status) {
    wait_for_status(L, ms->conn_data, conn, status, real_query_cont, ms->extra);
  } else if (ret == 0) {
    int count = real_query_result(L, ms->conn_data);
    utlua_resume(L, NULL, count);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int real_query_start(lua_State *L) {
  conn_data *conn = getconnection(L);
  size_t st_len;
  const char *statement = luaL_checklstring(L, 2, &st_len);

  int ret = 0;
  int status = mysql_real_query_start(&ret, conn->my_conn, statement, st_len);

  if (status) {
    wait_for_status(L, conn, conn->my_conn, status, real_query_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    return real_query_result(L, conn);
  } else {
    return luamariadb_close(L, conn);
  }
}

static void conn_commit_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  my_bool ret = 0;
  int status = mysql_commit_cont(&ret, conn, ms->status);
  int errorcode = mysql_errno(conn);
  if (errorcode) {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  } else if (status) {
    wait_for_status(L, ms->conn_data, conn, status, conn_commit_event,
                    ms->extra);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    utlua_resume(L, NULL, 1);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int conn_commit_start(lua_State *L) {
  conn_data *conn = getconnection(L);

  my_bool ret = 0;
  int status = mysql_commit_start(&ret, conn->my_conn);
  if (status) {
    wait_for_status(L, conn, conn->my_conn, status, conn_commit_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    return 1;
  } else {
    return luamariadb_close(L, conn);
  }
}

static void conn_rollback_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  my_bool ret = 0;
  int status = mysql_rollback_cont(&ret, conn, ms->status);
  int errorcode = mysql_errno(conn);
  if (errorcode) {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  } else if (status) {
    wait_for_status(L, ms->conn_data, conn, status, conn_rollback_event,
                    ms->extra);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    utlua_resume(L, NULL, 1);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int conn_rollback_start(lua_State *L) {
  conn_data *conn = getconnection(L);
  my_bool ret = 0;
  int status = mysql_rollback_start(&ret, conn->my_conn);
  if (status) {
    wait_for_status(L, conn, conn->my_conn, status, conn_rollback_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    return 1;
  } else {
    return luamariadb_close(L, conn);
  }
}

static void conn_autocommit_event(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  my_bool ret = 0;
  int status = mysql_autocommit_cont(&ret, conn, ms->status);
  int errorcode = mysql_errno(conn);
  if (errorcode) {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  } else if (status) {
    wait_for_status(L, ms->conn_data, conn, status, conn_autocommit_event,
                    ms->extra);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    utlua_resume(L, NULL, 1);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int conn_autocommit_start(lua_State *L) {
  conn_data *conn = getconnection(L);
  my_bool auto_mode = lua_toboolean(L, 2);

  my_bool ret = 0;
  int status = mysql_autocommit_start(&ret, conn->my_conn, auto_mode);
  if (status) {
    wait_for_status(L, conn, conn->my_conn, status, conn_autocommit_event, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    lua_pushboolean(L, true);
    return 1;
  } else {
    return luamariadb_close(L, conn);
  }
}

/*
** Get Last auto-increment id generated
*/
LUA_API int conn_getlastautoid(lua_State *L) {
  conn_data *conn = getconnection(L);
  lua_pushnumber(L, mysql_insert_id(conn->my_conn));
  return 1;
}

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
    utlua_resume(L, NULL, 1);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  event_free(ms->event);
  free(ms);
}

LUA_API int set_character_set_start(lua_State *L) {
  conn_data *conn = getconnection(L);

  const char *charset = luaL_checkstring(L, 2);

  int ret = 0;
  int status = mysql_set_character_set_start(&ret, conn->my_conn, charset);
  if (status) {
    wait_for_status(L, conn, conn->my_conn, status, set_character_set_cont, 0);
    return lua_yield(L, 0);
  } else if (ret == 0) {
    lua_pushboolean(L, 1);
    return 1;
  } else {
    return luamariadb_close(L, conn);
  }
}

static void real_connect_cont(int fd, short event, void *_userdata) {
  struct maria_status *ms = (struct maria_status *)_userdata;
  MYSQL *conn = (MYSQL *)ms->data;
  lua_State *L = ms->L;

  int skip_unref = 0;

  MYSQL *ret = NULL;
  int status = mysql_real_connect_cont(&ret, conn, ms->status);
  if (status) {
    wait_for_status(L, ms->conn_data, conn, status, real_connect_cont,
                    ms->extra);
    skip_unref = 1;
  } else if (ret == conn) {
    char value = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &value);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ms->extra);
    utlua_resume(L, NULL, 1);
  } else {
    utlua_resume(L, NULL, luamariadb_close(L, ms->conn_data));
  }

  if (!skip_unref) {
    luaL_unref(L, LUA_REGISTRYINDEX, ms->extra);
  }
  event_free(ms->event);
  free(ms);
}

/*
** Connects to a data source.
**     param: one string for each connection parameter, said
**     datasource, username, password, host and port.
*/
LUA_API int real_connect_start(lua_State *L) {
  const char *sourcename = luaL_checkstring(L, 1);
  const char *username = luaL_optstring(L, 2, NULL);
  const char *password = luaL_optstring(L, 3, NULL);
  const char *host = luaL_optstring(L, 4, NULL);
  const int port = luaL_optinteger(L, 5, 0);
  MYSQL *my_conn, *ret;

  /* Try to init the connection object. */
  my_conn = mysql_init(NULL);
  char value = 1;
  mysql_options(my_conn, MYSQL_OPT_NONBLOCK, 0);
  mysql_options(my_conn, MYSQL_OPT_RECONNECT, &value);

  conn_data *cdata = (conn_data *)lua_newuserdata(L, sizeof(conn_data));
  luasql_setmeta(L, LUASQL_CONNECTION_MYSQL);

  /* fill in structure */
  cdata->closed = 0;
  cdata->my_conn = my_conn;

  int status = mysql_real_connect_start(&ret, my_conn, host, username, password,
                                        sourcename, port, NULL, 0);
  if (status) {
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    wait_for_status(L, cdata, my_conn, status, real_connect_cont, ref);
    return lua_yield(L, 0);
  } else if (ret == my_conn) {
    mysql_options(my_conn, MYSQL_OPT_RECONNECT, &value);
    return 1;
  } else {
    return luamariadb_close(L, cdata);
  }
}

/*
** Create metatables for each class of object.
*/
static void create_metatables(lua_State *L) {
  struct luaL_Reg connection_methods[] = {
      {"__gc", conn_gc},
      {"close", conn_close},
      {"ping", conn_ping_start},
      {"escape", escape_string},
      {"execute", real_query_start},
      {"setcharset", set_character_set_start},
      {"prepare", stmt_prepare_start},

      {"commit", conn_commit_start},
      {"rollback", conn_rollback_start},
      {"autocommit", conn_autocommit_start},

      {"getlastautoid", conn_getlastautoid},
      {NULL, NULL},
  };
  struct luaL_Reg cursor_methods[] = {
      {"__gc", cur_gc},
      {"close", cur_close},
      {"getcolnames", cur_getcolnames},
      {"getcoltypes", cur_getcoltypes},
      {"fetch", fetch_row_start},
      {"numrows", cur_numrows},
      {NULL, NULL},
  };

  struct luaL_Reg statement_methods[] = {
      {"__gc", st_gc},
      {"close", st_close},
      {"bind_param", st_bind_param},
      {"bind", st_bind},
      {"execute", stmt_execute_start},
      {"fetch", stmt_fetch_start},
      {"pairs", st_pairs},
      {NULL, NULL},
  };

  luasql_createmeta(L, LUASQL_CONNECTION_MYSQL, connection_methods);
  luasql_createmeta(L, LUASQL_CURSOR_MYSQL, cursor_methods);
  luasql_createmeta(L, LUASQL_STATEMENT_MYSQL, statement_methods);
  lua_pop(L, 3);
}

/*
** Creates the metatables for the objects and registers the
** driver open method.
*/
LUA_API int luaopen_fan_mariadb(lua_State *L) {
  struct luaL_Reg driver[] = {
      {"connect", real_connect_start}, {NULL, NULL},
  };
  create_metatables(L);

  lua_newtable(L);
  luaL_setfuncs(L, driver, 0);
  return 1;
}
