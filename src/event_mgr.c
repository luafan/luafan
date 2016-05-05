
#include "event_mgr.h"
#include <lua.h>

static struct event_base *base = NULL;
static struct evdns_base *dnsbase = NULL;

static int looping = 0;

struct event_base *event_mgr_base() {
  return base;
}

struct evdns_base *event_mgr_dnsbase() {
  return dnsbase;
}

void event_mgr_break() {
  if (base) {
    event_base_loopbreak(base);
  }
}

int event_mgr_init() {
  if (!base) {
    base = event_base_new();
    dnsbase = evdns_base_new(base, 1);

    return 0;
  }

  return -1;
}

int event_mgr_loop() {
  if (!looping) {
    event_mgr_init();

    looping = 1;
    event_base_dispatch(base);
    event_base_free(base);
    base = NULL;
    looping = 0;
    return 0;
  }

  return -1;
}
