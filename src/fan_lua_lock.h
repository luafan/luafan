/*
 * fan_lua_lock.h — cross-platform global lua_lock hook for luafan worker mode.
 *
 * WHY THIS EXISTS
 * ---------------
 * luafan can run its libevent loop across multiple worker threads
 * (fan.workers_init(n), n>0). All those threads share ONE lua_State. Stock Lua
 * defines `lua_lock(L)` / `lua_unlock(L)` as no-ops, so without a real lock the
 * worker threads race on the shared VM stack and crash (corrupted stack,
 * "attempt to call a <...> value", GC faults, etc.).
 *
 * On Apple platforms LuanMac uses lua-apple/lua53/luauser.c for this. This file
 * is the portable equivalent used to build the Linux/Alpine docker images.
 *
 * HOW TO WIRE IT UP (CRITICAL)
 * ----------------------------
 * `lua_lock` is baked in at COMPILE time of every Lua-core .c file. fan.so does
 * NOT link its own liblua — it resolves Lua (and these lock) symbols from the
 * host `lua` executable at dlopen time. Therefore BOTH must be built with this
 * header force-included, and the lock implementation must live in the lua
 * executable (exported via -Wl,-E, which the stock `linux` Make target sets):
 *
 *   1. Build the Lua interpreter from source:
 *        - add fan_lua_lock.c to the core objects (CORE_O)
 *        - MYCFLAGS='-include /path/to/fan_lua_lock.h -pthread'
 *        - link with -pthread (SYSLIBS already carry -Wl,-E on linux)
 *   2. Build fan.so with the SAME force-include so its lua_lock matches:
 *        luarocks make ... CFLAGS='... -include /path/to/fan_lua_lock.h -pthread'
 *      (LockMainState/UnLockMainState resolve to the interpreter's symbols.)
 *
 * RUNTIME BEHAVIOUR
 * -----------------
 * The mutex is only taken when locking is ENABLED, which happens exactly once,
 * inside event_mgr_workers_init() BEFORE the first worker thread is spawned
 * (via the weakly-referenced LuaLockEnable()). With no workers the whole thing
 * is a relaxed atomic load + branch — effectively free — so single-threaded
 * builds pay nothing. It is a one-way latch: once enabled it never turns off
 * within a process run, so no thread can observe a stale "disabled" state.
 */
#ifndef FAN_LUA_LOCK_H
#define FAN_LUA_LOCK_H

/* Tell luafan's utlua.c that a user lock header is present, so it uses the real
 * LuaLockDepthGet/Set below instead of its no-op fallback. We self-declare this
 * (rather than requiring -DLUA_USER_H on the command line) because passing a
 * quoted string macro survives poorly through the docker RUN -> sh -> luarocks
 * -> gcc layers. This header is force-included (-include fan_lua_lock.h) into
 * both the Lua core and fan.so, which is enough. */
#ifndef LUA_USER_H
#define LUA_USER_H "fan_lua_lock.h"
#endif

#include <pthread.h>

/* Route Lua's core lock macros to our functions. Guarded so an accidental
 * double force-include (e.g. via another config header) is harmless. */
#if !defined(lua_lock)
#define lua_lock(L)    LockMainState(L)
#endif
#if !defined(lua_unlock)
#define lua_unlock(L)  UnLockMainState(L)
#endif

struct lua_State;

/* Core lock/unlock, keyed on a lua_State (as Lua core calls them). */
void LockMainState(struct lua_State *L);
void UnLockMainState(struct lua_State *L);

/* State-less variants, used by luafan C code that only has the mutex context. */
void LuaGlobalLock(void);
void LuaGlobalUnlock(void);

/* Turn locking on. Called by event_mgr_workers_init() before spawning workers.
 * Idempotent. Weakly referenced by luafan so stock builds link cleanly. */
void LuaLockEnable(void);

/* Release/re-acquire the global lock around the blocking main-thread event loop
 * so worker threads can run Lua callbacks while the main thread is parked in
 * event_base_loop(). Referenced (weak) by luafan_start(). */
int  LuaLockSuspendForLoop(void);
void LuaLockResumeAfterLoop(int depth);

/* Depth accessors used by luafan's fan_cb_setup() to reconcile both the
 * thread-local depth and the recursive mutex ownership count. */
int  LuaLockDepthGet(void);
void LuaLockDepthSet(int depth);

#endif /* FAN_LUA_LOCK_H */
