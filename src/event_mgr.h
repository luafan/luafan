
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
void event_mgr_start(void);

// Worker pool for multi-threaded event processing
#define EVENT_MGR_MAX_WORKERS 6
#define EVENT_MGR_DEFAULT_WORKERS 4

int event_mgr_workers_init(int num_workers);
void event_mgr_workers_shutdown(void);
struct event_base *event_mgr_worker_base(int worker_id);
struct evdns_base *event_mgr_worker_dnsbase(int worker_id);
int event_mgr_next_worker(void);
int event_mgr_worker_count(void);

#endif
