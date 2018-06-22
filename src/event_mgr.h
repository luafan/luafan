
#ifndef event_mgr_h
#define event_mgr_h

#include <event.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <stdio.h>

struct event_base *event_mgr_base();
struct event_base *event_mgr_base_current();

struct evdns_base *event_mgr_dnsbase();
void event_mgr_break();
int event_mgr_init();
int event_mgr_loop();
void event_mgr_start();

#endif
