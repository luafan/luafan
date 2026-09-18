#include "mariadb/luamariadb_common.h"
#include "mariadb/luamariadb_close.h"

// Global variable definition
int LONG_DATA = 0; // &LONG_DATA used as mariadb const.

/*
** Pending waits (DB_PENDING)
**
** Every async wait is published on its connection's pending list so that closing
** the connection can abort it, and that list is touched by up to three threads
** at once: the coroutine arming the wait (on the base of the connection's worker
** affinity), the thread running that base's loop (which dispatches the wait) and
** the thread closing the connection (which drains the list). The pending
** context's mutex and condition variable serialise all of them:
**
**   - wait_for_status() holds the mutex across "re-check ctx->closed, read the
**     socket/timeout, event_new, event_add and publish", so a published wait is
**     always fully armed and the arming thread never touches the bag once the
**     event can fire (the dispatching thread blocks in the claim until then).
**   - A wait is claimed exactly once, by the event's own dispatch
**     (wait_for_status_locked_cb) or by the close drain
**     (mariadb_cancel_pending_waits). The loser drops the bag without touching
**     it, so no continuation is run or freed twice.
**   - A claim counts as in_flight until its continuation returns, so the drain
**     can wait for a continuation another thread is running before it lets
**     mysql_close*() tear the MYSQL down.
**   - A dispatch only resumes a wait whose coroutine really is suspended; one
**     that fired too early is handed back to its loop as a timer while keeping
**     its claim (see "Deferred dispatch" near wait_for_status_locked_cb).
**
** The context itself lives outside DB_CTX and is reference counted, because the
** userdata's lifetime belongs to the Lua GC while a claim can outlive it: the
** dispatch callback resumes the suspended coroutine, which is the only strong
** reference to the connection, so the GC may collect and finalize the userdata
** before that callback has finished accounting for its claim. Each armed wait
** holds a reference (transferred to its claimer and released after the
** continuation returned) and DB_CTX holds one of its own; the last owner
** destroys the mutex and the condition variable. A dispatcher therefore touches
** only the pending context after its continuation returned -- never the
** connection.
*/

/* Number of wait continuations this thread is running. A claim is always
 * followed by exactly one lua_lock/callback/lua_unlock run and a thread cannot
 * be inside two of them at once, so this is 0 or 1. The close drain subtracts
 * it from pending->in_flight: a close issued from inside a continuation must not
 * wait for itself. */
static _Thread_local int wait_dispatch_depth = 0;

/* The Lua-lock hand-off pair (declared by the force-included fan_lua_lock.h).
 * Weak linkage keeps builds that do not wire the hook valid, exactly as in
 * event_mgr.c. */
#if !defined(FAN_LUA_LOCK_WIRED)
#if defined(__clang__)
#define MARIA_LOCK_WEAK_IMPORT __attribute__((weak_import))
#else
#define MARIA_LOCK_WEAK_IMPORT __attribute__((weak))
#endif
MARIA_LOCK_WEAK_IMPORT int LuaLockSuspendForLoop(void);
MARIA_LOCK_WEAK_IMPORT void LuaLockResumeAfterLoop(int depth);
#endif

#if defined(FAN_LUA_LOCK_WIRED)
#define MARIA_LOCK_SUSPEND()   (LuaLockSuspendForLoop())
#define MARIA_LOCK_RESUME(d)   LuaLockResumeAfterLoop(d)
#else
#define MARIA_LOCK_SUSPEND()   (LuaLockSuspendForLoop ? LuaLockSuspendForLoop() : 0)
#define MARIA_LOCK_RESUME(d)   do { if (LuaLockResumeAfterLoop) LuaLockResumeAfterLoop(d); } while (0)
#endif

/* The pending context is reference counted (see the file header): DB_CTX owns
 * one reference, every armed or claimed wait owns one. */
