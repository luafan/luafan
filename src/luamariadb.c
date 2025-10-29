#define true 1
#define false 0

#include "mariadb/luamariadb_common.h"

// Global variable definition
int LONG_DATA = 0; // &LONG_DATA used as mariadb const.

void wait_for_status(lua_State *L, DB_CTX *ctx, void *data,
                     int status, event_callback_fn callback, int extra)
{
  DB_STATUS *bag = malloc(sizeof(DB_STATUS));
  bag->data = data;
  bag->L = L;
  bag->status = status;
  bag->ctx = ctx;
  bag->extra = extra;

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
    fd = mysql_get_socket(&ctx->my_conn);
  else
    fd = -1;
  if (status & MYSQL_WAIT_TIMEOUT)
  {
    tv.tv_sec = mysql_get_timeout_value(&ctx->my_conn);
    tv.tv_usec = 0;
    ptv = &tv;
  }
  else
    ptv = NULL;

  bag->event = event_new(event_mgr_base(), fd, wait_event, callback, bag);
  event_add(bag->event, ptv);
}

DB_CTX *getconnection(lua_State *L)
{
  DB_CTX *ctx = (DB_CTX *)luaL_checkudata(L, 1, MARIADB_CONNECTION_METATABLE);
  luaL_argcheck(L, ctx != NULL, 1, "connection expected");
  luaL_argcheck(L, !ctx->closed, 1, "connection is closed");
  return ctx;
}

int luamariadb_push_errno(lua_State *L, DB_CTX *ctx)
{
  int errorcode = mysql_errno(&ctx->my_conn);
  if (errorcode)
  {
    lua_pushnil(L);
    printf("mysql_error: %s\n", mysql_error(&ctx->my_conn));
    lua_pushstring(L, mysql_error(&ctx->my_conn));
    return 2;
  }
  else
  {
    return 0;
  }
}

// Include all module headers
#include "mariadb/luamariadb_stmt.h"
#include "mariadb/luamariadb_cursor.h"
#include "mariadb/luamariadb_close.h"
#include "mariadb/luamariadb_prepare.h"
#include "mariadb/luamariadb_ping.h"
#include "mariadb/luamariadb_query.h"
#include "mariadb/luamariadb_commit.h"
#include "mariadb/luamariadb_rollback.h"
#include "mariadb/luamariadb_autocommit.h"
#include "mariadb/luamariadb_setcharset.h"
#include "mariadb/luamariadb_connect.h"
/*
** Get Last auto-increment id generated
*/
LUA_API int conn_getlastautoid(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);
  lua_pushinteger(L, mysql_insert_id(&ctx->my_conn));
  return 1;
}

LUA_API int conn_gc(lua_State *L)
{
  DB_CTX *ctx = (DB_CTX *)luaL_checkudata(L, 1, MARIADB_CONNECTION_METATABLE);

  if (ctx != NULL && !(ctx->closed))
  {
    return conn_close_start(L);
  }

  return 0;
}

LUA_API int escape_string(lua_State *L)
{
  DB_CTX *ctx = getconnection(L);

  size_t size = 0;
  const char *from = luaL_checklstring(L, 2, &size);
  char *to = lua_newuserdata(L, 2 * size + 1);
  size_t new_size = mysql_real_escape_string(&ctx->my_conn, to, from, size);
  lua_pushlstring(L, to, new_size);
  return 1;
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
      {"store_result", stmt_store_result_start},
      {"fetch", stmt_fetch_start},
      {"pairs", st_pairs},
      {NULL, NULL},
  };

  luasql_createmeta(L, MARIADB_CONNECTION_METATABLE, connection_methods);
  luasql_createmeta(L, MARIADB_CURSOR_METATABLE, cursor_methods);
  luasql_createmeta(L, MARIADB_STATEMENT_METATABLE, statement_methods);
  lua_pop(L, 3);
}

/*
** Creates the metatables for the objects and registers the
** driver open method.
*/
LUA_API int luaopen_fan_mariadb(lua_State *L)
{
  struct luaL_Reg driver[] = {
      {"connect", real_connect_start},
      {NULL, NULL},
  };
  create_metatables(L);

  lua_newtable(L);
  luaL_setfuncs(L, driver, 0);

  lua_pushlightuserdata(L, &LONG_DATA);
  lua_setfield(L, -2, "LONG_DATA");

  return 1;
}
