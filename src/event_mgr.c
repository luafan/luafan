#include "event_mgr.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static struct event_base *base = NULL;
static struct evdns_base *dnsbase = NULL;
static int initialized = 0;
static int looping = 0;
static int signal_count = 0;
static struct event signal_int;
static struct event signal_pipe;
static int signal_int_added = 0;
static int signal_pipe_added = 0;

extern void cleanup_http_curl(void);

static void cleanup_signal_events(void) {
    if (signal_int_added) {
        event_del(&signal_int);
        signal_int_added = 0;
    }
    if (signal_pipe_added) {
        event_del(&signal_pipe);
        signal_pipe_added = 0;
    }
    memset(&signal_int, 0, sizeof(signal_int));
    memset(&signal_pipe, 0, sizeof(signal_pipe));
}

static void cleanup_signals(void) {
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

static void signal_handler(int sig) {
    if (sig == SIGINT) {
        signal_count++;
        if (signal_count > 1) {
            _exit(0);
        }
    }
    event_mgr_break();
}

static void signal_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd;
    (void)what;
    (void)arg;
    event_mgr_break();
}

struct event_base *event_mgr_base(void) {
    if (!base) {
        base = event_base_new();
    }
    if (base && !initialized) {
        event_mgr_init();
    }
    return base;
}

struct event_base *event_mgr_base_current(void) {
    return base;
}

struct evdns_base *event_mgr_dnsbase(void) {
    return dnsbase;
}

int event_mgr_init(void) {
    if (initialized) {
        return -1;
    }
    if (!base) {
        base = event_base_new();
    }
    if (!base) {
        return -1;
    }
    initialized = 1;
    dnsbase = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS);
    if (dnsbase) {
        evdns_base_set_option(dnsbase, "randomize-case:", "0");
    }

    signal(SIGHUP, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGPIPE, signal_handler);
    event_assign(&signal_int, base, SIGINT, EV_SIGNAL | EV_PERSIST, signal_cb, NULL);
    signal_int_added = event_add(&signal_int, NULL) == 0;
    event_assign(&signal_pipe, base, SIGPIPE, EV_SIGNAL | EV_PERSIST, signal_cb, NULL);
    signal_pipe_added = event_add(&signal_pipe, NULL) == 0;
    return 0;
}

void event_mgr_break(void) {
    if (base) {
        event_base_loopbreak(base);
    }
}

int event_mgr_is_loop_running(void) {
    return looping;
}

int event_mgr_is_looping(void) {
    return looping;
}

static int once_delay(event_callback_fn callback, void *arg, long delay_ms) {
    if (!callback || !event_mgr_base()) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = delay_ms / 1000;
    tv.tv_usec = (delay_ms % 1000) * 1000;
    return event_base_once(base, -1, EV_TIMEOUT, callback, arg, &tv);
}

int event_mgr_once_internal(event_callback_fn callback, void *arg) {
    return once_delay(callback, arg, 0);
}

int event_mgr_once_internal_delay(event_callback_fn callback, void *arg, long delay_ms) {
    return once_delay(callback, arg, delay_ms);
}

int event_mgr_lua_lock_mode(void) {
    return FAN_LUA_LOCK_MODE_SINGLE;
}
int event_mgr_lua_lock_depth(void) { return 0; }
void event_mgr_lua_lock_depth_set(int depth) { (void)depth; }

int event_mgr_loop(void) {
    if (looping || !event_mgr_base()) {
        return -1;
    }
    looping = 1;
    event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY);
    looping = 0;
    cleanup_signals();
    cleanup_signal_events();
    cleanup_http_curl();
    if (dnsbase) {
        evdns_base_free(dnsbase, 1);
        dnsbase = NULL;
    }
    initialized = 0;
    return 0;
}

int event_mgr_loop_later_cleanup(void) {
    return event_mgr_loop();
}

void event_mgr_loop_cleanup(void) {
    cleanup_signal_events();
    if (dnsbase) {
        evdns_base_free(dnsbase, 1);
        dnsbase = NULL;
    }
    if (base) {
        event_base_free(base);
        base = NULL;
    }
    initialized = 0;
    looping = 0;
    signal_count = 0;
}

void event_mgr_cleanup(void) {
    event_mgr_loop_cleanup();
}