DB_PENDING *mariadb_pending_new(void)
{
  DB_PENDING *pending = malloc(sizeof(DB_PENDING));
  if (!pending)
  {
    return NULL;
  }
  pthread_mutex_init(&pending->mutex, NULL);
  pthread_cond_init(&pending->cond, NULL);
  pending->refs = 1; /* the creating DB_CTX's reference */
  pending->in_flight = 0;
  pending->waits = NULL;
  return pending;
}

DB_PENDING *mariadb_pending_ref(DB_PENDING *pending)
{
  if (pending != NULL)
  {
    __atomic_add_fetch(&pending->refs, 1, __ATOMIC_RELAXED);
  }
  return pending;
}

/* Drops one reference. The last one destroys the synchronisation primitives and
 * releases the memory, so callers must not touch `pending` afterwards. */
void mariadb_pending_unref(DB_PENDING *pending)
{
  if (pending == NULL)
  {
    return;
  }
  if (__atomic_sub_fetch(&pending->refs, 1, __ATOMIC_ACQ_REL) > 0)
  {
    return;
  }
  pthread_mutex_destroy(&pending->mutex);
  pthread_cond_destroy(&pending->cond);
  free(pending);
}

/* Detach a wait from the connection's pending list. pending->mutex must be held
 * by the caller. */
static void wait_unlink_locked(DB_PENDING *pending, DB_STATUS *bag)
{
  DB_STATUS **pp = &pending->waits;

  while (*pp != NULL)
  {
    if (*pp == bag)
    {
      *pp = bag->next;
      bag->next = NULL;
      return;
    }
    pp = &(*pp)->next;
  }
}

/* Take exclusive ownership of an armed wait. pending->mutex must be held.
 * Returns 1 for the dispatcher that is now the only one allowed to run (and
 * free) the bag, 0 when the other dispatcher claimed it first. The claim also
 * counts the bag as in_flight until wait_in_flight_leave(), and takes over the
 * bag's reference on `pending`. */
static int wait_claim(DB_PENDING *pending, DB_STATUS *bag)
{
  if (bag->state != WAIT_ARMED)
  {
    return 0;
  }
  bag->state = WAIT_DISPATCHING;
  wait_unlink_locked(pending, bag);
  pending->in_flight++;
  return 1;
}

/* Release a claim taken by wait_claim(). Also valid after the bag was freed:
 * only the pending context is touched. */
static void wait_in_flight_leave(DB_PENDING *pending)
{
  pthread_mutex_lock(&pending->mutex);
  if (pending->in_flight > 0)
  {
    pending->in_flight--;
  }
  pthread_cond_broadcast(&pending->cond);
  pthread_mutex_unlock(&pending->mutex);
}

/* ---- Deferred dispatch (R22) --------------------------------------------
 *
 * A wait event can fire before the coroutine that armed it has reached its
 * lua_yield(): on the hooked shape the core drops the Lua lock around every C
 * function call (`luaD_precall` in `lua53/ldo.c`), and that C call is exactly
 * "arm the event, then yield" (`real_query_start()` in mariadb/
 * luamariadb_query.c and every other *_start). The thread owning `base` may
 * therefore dispatch the wait while the arming thread is still running it.
 * lua_resume() refuses that ("cannot resume non-suspended coroutine",
 * `resume()` in `lua53/ldo.c`) and the continuation would be consumed without
 * anybody ever resuming the suspended call: the request hangs forever
 * (measured ~7 times in 150 multi-worker runs of tests/lua/
 * test_mariadb_workers.lua, each as one client-side timeout).
 *
 * So dispatch a wait only when its coroutine really is suspended: lua_yieldk()
 * sets `L->status = LUA_YIELD` under the Lua lock and lua_resume() keeps every
 * further change of the state machine before it releases that same lock again
 * (`lua53/ldo.c`), so under the lock the test `lua_status(L) == LUA_YIELD` is
 * both necessary and race-free -- LUA_OK covers "fresh" and "running" alike,
 * anything above LUA_YIELD means "dead".
 *
 * A deferred dispatch keeps its claim (the bag stays exclusively ours, and it
 * stays counted as in_flight, so a close still waits for it) and hands the wait
 * back to its own loop as a pure timer: the socket that made it ready is
 * dropped, because a level-triggered fd would re-fire immediately and spin the
 * loop instead of letting the arming thread finish. The retries are bounded --
 * a status that never becomes LUA_YIELD (dead coroutine, e.g. a yield that
 * raised instead of suspending) falls through to the old behaviour, so the
 * claim is always released and the connection can still be closed.
 *
 * On a stock core the lock comes from event_mgr's resume wrapper
 * (locking_resume()), which holds it across the whole lua_resume(), so the
 * dispatcher's lua_lock() blocks until the resume returned -- i.e. until after
 * the yield -- and the test always reads LUA_YIELD here. */
