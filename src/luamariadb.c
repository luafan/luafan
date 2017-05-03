#define true 1
#define false 0

#include "utlua.h"

#include "luasql.h"
#include <mysql/mysql.h>

static int LONG_DATA = 0; // &LONG_DATA used as mariadb const.

#define MYSQL_SET_VARSTRING(bind, buff, length)  \
  {                                              \
    (bind)->buffer_type = MYSQL_TYPE_VAR_STRING; \
    (bind)->buffer = (buff);                     \
    (bind)->buffer_length = (length);            \
    (bind)->is_null = 0;                         \
  }

#define MYSQL_SET_ULONGLONG(bind, buff)        \
  {                                            \
    (bind)->buffer_type = MYSQL_TYPE_LONGLONG; \
    (bind)->buffer = (buff);                   \
    (bind)->is_unsigned = true;                \
    (bind)->buffer_length = sizeof(uint64_t);  \
    (bind)->is_null = 0;                       \
  }

#define MYSQL_SET_DOUBLE(bind, buff)         \
  {                                          \
    (bind)->buffer_type = MYSQL_TYPE_DOUBLE; \
    (bind)->buffer = (buff);                 \
    (bind)->is_unsigned = false;             \
    (bind)->buffer_length = sizeof(double);  \
    (bind)->is_null = 0;                     \
  }

#define MYSQL_SET_LONG(bind, buff)         \
  {                                        \
    (bind)->buffer_type = MYSQL_TYPE_LONG; \
    (bind)->buffer = (buff);               \
    (bind)->is_unsigned = false;           \
    (bind)->buffer_length = sizeof(long);  \
    (bind)->is_null = 0;                   \
  }

#define MYSQL_SET_TIMESTAMP(bind, buff)         \
  {                                             \
    (bind)->buffer_type = MYSQL_TYPE_TIMESTAMP; \
    (bind)->buffer = (buff);                    \
    (bind)->is_unsigned = true;                 \
    (bind)->buffer_length = sizeof(MYSQL_TIME); \
    (bind)->is_null = 0;                        \
  }

#define MYSQL_SET_ULONG(bind, buff)           \
  {                                           \
    (bind)->buffer_type = MYSQL_TYPE_LONG;    \
    (bind)->buffer = (buff);                  \
    (bind)->is_unsigned = true;               \
    (bind)->buffer_length = sizeof(uint32_t); \
    (bind)->is_null = 0;                      \
  }

#define MYSQL_SET_UINT(bind, buff)                \
  {                                               \
    (bind)->buffer_type = MYSQL_TYPE_LONG;        \
    (bind)->buffer = (buff);                      \
    (bind)->is_unsigned = true;                   \
    (bind)->buffer_length = sizeof(unsigned int); \
    (bind)->is_null = 0;                          \
  }

#define MYSQL_SET_SHORT(bind, buff)         \
  {                                         \
    (bind)->buffer_type = MYSQL_TYPE_SHORT; \
    (bind)->buffer = (buff);                \
    (bind)->is_unsigned = false;            \
    (bind)->buffer_length = sizeof(short);  \
    (bind)->is_null = 0;                    \
  }

#define MYSQL_SET_USHORT(bind, buff)                \
  {                                                 \
    (bind)->buffer_type = MYSQL_TYPE_SHORT;         \
    (bind)->buffer = (buff);                        \
    (bind)->is_unsigned = true;                     \
    (bind)->buffer_length = sizeof(unsigned short); \
    (bind)->is_null = 0;                            \
  }

#define MYSQL_SET_TINYINT(bind, buff)       \
  {                                         \
    (bind)->buffer_type = MYSQL_TYPE_TINY;  \
    (bind)->buffer = (buff);                \
    (bind)->is_unsigned = false;            \
    (bind)->buffer_length = sizeof(int8_t); \
    (bind)->is_null = 0;                    \
  }

#define MYSQL_SET_UTINYINT(bind, buff)       \
  {                                          \
    (bind)->buffer_type = MYSQL_TYPE_TINY;   \
    (bind)->buffer = (buff);                 \
    (bind)->is_unsigned = true;              \
    (bind)->buffer_length = sizeof(uint8_t); \
    (bind)->is_null = 0;                     \
  }

LUA_API int luaopen_mariadb(lua_State *L);

typedef struct
{
  short closed;
  MYSQL my_conn;
  int coref;
  int coref_count;
} conn_data;

struct maria_status
{
  lua_State *L;
  void *data;
  int status;
  struct event *event;
  conn_data *conn_data;
  int extra;
};

typedef struct
{
  short closed;
  int numcols;            /* number of columns */
  int colnames, coltypes; // ref in registry
  MYSQL_RES *my_res;
  conn_data *conn_data;
  int coref;
  int coref_count;
} cur_data;

typedef struct
{
  short closed;
  int table; // ref in registry
  int bind;  // index in table
  int nums;  // index in table
  int has_bind_param;

  int rbind;      // index in table
  int buffers;    // index in table
  int bufferlens; // index in table
  int is_nulls;   // index in table

  MYSQL_STMT *my_stmt;
  conn_data *conn_data;
  int coref;
  int coref_count;
} st_data;

