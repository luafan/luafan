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

/* One process-wide recursive mutex. The thread-local depth mirrors the
 * recursive ownership count so longjmp recovery can restore both states. */
static pthread_mutex_t g_lock;

/* Runtime switch: locking is off until worker threads are started. Off => the
 * lock path is a single atomic load + branch (effectively free) for
 * single-threaded (worker=0) runs. One-way latch: never turned back off. */
static atomic_int g_lua_locking_enabled = 0;

/* Thread-local nesting depth. It mirrors the recursive mutex ownership count;
 * every successful lock has a matching real unlock. */
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
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
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
        pthread_mutex_lock(&g_lock);
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
        pthread_mutex_unlock(&g_lock);
        lua_lock_depth--;
    }
}

void LuaGlobalLock(void) {
    if (locking_on()) {
        pthread_mutex_lock(&g_lock);
        lua_lock_depth++;
    }
}

void LuaGlobalUnlock(void) {
    if (locking_on()) {
        if (lua_lock_depth <= 0) return;
        pthread_mutex_unlock(&g_lock);
        lua_lock_depth--;
    }
}

/* Fully release every recursive level around a blocking event loop. */
int LuaLockSuspendForLoop(void) {
    int depth = lua_lock_depth;
    if (locking_on()) {
        while (lua_lock_depth > 0) {
            pthread_mutex_unlock(&g_lock);
            lua_lock_depth--;
        }
    }
    return depth;
}

void LuaLockResumeAfterLoop(int depth) {
    if (depth < 0) depth = 0;
    if (locking_on()) {
        while (lua_lock_depth < depth) {
            pthread_mutex_lock(&g_lock);
            lua_lock_depth++;
        }
    }
}

int LuaLockDepthGet(void) { return lua_lock_depth; }

/* Restore both the TLS mirror and the recursive mutex's actual count. */
void LuaLockDepthSet(int depth) {
    if (depth < 0) depth = 0;
    if (locking_on()) {
        while (lua_lock_depth > depth) {
            pthread_mutex_unlock(&g_lock);
            lua_lock_depth--;
        }
        while (lua_lock_depth < depth) {
            pthread_mutex_lock(&g_lock);
            lua_lock_depth++;
        }
    } else {
        // Locking is disabled, so no real mutex ownership exists to restore.
        lua_lock_depth = 0;
    }
}
