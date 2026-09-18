#ifndef LUAMARIADB_COMMON_H
#define LUAMARIADB_COMMON_H

#include "../utlua.h"
#include "../luasql.h"
#include <mysql/mysql.h>
#include <pthread.h>

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
#define REF_CO(x) do {                             \
  lua_lock(L);                                     \
  if ((x)->coref == LUA_NOREF)                     \
  {                                                \
    lua_pushthread(L);                             \
    (x)->coref = luaL_ref(L, LUA_REGISTRYINDEX);   \
    (x)->coref_count = 1;                          \
  }                                                \
  else                                             \
  {                                                \
    (x)->coref_count++;                            \
  }                                                \
  lua_unlock(L);                                   \
} while(0)

#define UNREF_CO(x) do {                              \
  lua_lock(L);                                        \
  if ((x)->coref != LUA_NOREF)                        \
  {                                                   \
    (x)->coref_count--;                               \
    if ((x)->coref_count == 0)                        \
    {                                                 \
      luaL_unref(L, LUA_REGISTRYINDEX, (x)->coref);   \
      (x)->coref = LUA_NOREF;                         \
    }                                                 \
  }                                                   \
  lua_unlock(L);                                      \
} while(0)

/* Releases the object's reference to the parked coroutine, but only *after*
 * resuming it.
 *
 * The slot taken by REF_CO is the only strong reference to the suspended
 * coroutine, so it has to stay valid across FAN_RESUME: with the unref in front
 * of the resume another thread's GC could collect the coroutine and the resume
 * would continue on freed memory -- the very order R17 had to fix in the
 * WebSocket path. The resumed Lua code may free the object itself (connection,
 * cursor or statement), so the slot is detached from it *before* the resume and
 * unref'd from a local afterwards; never touch (x) after the resume. */
#define RESUME_AND_UNREF_CO(x, nresults) do {                 \
  int resume_held_coref = LUA_NOREF;                          \
  lua_lock(L);                                                \
  if ((x)->coref != LUA_NOREF)                                \
  {                                                           \
    (x)->coref_count--;                                       \
    if ((x)->coref_count == 0)                                \
    {                                                         \
      resume_held_coref = (x)->coref;                         \
      (x)->coref = LUA_NOREF;                                 \
    }                                                         \
  }                                                           \
  lua_unlock(L);                                              \
  FAN_RESUME(L, NULL, (nresults));                            \
  if (resume_held_coref != LUA_NOREF)                         \
  {                                                           \
    lua_lock(L);                                              \
    luaL_unref(L, LUA_REGISTRYINDEX, resume_held_coref);      \
    lua_unlock(L);                                            \
  }                                                           \
} while(0)

// Structure definitions
struct DB_STATUS;

/* Pending-wait context: everything a wait needs to be armed, claimed and
 * accounted for, allocated on the heap instead of living inside DB_CTX (see the
 * reference-count note in luamariadb.c). */
typedef struct
{
  pthread_mutex_t mutex;   // guards waits/state/in_flight, refs is atomic
  pthread_cond_t cond;     // signalled when in_flight drops (close drain)
  int refs;                // DB_CTX's reference plus one per armed/claimed wait
  int in_flight;           // claims whose continuation has not returned, all threads
  struct DB_STATUS *waits; // armed waits of one connection
} DB_PENDING;

typedef struct
{
  short closed;
  MYSQL my_conn;
  int coref;
  int coref_count;
  int worker_id;
  /* Async waits still armed for this connection, and the accounting the close
   * drain needs. The base of ctx->worker_id may be run by another worker thread,
   * so the arming thread, the dispatching loop thread and the closing thread all
   * touch this object. It is reference counted and owned by this userdata, but
   * outlives it while a claim is still in flight: the resumed coroutine is the
   * only strong reference to the connection, so the GC may finalize this
   * userdata before the dispatcher left its claim (see luamariadb.c). */
  DB_PENDING *pending;
} DB_CTX;

/* Pending wait, one per armed async operation. Owned by the arming thread until
 * wait_for_status() publishes it, then by whichever dispatcher claims it (see
 * wait_for_status_locked_cb / mariadb_cancel_pending_waits in luamariadb.c).
 * `ctx` is what the continuation needs while it runs; `pending` is what the
 * dispatcher is allowed to touch after it returned (the connection userdata may
 * be gone by then). The bag holds one reference on `pending` for as long as it
 * exists; the claim inherits it and the dispatcher releases it. */
typedef struct DB_STATUS
{
  lua_State *L;
  void *data;
  int status;
  struct event *event;
  event_callback_fn callback;
  DB_CTX *ctx;
  DB_PENDING *pending;    // reference-counted accounting (see DB_PENDING)
  int extra;
  int defer_retries;      // dispatches handed back until the coroutine yielded
  struct DB_STATUS *next; // next armed wait on pending->waits
  int state;              // WAIT_ARMED / WAIT_DISPATCHING while on pending->waits
} DB_STATUS;

/* Claim state of a DB_STATUS, guarded by DB_PENDING.mutex. A wait is only ever
 * claimed once, so the event's own dispatch and the close drain can never run
 * (and free) the same bag. */
enum
{
  WAIT_ARMED = 1,      // armed, published on pending->waits, nobody dispatched it
  WAIT_DISPATCHING = 2 // claimed by exactly one dispatcher, which owns the bag
};

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
/* Creates the pending-wait context of a connection (refs = 1, owned by it), and
 * takes/drops one reference on it. The last reference destroys the mutex, the
 * condition variable and the memory (see luamariadb.c). */
DB_PENDING *mariadb_pending_new(void);
DB_PENDING *mariadb_pending_ref(DB_PENDING *pending);
void mariadb_pending_unref(DB_PENDING *pending);
/* Registers the async wait event for `status` on the base of ctx->worker_id.
 * Returns 0 when the wait event is armed (the caller must yield), non-zero when
 * it could not be armed (the caller must restore its own coroutine instead of
 * yielding, otherwise the operation hangs forever).
 *
 * Arming and publishing the wait on ctx->pending is one critical section (the
 * pending context's mutex): the event base of ctx->worker_id may belong to
 * another worker thread, so from the moment event_add() returns that thread can
 * dispatch the wait, and the arming thread must not touch the bag after it. */
int wait_for_status(lua_State *L, DB_CTX *ctx, void *data, int status, event_callback_fn callback, int extra);
int mariadb_push_wait_error(lua_State *L);
/* Marks `ctx` closed under the pending context's mutex. Callers must do this
 * *before* mariadb_cancel_pending_waits(), so that no wait can be armed between
 * the drain and the mysql_close*() that releases the connection internals. */
void mariadb_close_begin(DB_CTX *ctx);
/* Aborts every async wait still armed for `ctx`, resuming each suspended call
 * with (nil, error). Callers must mark ctx closed first (mariadb_close_begin),
 * so no new wait can be armed while the list drains (see luamariadb_close.c). */
void mariadb_cancel_pending_waits(DB_CTX *ctx);

// Statement utility functions (implemented in luamariadb_stmt.c)
STMT_CTX *getstatement(lua_State *L);
void *get_or_create_ud(lua_State *L, int tableidx, int *ref, size_t size);
int luamariadb_push_stmt_error(lua_State *L, STMT_CTX *st);

// Cursor utility functions (implemented in luamariadb_cursor.c)
CURSOR_CTX *getcursor(lua_State *L);
int create_cursor(lua_State *L, DB_CTX *ctx, MYSQL_RES *result, int cols);

#endif // LUAMARIADB_COMMON_H