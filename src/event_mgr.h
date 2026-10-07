#ifndef event_mgr_h
#define event_mgr_h

#include <event.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <stdio.h>

struct event_base *event_mgr_base(void);
struct event_base *event_mgr_base_current(void);
struct evdns_base *event_mgr_dnsbase(void);

/* Serialize event creation/addition with event base teardown. */
void event_mgr_base_lock(void);
void event_mgr_base_unlock(void);
void event_mgr_break(void);
int event_mgr_init(void);
void event_mgr_cleanup(void);
int event_mgr_loop(void);
int event_mgr_loop_later_cleanup(void);
void event_mgr_loop_cleanup(void);
int event_mgr_is_looping(void);


/* Internal continuations always run on the single event base. */
int event_mgr_once_internal(event_callback_fn callback, void *arg);
int event_mgr_once_internal_delay(event_callback_fn callback, void *arg, long delay_ms);
int event_mgr_is_loop_running(void);

/* Retained diagnostic API: the single-threaded runtime has no Lua lock. */
enum {
    FAN_LUA_LOCK_MODE_NONE = 0,
    FAN_LUA_LOCK_MODE_SINGLE = 1
};
int event_mgr_lua_lock_mode(void);
int event_mgr_lua_lock_depth(void);
void event_mgr_lua_lock_depth_set(int depth);

#endif