#define REF_CO(x)                              \
  if (x->coref == LUA_NOREF)                   \
  {                                            \
    lua_pushthread(L);                         \
    x->coref = luaL_ref(L, LUA_REGISTRYINDEX); \
    x->coref_count = 1;                        \
  }                                            \
  else                                         \
  {                                            \
    x->coref_count++;                          \
  }

#define UNREF_CO(x)                               \
  if (x->coref != LUA_NOREF)                      \
  {                                               \
    x->coref_count--;                             \
    if (x->coref_count == 0)                      \
    {                                             \
      luaL_unref(L, LUA_REGISTRYINDEX, x->coref); \
      x->coref = LUA_NOREF;                       \
    }                                             \
  }

static void wait_for_status(lua_State *L, conn_data *conn, void *data,
                            int status, event_callback_fn callback, int extra);

#define LUASQL_CONNECTION_MYSQL "MySQL connection"
#define LUASQL_STATEMENT_MYSQL "MySQL prepared statement"
#define LUASQL_CURSOR_MYSQL "MySQL cursor"

#define CONTINUE_YIELD -1

static void wait_for_status(lua_State *L, conn_data *cdata, void *data,
                            int status, event_callback_fn callback, int extra)
{
  struct maria_status *ms = malloc(sizeof(struct maria_status));
  ms->data = data;
  ms->L = L;
  ms->status = status;
  ms->conn_data = cdata;
  ms->extra = extra;

  short wait_event = 0;
  struct timeval tv, *ptv;
  int fd;

  if (status & MYSQL_WAIT_READ)
  {
    wait_event |= EV_READ;
  }
  if (status & MYSQL_WAIT_WRITE)
  {
    wait_event |= EV_WRITE;
  }
  if (wait_event)
    fd = mysql_get_socket(&cdata->my_conn);
  else
    fd = -1;
  if (status & MYSQL_WAIT_TIMEOUT)
  {
    tv.tv_sec = mysql_get_timeout_value(&cdata->my_conn);
    tv.tv_usec = 0;
    ptv = &tv;
  }
  else
    ptv = NULL;

  ms->event = event_new(event_mgr_base(), fd, wait_event, callback, ms);
  event_add(ms->event, ptv);
}

static conn_data *getconnection(lua_State *L)
{
  conn_data *conn = (conn_data *)luaL_checkudata(L, 1, LUASQL_CONNECTION_MYSQL);
  luaL_argcheck(L, conn != NULL, 1, "connection expected");
  luaL_argcheck(L, !conn->closed, 1, "connection is closed");
  return conn;
}

static int luamariadb_push_errno(lua_State *L, conn_data *conn)
{
  int errorcode = mysql_errno(&conn->my_conn);
  if (errorcode)
  {
    lua_pushnil(L);
    printf("mysql_error: %s\n", mysql_error(&conn->my_conn));
    lua_pushstring(L, mysql_error(&conn->my_conn));
    return 2;
  }
  else
  {
    return 0;
  }
}

#include "mariadb/luamariadb_stmt.c"
#include "mariadb/luamariadb_cursor.c"

#include "mariadb/luamariadb_close.c"
#include "mariadb/luamariadb_prepare.c"

#include "mariadb/luamariadb_ping.c"
#include "mariadb/luamariadb_query.c"
#include "mariadb/luamariadb_commit.c"
#include "mariadb/luamariadb_rollback.c"
#include "mariadb/luamariadb_autocommit.c"
#include "mariadb/luamariadb_setcharset.c"
#include "mariadb/luamariadb_connect.c"
/*
** Get Last auto-increment id generated
*/
LUA_API int conn_getlastautoid(lua_State *L)
{
  conn_data *conn = getconnection(L);
  lua_pushnumber(L, mysql_insert_id(&conn->my_conn));
  return 1;
}

LUA_API int conn_gc(lua_State *L)
{
  conn_data *conn = (conn_data *)luaL_checkudata(L, 1, LUASQL_CONNECTION_MYSQL);

  if (conn != NULL && !(conn->closed))
  {
    return conn_close_start(L);
  }

  return 0;
}

LUA_API int escape_string(lua_State *L)
{
  size_t size, new_size;
  conn_data *conn = getconnection(L);
  const char *from = luaL_checklstring(L, 2, &size);
  char *to;
  to = (char *)malloc(sizeof(char) * (2 * size + 1));
  if (to)
  {
    new_size = mysql_real_escape_string(&conn->my_conn, to, from, size);
    lua_pushlstring(L, to, new_size);
    free(to);
    return 1;
  }
  luaL_error(L, "could not allocate escaped string");
  return 0;
}

/*
** Create metatables for each class of object.
*/
static void create_metatables(lua_State *L)
{
  struct luaL_Reg connection_methods[] = {
      {"__gc", conn_gc},
      {"close", conn_close_start},
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
      {"send_long_data", st_send_long_data},
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
LUA_API int luaopen_fan_mariadb(lua_State *L)
{
  struct luaL_Reg driver[] = {
      {"connect", real_connect_start}, {NULL, NULL},
  };
  create_metatables(L);

  lua_newtable(L);
  luaL_setfuncs(L, driver, 0);

  lua_pushlightuserdata(L, &LONG_DATA);
  lua_setfield(L, -2, "LONG_DATA");

  return 1;
}
