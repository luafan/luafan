#include "event_mgr.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <event2/thread.h>

#if DEBUG
#if defined(__APPLE__)
#include <os/log.h>
#define PANPIPE_LOOP_LOG(...) os_log(OS_LOG_DEFAULT, __VA_ARGS__)
#else
#define PANPIPE_LOOP_LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#endif
#else
#define PANPIPE_LOOP_LOG(...) do { } while (0)
#endif

static struct event_base *base = NULL;
static struct evdns_base *dnsbase = NULL;
static pthread_mutex_t base_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static int pthread_support_initialized = 0;
static int initialized = 0;
static int looping = 0;
static int signal_count = 0;
static struct event signal_int;
static int signal_int_added = 0;

/*
 * Protects the event base pointer and the interval in which callers create
 * and add events to it. Cleanup takes the same lock before freeing the base.
 */
void event_mgr_base_lock(void) {
    pthread_mutex_lock(&base_lifecycle_mutex);
}

void event_mgr_base_unlock(void) {
    pthread_mutex_unlock(&base_lifecycle_mutex);
}

extern void cleanup_http_curl(void);

static void cleanup_signal_events(void) {
    if (signal_int_added) {
        event_del(&signal_int);
        signal_int_added = 0;
    }
    memset(&signal_int, 0, sizeof(signal_int));
}

static void cleanup_signals(void) {
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    // SIGPIPE is a per-connection write failure (EPIPE), not a process shutdown.
    // Keep it ignored so one broken upstream socket cannot break the global loop.
    signal(SIGPIPE, SIG_IGN);
}

static void signal_handler(int sig) {
    PANPIPE_LOOP_LOG("[event_mgr] signal_handler sig=%d thread=%lu",
                 sig, (unsigned long)pthread_self());
    if (sig == SIGINT) {
        signal_count++;
        if (signal_count > 1) {
            _exit(0);
        }
    }
    event_mgr_break();
}

static void signal_cb(evutil_socket_t fd, short what, void *arg) {
    (void)arg;
    PANPIPE_LOOP_LOG("[event_mgr] signal_cb fd=%d what=%d thread=%lu",
                 (int)fd, (int)what, (unsigned long)pthread_self());
    event_mgr_break();
}

struct event_base *event_mgr_base(void) {
    if (!base && event_mgr_init() != 0) {
        return NULL;
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

    if (!pthread_support_initialized) {
        if (evthread_use_pthreads() != 0) {
            return -1;
        }
        pthread_support_initialized = 1;
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
    // Do not register SIGPIPE with signal_handler/signal_cb: EPIPE belongs to
    // the individual connection and must never call event_mgr_break().
    signal(SIGPIPE, SIG_IGN);
    event_assign(&signal_int, base, SIGINT, EV_SIGNAL | EV_PERSIST, signal_cb, NULL);
    signal_int_added = event_add(&signal_int, NULL) == 0;
    return 0;
}

void event_mgr_break(void) {
    struct event_base *current = base;
    int result = -1;
    if (current) {
        result = event_base_loopbreak(current);
    }
    PANPIPE_LOOP_LOG("[event_mgr] break base=%p looping=%d thread=%lu result=%d",
                 (void *)current, looping, (unsigned long)pthread_self(), result);
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
        PANPIPE_LOOP_LOG("[event_mgr] loop refused looping=%d base=%p thread=%lu",
                     looping, (void *)base, (unsigned long)pthread_self());
        return -1;
    }
    looping = 1;
    PANPIPE_LOOP_LOG("[event_mgr] loop enter base=%p thread=%lu",
                 (void *)base, (unsigned long)pthread_self());
    int result = event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY);
    PANPIPE_LOOP_LOG("[event_mgr] loop exit base=%p result=%d thread=%lu",
                 (void *)base, result, (unsigned long)pthread_self());
    looping = 0;
    cleanup_signals();
    cleanup_signal_events();
    cleanup_http_curl();
    if (dnsbase) {
        evdns_base_free(dnsbase, 1);
        dnsbase = NULL;
    }
    initialized = 0;
    return result;
}

int event_mgr_loop_later_cleanup(void) {
    return event_mgr_loop();
}

void event_mgr_loop_cleanup(void) {
    PANPIPE_LOOP_LOG("[event_mgr] cleanup begin base=%p looping=%d thread=%lu",
                 (void *)base, looping, (unsigned long)pthread_self());
    event_mgr_base_lock();
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
    event_mgr_base_unlock();
    PANPIPE_LOOP_LOG("[event_mgr] cleanup end thread=%lu",
                 (unsigned long)pthread_self());
}

void event_mgr_cleanup(void) {
    event_mgr_loop_cleanup();
}
