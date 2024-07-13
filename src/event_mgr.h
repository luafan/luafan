
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
int event_mgr_loop(void);
int event_mgr_loop_later_cleanup(void);
void event_mgr_loop_cleanup(void);
void event_mgr_start(void);

#endif