#define WAIT_DEFER_FAST_USEC 1000 /* 1 ms: the arming thread was preempted */
#define WAIT_DEFER_FAST_TRIES 50
#define WAIT_DEFER_SLOW_USEC 5000 /* 5 ms: it is doing something long */
#define WAIT_DEFER_MAX_TRIES (WAIT_DEFER_FAST_TRIES + 200)

static void wait_dispatch(DB_STATUS *bag, int fd, short event, int claimed);
static void wait_defer_retry_cb(int fd, short event, void *_userdata);

/* Hand a claimed wait back to its own loop as a pure timer. Only the owner of
 * the bag (the thread that claimed it) calls this, from the loop that runs the
 * bag's base; the claim is kept across the whole deferral, so the bag stays
 * exclusively ours and nobody else can run or free it meanwhile.
 *
 * The retry is a one-shot timer of its own (event_base_once), not the bag's
 * socket event re-armed in place: that event is still being dispatched by this
 * very callback, and re-assigning an active event is not allowed. Re-adding the
 * socket event is no option either -- a level-triggered fd would fire again
 * immediately and spin the loop instead of letting the arming thread finish.
 * Returns 0 when the retry is armed. */
static int wait_defer_retry(DB_STATUS *bag)
{
  struct event_base *base = event_get_base(bag->event);
  if (base == NULL)
  {
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = (bag->defer_retries < WAIT_DEFER_FAST_TRIES) ? WAIT_DEFER_FAST_USEC
                                                            : WAIT_DEFER_SLOW_USEC;
  if (event_base_once(base, -1, EV_TIMEOUT, wait_defer_retry_cb, bag, &tv) != 0)
  {
    return -1;
  }
  bag->defer_retries++;
  return 0;
}

/* Dispatcher shared by the socket event and the deferral timer. `claimed` tells
 * whether this dispatch still has to take the claim: the socket event does, a
 * deferred retry already holds it -- the claim lives across the whole deferral,
 * so claiming again would see WAIT_DISPATCHING and drop the wait forever. */
static void wait_dispatch(DB_STATUS *bag, int fd, short event, int claimed)
{
  DB_PENDING *pending = bag->pending;

  if (pending == NULL)
  {
    return;
  }

  if (!claimed)
  {
    /* Claim the wait *before* taking the Lua lock. If a close claimed it first,
     * that close may free the bag (and its event) as soon as we are out of its
     * way, so a losing dispatch must not touch the bag at all -- not even to read
     * its lua_State. */
    pthread_mutex_lock(&pending->mutex);
    claimed = wait_claim(pending, bag);
    pthread_mutex_unlock(&pending->mutex);
    if (!claimed)
    {
      return;
    }
  }

  /* Safe now: this thread is the only owner of the bag. The continuation
   * resumes the suspended call, runs the next mysql_*_cont() step or re-arms --
   * and frees the bag (with its event) on its way out, so only `pending` may be
   * used below. */
  lua_State *L = bag->L;
  lua_lock(L);

  /* Never resume a coroutine that has not suspended itself yet: hand the wait
   * back to the loop and come back once it did (see "Deferred dispatch"). */
  if (lua_status(L) != LUA_YIELD)
  {
    if (bag->defer_retries < WAIT_DEFER_MAX_TRIES && wait_defer_retry(bag) == 0)
    {
      lua_unlock(L);
      return;
    }
    LOGE("mariadb: wait dispatched while its coroutine is not suspended "
         "(status=%d, deferred=%d); resuming it anyway\n",
         lua_status(L), bag->defer_retries);
  }

  wait_dispatch_depth++;
  bag->callback(fd, event, bag);
  lua_unlock(L);
  wait_dispatch_depth--;

  /* The continuation dropped its own references, and the resumed coroutine was
   * the only thing keeping the connection alive: the GC may have collected (and
   * finalized) the userdata by now. That is why the claim carries a reference on
   * the pending context -- leave the claim and release it without ever touching
   * DB_CTX again. */
  wait_in_flight_leave(pending);
  mariadb_pending_unref(pending);
}

static void wait_for_status_locked_cb(int fd, short event, void *_userdata)
{
  wait_dispatch((DB_STATUS *)_userdata, fd, event, 0);
}

static void wait_defer_retry_cb(int fd, short event, void *_userdata)
{
  wait_dispatch((DB_STATUS *)_userdata, fd, event, 1);
}

int mariadb_push_wait_error(lua_State *L)
{
  lua_pushnil(L);
  lua_pushliteral(L, "async wait could not be armed (connection closed or "
                     "event registration failed)");
  return 2;
}

int wait_for_status(lua_State *L, DB_CTX *ctx, void *data,
                    int status, event_callback_fn callback, int extra)
{
  if (ctx == NULL || ctx->pending == NULL)
  {
    return -1;
  }

  short wait_event = 0;
  struct timeval tv;
  struct timeval *ptv = NULL;
  int fd = -1;
  DB_PENDING *pending = ctx->pending;

  if (status & MYSQL_WAIT_READ)
  {
    wait_event |= EV_READ;
  }
  if (status & MYSQL_WAIT_WRITE)
  {
    wait_event |= EV_WRITE;
  }

  struct event_base *base = event_mgr_base();
  if (ctx->worker_id >= 0 && event_mgr_worker_count() > 0)
  {
    base = event_mgr_worker_base(ctx->worker_id);
  }

  /* One critical section for "closed?", reading the socket and timeout out of
   * the MYSQL, arming the event and publishing the wait:
   *
   *  - mariadb_close_begin() and mariadb_cancel_pending_waits() take the same
   *    mutex, so this either arms a wait the drain will see, or is refused once
   *    the connection is closed. It can never leave a wait armed for a
   *    mysql_close*() that is already on its way, and mysql_close*() cannot run
   *    while the socket/timeout are read here.
   *  - once event_add() has returned, the thread owning `base` may dispatch the
   *    event at any moment. Publishing under the mutex makes that dispatch wait
   *    for this critical section (wait_for_status_locked_cb claims under the
   *    same mutex), so the arming thread never touches the bag after unlocking:
   *    the dispatch either blocks until the wait is complete or has not been
   *    entered yet.
   *  - the bag holds a reference on `pending` for as long as it exists (see
   *    DB_PENDING): the claim inherits it, so the dispatching thread can update
   *    the in-flight count after its continuation returned even if the GC
   *    collected this connection meanwhile. */
  pthread_mutex_lock(&pending->mutex);
  // Refuse to schedule new wait events on a closed context, except for the
  // connection close state machine, which marks ctx closed before yielding.
  if (ctx->closed && callback != conn_close_cont)
  {
    pthread_mutex_unlock(&pending->mutex);
    return -1;
  }

  DB_STATUS *bag = malloc(sizeof(DB_STATUS));
  if (!bag)
  {
    pthread_mutex_unlock(&pending->mutex);
    return -1;
  }
  bag->data = data;
  bag->L = L;
  bag->status = status;
  bag->event = NULL;
  bag->callback = callback;
  bag->ctx = ctx;
  bag->pending = mariadb_pending_ref(pending);
  bag->extra = extra;
  bag->next = NULL;
  bag->state = WAIT_ARMED;
  bag->defer_retries = 0;

  if (wait_event)
  {
    fd = mysql_get_socket(&ctx->my_conn);
  }
  if (status & MYSQL_WAIT_TIMEOUT)
  {
    tv.tv_sec = mysql_get_timeout_value(&ctx->my_conn);
    tv.tv_usec = 0;
    ptv = &tv;
  }

  bag->event = event_new(base, fd, wait_event, wait_for_status_locked_cb, bag);
  if (!bag->event)
  {
    /* Nothing can dispatch an event that was never created or added, so the
     * bag is still private here. */
    mariadb_pending_unref(bag->pending);
    free(bag);
    pthread_mutex_unlock(&pending->mutex);
    return -1;
  }
  if (event_add(bag->event, ptv) != 0)
  {
    event_free(bag->event);
    mariadb_pending_unref(bag->pending);
    free(bag);
    pthread_mutex_unlock(&pending->mutex);
    return -1;
  }

  // Publish the armed wait so a close can abort it (see
  // mariadb_cancel_pending_waits), still holding the mutex: the bag is complete
  // and armed, so the dispatch thread may take it over as soon as we unlock.
  bag->next = pending->waits;
  pending->waits = bag;
  pthread_mutex_unlock(&pending->mutex);
  return 0;
}

/*
** Abort every async wait still armed for `ctx`.
**
** Called by the close state machine after ctx->closed has been set and *before*
** mysql_close_start() tears the MYSQL down. Without this, a continuation parked
** on a socket event would still be delivered by libevent after mysql_close()
** released the connection internals, and would re-enter mysql_real_query_cont()
** / mysql_stmt_*_cont() on freed memory -- a use-after-free that is
** deterministically reproducible by closing a connection while a
** "SELECT SLEEP(5)" is still in flight.
**
** Each pending continuation is replayed once, by *this* drain only: a wait is
** claimed under the pending context's mutex first, so a wait whose event was
** already dispatched by the connection's worker thread is left to that
** dispatch and waited for (both
** resume the coroutine with an error, exactly once). Since ctx->closed is now
** set, the replayed continuation's attempt to re-arm fails -- wait_for_status()
** refuses closed contexts -- so it takes its existing "cannot park on an event"
** branch: it releases the coroutine reference for *its own* object type
** (connection, cursor or statement, which is why the bag cannot simply call
** UNREF_CO itself), resumes the suspended Lua call with (nil, error) and frees
** the bag. Replaying is safe because the connection is still open here: the
** *_cont call only steps libmariadb's non-blocking state machine and returns
** the same MYSQL_WAIT_* mask instead of blocking on the socket.
**
** The drain runs on whatever thread executes the close call, which is *not*
** necessarily the thread that owns ctx's event base (a round-robin connection
** issued from a worker handler lives on another worker's base), so it does not
** assume base ownership: the claim and wait_for_foreign_continuations() make it
** safe instead.
*/
/* Marks the connection closed under the pending context's mutex (see
 * luamariadb_common.h). Doing it under the same mutex wait_for_status() holds
 * while it checks the flag and publishes makes "closed" an atomic switch for
 * the arming path: an arm either completes before the drain (and is seen by it)
 * or is refused. */
void mariadb_close_begin(DB_CTX *ctx)
{
  if (ctx == NULL)
  {
    return;
  }
  if (ctx->pending == NULL)
  {
    ctx->closed = 1;
    return;
  }
  pthread_mutex_lock(&ctx->pending->mutex);
  ctx->closed = 1;
  pthread_mutex_unlock(&ctx->pending->mutex);
}

/* Wait until every continuation that another thread claimed has finished. Those
 * continuations run against this still-open connection and need the Lua lock to
 * reach their end, which the closing thread may be holding (stock-interpreter
 * shape), so hand the lock over while waiting -- the depth-symmetric hand-off
 * event_mgr_loop() uses. A continuation this thread is running itself (the
 * close may be issued from inside one) is not waited for: wait_dispatch_depth
 * is subtracted from the count. */
static void wait_for_foreign_continuations(DB_PENDING *pending)
{
  pthread_mutex_lock(&pending->mutex);
  if (pending->in_flight > wait_dispatch_depth)
  {
    int lock_depth = MARIA_LOCK_SUSPEND();
    while (pending->in_flight > wait_dispatch_depth)
    {
      pthread_cond_wait(&pending->cond, &pending->mutex);
    }
    MARIA_LOCK_RESUME(lock_depth);
  }
  pthread_mutex_unlock(&pending->mutex);
}

void mariadb_cancel_pending_waits(DB_CTX *ctx)
{
  if (ctx == NULL || ctx->pending == NULL)
  {
    return;
  }

  /* Hold a reference for the whole drain: replaying a wait below hands its own
   * reference over to this function and drops it again, so without this the
   * pending context could die in the middle of the loop (it would also be safe
   * while the caller keeps `ctx` alive, but the drain must not depend on that).
   * All uses of the bag are covered by the bag's own reference, transferred by
   * wait_claim(). */
  DB_PENDING *pending = mariadb_pending_ref(ctx->pending);

  for (;;)
  {
    pthread_mutex_lock(&pending->mutex);
    DB_STATUS *bag = pending->waits;
    if (bag == NULL)
    {
      pthread_mutex_unlock(&pending->mutex);
      break;
    }

    /* conn_close_cont is the one continuation that may arm a wait on a closed
     * connection, so replaying it here would just re-arm in a loop. It is never
     * queued when this runs (conn_close_start cancels before arming it), so
     * treat its presence as a bug and stop instead of spinning. */
    if (bag->callback == conn_close_cont)
    {
      LOGE("mariadb: close continuation queued before pending waits drained\n");
      pthread_mutex_unlock(&pending->mutex);
      break;
    }

    /* Claim the wait while the list is locked: if its event was dispatched by
     * the connection's worker thread first, that dispatch owns the bag and
     * resumes its coroutine (which sees the closed context and fails its next
     * arm), so this drain must leave the bag alone. */
    if (!wait_claim(pending, bag))
    {
      pthread_mutex_unlock(&pending->mutex);
      continue;
    }
    pthread_mutex_unlock(&pending->mutex);

    /* We are now the only dispatcher of this bag. Replaying the continuation
     * runs it against the still-open connection: it fails to re-arm (closed
     * context) and takes its "cannot park on an event" branch -- resume with
     * (nil, error), release the coroutine reference for its own object type,
     * free the bag and its event. Keeping the replay here (instead of letting
     * the event fire later) is what keeps the connection alive until every
     * pending continuation is done, which is what mysql_close*() needs. */
    lua_State *L = bag->L;
    lua_lock(L);
    bag->callback(0, 0, bag);
    lua_unlock(L);
    wait_in_flight_leave(pending);
    mariadb_pending_unref(pending);
  }

  /* A continuation that another thread dispatched before this close started may
   * still be running; mysql_close_start() must not tear the MYSQL down under
   * it. */
  wait_for_foreign_continuations(pending);
  mariadb_pending_unref(pending);
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
    LOGE("mysql_error: %s\n", mysql_error(&ctx->my_conn));
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

  if (ctx == NULL)
  {
    return 0;
  }

  if (!(ctx->closed))
  {
    /* A finalizer cannot yield. Close synchronously so no async event keeps
     * a pointer to this userdata after GC has started reclaiming it. No wait
     * can be pending here either: an armed wait holds a registry reference to
     * the suspended coroutine, whose stack still holds this connection, so the
     * userdata cannot become garbage while a wait is still armed. */
    mariadb_close_begin(ctx);
    mysql_close(&ctx->my_conn);
  }

  /* Give up this connection's reference on the pending-wait context. A
   * continuation another thread claimed may still be running: it holds its own
   * reference (and only touches the pending context, never this userdata), so
   * the accounting survives until it leaves, and the last owner tears the mutex
   * and the condition variable down (mariadb_pending_unref). */
  if (ctx->pending != NULL)
  {
    mariadb_pending_unref(ctx->pending);
    ctx->pending = NULL;
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
