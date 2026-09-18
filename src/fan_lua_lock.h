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

/* Marker for build wiring checks: defined when this header is force-included
 * (-include fan_lua_lock.h), i.e. when the build followed the wiring described
 * below. event_mgr.c uses it to decide whether the state-less lock functions
 * need weak fallback declarations. */
#define FAN_LUA_LOCK_WIRED 1

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

/* ---- R18: close the checkGC() yield window -------------------------------- *
 *
 * Routing lua_lock/lua_unlock to a real mutex (above) turns one core yield point
 * into a genuine window:
 *
 *     #define checkGC(L,c) \
 *         { luaC_condGC(L, L->top = (c), Protect(L->top = ci->top)); \
 *            luai_threadyield(L); }
 *
 * Protect() re-derives the interpreter's local frame base after luaC_step(), but
 * the luai_threadyield() that follows it is NOT wrapped in Protect. In stock Lua
 * that gap is harmless -- nothing else can run inside it -- while on a hooked
 * core another worker can take the lock there and run a whole GC whose mark
 * phase traverses this very coroutine (lgc.c: traversethread() ->
 * luaD_shrinkstack()) and reallocates, i.e. moves, its stack block.
 * correctstack() fixes L->top / ci->func / ci->u.l.base, but the interpreter's
 * *local* `base` (and every `ra` derived from it) keeps pointing into the freed
 * block, so the next instruction that touches a register reads or writes freed
 * memory (ASan: heap-use-after-free in luaV_execute/luaD_precall). The window is
 * reachable only on a hooked core: with the wrapper shape the mutex is held
 * across a whole resume segment, which makes luai_threadyield a pair of no-ops.
 *
 * Re-deriving `base` after re-locking closes the window without giving up the
 * yield point: the unlock stays real, so the property that
 * tests/lua/test_lock_granularity.lua asserts (a worker in an allocating loop
 * must not freeze the main thread) is preserved. The macro expands in exactly
 * one place -- checkGC() inside luaV_execute(), where `base` and `ci` are
 * locals, the same invariant Protect() relies on -- and llimits.h defines its
 * own version only `#if !defined(luai_threadyield)`, so this one wins. If a
 * future version used luai_threadyield elsewhere, this would be a loud compile
 * error rather than silent corruption.
 *
 * The expression that recomputes the base differs per Lua version, and this
 * header is force-included BEFORE lua.h (so LUA_VERSION_NUM does not exist yet,
 * and including lua.h from here would freeze luaconf.h's LUA_CORE-only
 * definitions), so the build selects it:
 *
 *     5.3.x   (default; every build today)   base = ci->u.l.base
 *     5.4.6+  -DFAN_LUA_STACK_BASE_54=1      base = ci->func.p + 1
 *     other   -DFAN_LUA_BASE_EXPR='...'      explicit expression (quote it)
 *
 * A mismatch is a compile error (the field does not exist in the other version),
 * never silent corruption. tests/build_hooked_lua.sh derives the flag from the
 * Lua version it builds. -DFAN_LUA_THREADYIELD_FIX=0 disables the override, to
 * reproduce R18. See docs/threading-model.md (R18). */
#if !defined(FAN_LUA_THREADYIELD_FIX)
#define FAN_LUA_THREADYIELD_FIX 1
#endif

#if FAN_LUA_THREADYIELD_FIX && !defined(luai_threadyield)
#  if defined(FAN_LUA_BASE_EXPR)
#    define luai_threadyield(L)  { lua_unlock(L); lua_lock(L); \
                                   base = FAN_LUA_BASE_EXPR; }
#  elif defined(FAN_LUA_STACK_BASE_54)
#    define luai_threadyield(L)  { lua_unlock(L); lua_lock(L); \
                                   base = (ci)->func.p + 1; }
#  else
#    define luai_threadyield(L)  { lua_unlock(L); lua_lock(L); \
                                   base = (ci)->u.l.base; }
#  endif
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

/* BUILD ROLE MARKER ------------------------------------------------ *
 * Pass -DFAN_LUA_LOCK_CORE=1 when this file is compiled INTO the Lua
 * interpreter (added to CORE_O): LuaCoreLockHooked() then reports 1, which is
 * how fan.so learns that the running interpreter already serialises every
 * resume and therefore does not need event_mgr's locking resume wrapper
 * (see install_locking_resume() in event_mgr.c).
 *
 * When this file is compiled into fan.so instead (stock interpreter; CMake
 * build), the marker is absent and LuaCoreLockHooked() reports 0 -- i.e. the
 * conservative answer, "assume no core hook, install the wrapper".
 *
 * Both roles must NEVER be combined in one process: two copies would mean two
 * mutexes and two thread-local depth counters. The CMake build enforces that by
 * excluding this file from fan.so when it assumes a hooked core, and
 * fan_lua_lock.c refuses to compile if both markers are set. */
int LuaCoreLockHooked(void);

/* Depth accessors used by luafan's fan_cb_setup() to reconcile both the
 * thread-local depth and the recursive mutex ownership count. */
int  LuaLockDepthGet(void);
void LuaLockDepthSet(int depth);

#endif /* FAN_LUA_LOCK_H */
