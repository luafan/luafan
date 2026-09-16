
#ifndef event_mgr_h
#define event_mgr_h

#include <event.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <stdio.h>

struct event_base *event_mgr_base(void);
struct event_base *event_mgr_base_current(void);

struct evdns_base *event_mgr_dnsbase(void);
void event_mgr_break(void);
int event_mgr_init(void);
void event_mgr_cleanup(void);
int event_mgr_loop(void);
int event_mgr_loop_later_cleanup(void);
void event_mgr_loop_cleanup(void);
int event_mgr_is_looping(void);

// Worker pool for multi-threaded event processing
#define EVENT_MGR_MAX_WORKERS 16
#define EVENT_MGR_DEFAULT_WORKERS 8

int event_mgr_workers_init(int num_workers);
void event_mgr_workers_shutdown(void);
void event_mgr_workers_stop_threads(void);
void event_mgr_workers_free_bases(void);
struct event_base *event_mgr_worker_base(int worker_id);
struct evdns_base *event_mgr_worker_dnsbase(int worker_id);
int event_mgr_worker_once(int worker_id, event_callback_fn callback, void *arg);
int event_mgr_worker_once_delay(int worker_id, event_callback_fn callback, void *arg, long delay_ms);
/* Framework hand-off dispatch: same contract, but it stays usable while the
 * pool is shutting down (the user-level dispatchers above are gated by
 * workers_accepting_dispatch). Use it only for internal continuations that own
 * the target base and are drained by event_mgr_drain_internal_jobs() during
 * loop shutdown — see the implementation notes in event_mgr.c. */
int event_mgr_worker_once_internal(int worker_id, event_callback_fn callback, void *arg);
int event_mgr_worker_once_internal_delay(int worker_id, event_callback_fn callback, void *arg, long delay_ms);
int event_mgr_is_current_owner(int worker_id);
int event_mgr_is_loop_running(void);
int event_mgr_next_worker(void);
int event_mgr_worker_count(void);
int event_mgr_current_worker_id(void);

/* How the global Lua lock is currently provided (see docs/threading-model.md).
 * Reported by fan.diag_lock_mode() and asserted by tests/lua/test_lock_granularity.lua:
 * the lock must come from the interpreter core when it was built with the hook,
 * and from luafan's resume wrapper otherwise. */
enum {
    FAN_LUA_LOCK_MODE_NONE = 0,      /* no lock implementation linked at all */
    FAN_LUA_LOCK_MODE_SINGLE = 1,    /* linked, no workers: locking disabled by design */
    FAN_LUA_LOCK_MODE_CORE_HOOK = 2, /* hooked interpreter serialises every resume */
    FAN_LUA_LOCK_MODE_WRAPPER = 3    /* luafan wraps FAN_RESUME (stock core) */
};

int event_mgr_lua_lock_mode(void);

/* Recursive depth of the global Lua lock on the CALLING thread (0 when the lock
 * is not in use). Read-only diagnostics; the setter is only used by the
 * FAN_LOCK_PROBE timing lever in luafan.c. */
int event_mgr_lua_lock_depth(void);
void event_mgr_lua_lock_depth_set(int depth);

#endif
