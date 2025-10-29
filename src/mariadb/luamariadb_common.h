#ifndef LUAMARIADB_COMMON_H
#define LUAMARIADB_COMMON_H

#include "../utlua.h"
#include "../luasql.h"
#include <mysql/mysql.h>

// Constants and macros
#define MARIADB_CONNECTION_METATABLE "MARIADB_CONNECTION_METATABLE"
#define MARIADB_STATEMENT_METATABLE "MARIADB_STATEMENT_METATABLE"
#define MARIADB_CURSOR_METATABLE "MARIADB_CURSOR_METATABLE"
#define CONTINUE_YIELD -1

// MySQL binding macros
#define MYSQL_SET_VARSTRING(bind, buff, length)  \
  {                                              \
    (bind)->buffer_type = MYSQL_TYPE_VAR_STRING; \
    (bind)->buffer = (buff);                     \
    (bind)->buffer_length = (length);            \
    (bind)->is_null = 0;                         \
  }

#define MYSQL_SET_LONGLONG(bind, buff)        \
  {                                            \
    (bind)->buffer_type = MYSQL_TYPE_LONGLONG; \
    (bind)->buffer = (buff);                   \
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

// Reference counting macros
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

// Structure definitions
typedef struct
{
  short closed;
  MYSQL my_conn;
  int coref;
  int coref_count;
} DB_CTX;

typedef struct
{
  lua_State *L;
  void *data;
  int status;
  struct event *event;
  DB_CTX *ctx;
  int extra;
} DB_STATUS;

typedef struct
{
  short closed;
  int numcols;            // number of columns
  int colnames, coltypes; // ref in registry
  MYSQL_RES *my_res;
  DB_CTX *ctx;
  int coref;
  int coref_count;
} CURSOR_CTX;

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
  DB_CTX *ctx;
  int coref;
  int coref_count;
} STMT_CTX;

// Global variables
extern int LONG_DATA;

// Core utility functions (implemented in luamariadb.c)
DB_CTX *getconnection(lua_State *L);
int luamariadb_push_errno(lua_State *L, DB_CTX *ctx);
void wait_for_status(lua_State *L, DB_CTX *ctx, void *data, int status, event_callback_fn callback, int extra);

// Statement utility functions (implemented in luamariadb_stmt.c)
STMT_CTX *getstatement(lua_State *L);
void *get_or_create_ud(lua_State *L, int tableidx, int *ref, size_t size);
int luamariadb_push_stmt_error(lua_State *L, STMT_CTX *st);

// Cursor utility functions (implemented in luamariadb_cursor.c)
CURSOR_CTX *getcursor(lua_State *L);
int create_cursor(lua_State *L, DB_CTX *ctx, MYSQL_RES *result, int cols);

#endif // LUAMARIADB_COMMON_H