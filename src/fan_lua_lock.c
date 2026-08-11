/*
 * fan_lua_lock.c — implementation of the cross-platform global lua_lock hook.
 * See fan_lua_lock.h for the rationale and build wiring.
 *
 * This file is compiled INTO the lua interpreter (added to CORE_O) so the
 * symbols are exported via -Wl,-E and resolvable by dlopen'd fan.so.
 */
#include <pthread.h>
#include <stdatomic.h>

#include "fan_lua_lock.h"

/* One process-wide mutex. Plain (non-recursive): the thread-local depth below
 * guarantees pthread_mutex_lock/unlock only run on the outermost lock/unlock
 * pair, so recursion never reaches the mutex. Matches lua-apple's luauser.c. */
static pthread_mutex_t g_lock;

/* Runtime switch: locking is off until worker threads are started. Off => the
 * lock path is a single atomic load + branch (effectively free) for
 * single-threaded (worker=0) runs. One-way latch: never turned back off. */
static atomic_int g_lua_locking_enabled = 0;

/* Thread-local nesting depth. Only the outermost lock/unlock pair operates the
 * real mutex; inner (nested) calls just adjust the counter. This makes the
 * lock resilient to a Lua longjmp skipping inner unlock calls — the outermost
 * unlock still releases the mutex. */
static _Thread_local int lua_lock_depth = 0;

/* Enable locking and initialise the mutex. Called by event_mgr_workers_init()
 * BEFORE any worker thread is spawned (i.e. still single-threaded here), so
 * initialising the mutex and flipping the switch need no extra synchronisation.
 * Idempotent: only the first call initialises. */
void LuaLockEnable(void) {
    if (atomic_load_explicit(&g_lua_locking_enabled, memory_order_relaxed)) {
        return;
    }
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
    atomic_store_explicit(&g_lua_locking_enabled, 1, memory_order_release);
}

static inline int locking_on(void) {
    return atomic_load_explicit(&g_lua_locking_enabled, memory_order_acquire);
}

void LockMainState(struct lua_State *L) {
    (void)L;
    if (locking_on()) {
        if (lua_lock_depth == 0) {
            pthread_mutex_lock(&g_lock);
        }
        lua_lock_depth++;
    }
}

void UnLockMainState(struct lua_State *L) {
    (void)L;
    if (locking_on()) {
        if (lua_lock_depth <= 0) {
            /* Defensive: unbalanced unlock; do not touch a mutex we do not hold. */
            return;
        }
        lua_lock_depth--;
        if (lua_lock_depth == 0) {
            pthread_mutex_unlock(&g_lock);
        }
    }
}

void LuaGlobalLock(void) {
    if (locking_on()) {
        if (lua_lock_depth == 0) {
            pthread_mutex_lock(&g_lock);
        }
        lua_lock_depth++;
    }
}

void LuaGlobalUnlock(void) {
    if (locking_on()) {
        if (lua_lock_depth <= 0) {
            return;
        }
        lua_lock_depth--;
        if (lua_lock_depth == 0) {
            pthread_mutex_unlock(&g_lock);
        }
    }
}

/* Fully release the lock around a blocking main-thread event loop so worker
 * threads can acquire it and run Lua callbacks. Returns the suspended depth so
 * the caller can restore it after the loop exits (keeping the enclosing
 * resume's trailing unlock balanced). No-op when nothing is held / locking off. */
int LuaLockSuspendForLoop(void) {
    int depth = lua_lock_depth;
    if (locking_on() && depth > 0) {
        lua_lock_depth = 0;
        pthread_mutex_unlock(&g_lock);
    }
    return depth;
}

void LuaLockResumeAfterLoop(int depth) {
    if (locking_on() && depth > 0) {
        pthread_mutex_lock(&g_lock);
        lua_lock_depth = depth;
    }
}

int  LuaLockDepthGet(void)        { return lua_lock_depth; }
void LuaLockDepthSet(int depth)   { lua_lock_depth = depth; }
