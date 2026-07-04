
#include "event_mgr.h"

#include "utlua.h"
#include <lua.h>

#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>

static struct event_base *base = NULL;
static struct evdns_base *dnsbase = NULL;

static int signal_count = 0;
static struct event signal_int;
static struct event signal_pipe;

static int looping = 0;
static int initialized = 0;

// Worker pool for multi-threaded event processing
struct event_worker {
    struct event_base *base;
    struct evdns_base *dnsbase;
    pthread_t thread;
    _Atomic int running;
    int id;
};

static struct event_worker workers[EVENT_MGR_MAX_WORKERS];
static int num_workers = 0;
static _Atomic unsigned int next_worker_idx = 0;

static void *worker_thread_func(void *arg) {
    struct event_worker *w = (struct event_worker *)arg;
    // Block SIGPIPE on worker threads
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    event_base_loop(w->base, EVLOOP_NO_EXIT_ON_EMPTY);
    w->running = 0;
    return NULL;
}

int event_mgr_workers_init(int count) {
    if (count <= 0 || count > EVENT_MGR_MAX_WORKERS) {
        count = EVENT_MGR_DEFAULT_WORKERS;
    }
    num_workers = count;

    for (int i = 0; i < num_workers; i++) {
        workers[i].id = i;
        workers[i].base = event_base_new();
        if (!workers[i].base) return -1;

        workers[i].dnsbase = evdns_base_new(workers[i].base, 0);
        if (!workers[i].dnsbase) return -1;
        evdns_base_set_option(workers[i].dnsbase, "randomize-case:", "0");

        workers[i].running = 1;
        int rc = pthread_create(&workers[i].thread, NULL, worker_thread_func, &workers[i]);
        if (rc != 0) return -1;
    }
    return 0;
}

void event_mgr_workers_shutdown(void) {
    // Stop the worker threads first so no callbacks fire while we free state.
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].running && workers[i].base) {
            event_base_loopbreak(workers[i].base);
        }
    }
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].thread) {
            pthread_join(workers[i].thread, NULL);
            workers[i].thread = 0;
        }
    }

    // Free the bases. Callers that still need to run Lua finalisers (which may
    // bufferevent_free into a worker base) MUST do so before reaching here —
    // see event_mgr_workers_stop_threads().
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].dnsbase) {
            // fail_requests=1 — see cleanup_dnsbase(): dropping pending DNS
            // callbacks silently can leave Lua refs dangling.
            evdns_base_free(workers[i].dnsbase, 1);
            workers[i].dnsbase = NULL;
        }
        if (workers[i].base) {
            event_base_free(workers[i].base);
            workers[i].base = NULL;
        }
    }
    num_workers = 0;
}

// Stop worker threads but keep their event_bases alive, so that Lua __gc
// finalisers running afterwards can still bufferevent_free() into them.
// The matching event_base_free() happens later via event_mgr_workers_free_bases.
void event_mgr_workers_stop_threads(void) {
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].running && workers[i].base) {
            event_base_loopbreak(workers[i].base);
        }
    }
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].thread) {
            pthread_join(workers[i].thread, NULL);
            workers[i].thread = 0;
        }
    }
}

void event_mgr_workers_free_bases(void) {
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].dnsbase) {
            // fail_requests=1 — see cleanup_dnsbase().
            evdns_base_free(workers[i].dnsbase, 1);
            workers[i].dnsbase = NULL;
        }
        if (workers[i].base) {
            event_base_free(workers[i].base);
            workers[i].base = NULL;
        }
    }
    num_workers = 0;
}

struct event_base *event_mgr_worker_base(int worker_id) {
    if (worker_id < 0 || worker_id >= num_workers) {
        return event_mgr_base(); // fallback to main
    }
    return workers[worker_id].base;
}

struct evdns_base *event_mgr_worker_dnsbase(int worker_id) {
    if (worker_id < 0 || worker_id >= num_workers) {
        return event_mgr_dnsbase();
    }
    return workers[worker_id].dnsbase;
}

int event_mgr_next_worker(void) {
    if (num_workers <= 0) return -1;
    unsigned int idx = next_worker_idx++;
    return (int)(idx % (unsigned int)num_workers);
}

int event_mgr_worker_count(void) {
    return num_workers;
}

struct event_base *event_mgr_base() {
    if (!base) {
        base = event_base_new();
    }

    event_mgr_init();

    return base;
}

struct event_base *event_mgr_base_current() {
    return base;
}

struct evdns_base *event_mgr_dnsbase() {
    return dnsbase;
}

static void signal_handler(int sig) {
    printf("%s: got signal %d\n", __func__, sig);
    switch (sig) {
        case SIGINT:
            signal_count++;
            if (signal_count > 1) {
                printf("force exit.\n");
                _exit(0);
            }
        case SIGTERM:
        case SIGHUP:
        case SIGQUIT:
            event_mgr_break();
            break;
        case SIGPIPE:
            printf("ignore SIGPIPE.\n");
            break;
    }
}

static void signal_cb(evutil_socket_t fd, short event, void *arg) {
    struct event *signal = arg;
    printf("%s: got signal %d\n", __func__, EVENT_SIGNAL(signal));

    switch (EVENT_SIGNAL(signal)) {
        case SIGTERM:
        case SIGHUP:
        case SIGQUIT:
        case SIGINT:
            event_mgr_break();
        default:
            break;
    }
}

void event_mgr_break() {
    // Only break the main loop. Worker threads are stopped by
    // event_mgr_workers_stop_threads() inside event_mgr_loop_later_cleanup,
    // and their bases are freed later by event_mgr_workers_free_bases()
    // inside event_mgr_loop_cleanup — after the caller has run lua_close.
    // Freeing worker bases here races Lua finalisers (__gc → bufferevent_free)
    // and triggers libevent's "evcb_pri < nactivequeues" assertion.
    if (base) {
        event_base_loopbreak(base);
    }
}

/* Internal cleanup functions to eliminate redundancy */
static void cleanup_signals() {
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

static void cleanup_signal_events() {
    if (base) {
        event_del(&signal_int);
        event_del(&signal_pipe);
    }
}

static void cleanup_openssl() {
#if FAN_HAS_OPENSSL && OPENSSL_VERSION_NUMBER < 0x1010000fL
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_state(0);
    ERR_free_strings();
#endif
}

static void cleanup_dnsbase() {
    if (dnsbase) {
        // Pass 1 to fail outstanding requests rather than silently
        // dropping them. With fail_requests=0 any callback that runs
        // before the base is fully torn down can dereference state
        // (lua_State, conn) that is already gone — the resolver's
        // worker thread may queue a result just as we free the base.
        // Asking libevent to invoke each pending callback with an
        // error gives the user code a chance to clean its refs while
        // the base is still valid.
        evdns_base_free(dnsbase, 1);
        dnsbase = NULL;
    }
}

// Defined in http.c. Strongly linked — both TUs live in the same archive
// (luafan). Tears down the curl multi handle and its evtimers. Must be called
// while BOTH the event_base AND the Lua state are still alive: curl_multi_cleanup
// drives multi_done → progress callbacks that touch Lua via clientp.
extern void cleanup_http_curl(void);

static void cleanup_eventbase() {
    if (base) {
        event_base_free(base);
        base = NULL;
    }
}

static void reset_state() {
    initialized = 0;
    looping = 0;
    signal_count = 0;
}

static void full_cleanup() {
    cleanup_signals();
    cleanup_signal_events();
    cleanup_http_curl();
    cleanup_openssl();
    cleanup_dnsbase();
    event_mgr_workers_shutdown();
    cleanup_eventbase();
    reset_state();
}

int event_mgr_init() {
    if (!initialized) {
        initialized = 1;

        dnsbase = evdns_base_new(event_mgr_base(), EVDNS_BASE_INITIALIZE_NAMESERVERS);
        if (dnsbase) {
            evdns_base_set_option(dnsbase, "randomize-case:", "0");
        } else {
            fprintf(stderr, "event_mgr_init: evdns_base_new failed (no network?), DNS resolution unavailable\n");
        }

#if FAN_HAS_OPENSSL && OPENSSL_VERSION_NUMBER < 0x1010000fL
        SSL_library_init();
        ERR_load_crypto_strings();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
#endif

        signal(SIGHUP, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGQUIT, signal_handler);
        signal(SIGPIPE, signal_handler);

        event_assign(&signal_int, event_mgr_base_current(), SIGINT, EV_SIGNAL | EV_PERSIST, signal_cb, &signal_int);
        event_add(&signal_int, NULL);

        event_assign(&signal_pipe, event_mgr_base_current(), SIGPIPE, EV_SIGNAL | EV_PERSIST, signal_cb, &signal_pipe);
        event_add(&signal_pipe, NULL);
        return 0;
    }

    return -1;
}

int event_mgr_loop() {
    if (!looping) {
        event_mgr_init();

        looping = 1;

        event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY);

        // Symmetric with event_mgr_loop_later_cleanup(): we cannot run
        // full_cleanup() here because the caller still holds Lua state
        // whose __gc finalisers are about to run (lua_close happens later
        // in the embedder, e.g. when the Lua module is unloaded). Those
        // finalisers may bufferevent_free() into worker bases — freeing
        // those bases here triggers libevent's "evcb_pri < nactivequeues"
        // assertion and use-after-free in __gc.
        //
        // Stop worker threads (no callbacks can fire any more) and clean
        // signal handlers / dns / openssl. The worker bases and the main
        // base are freed by the matching event_mgr_loop_cleanup() call,
        // which the embedder invokes after lua_close. If the embedder
        // never calls it, the bases leak at process exit — that is far
        // preferable to a crash.
        //
        // cleanup_http_curl() must run here, while BOTH the base and the
        // Lua state are alive: curl_multi_cleanup drives multi_done →
        // Curl_pgrsDone → onprogress, which dereferences L via clientp.
        // Running it later (after lua_close) crashes in lua_rawgeti.
        cleanup_signals();
        cleanup_signal_events();
        cleanup_http_curl();
        cleanup_openssl();
        cleanup_dnsbase();
        event_mgr_workers_stop_threads();

        looping = 0;
        initialized = 0;
        return 0;
    }

    return -1;
}

int event_mgr_loop_later_cleanup() {
    if (!looping) {
        event_mgr_init();

        looping = 1;

        event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY);

        // Partial cleanup - keep event base for later cleanup.
        // Stop worker threads but keep their bases alive: Lua finalisers
        // run after this returns (lua_close → __gc → bufferevent_free) and
        // must be able to remove themselves from their owning worker base.
        // The worker bases are freed in event_mgr_loop_cleanup().
        //
        // cleanup_http_curl() must run here (not in event_mgr_loop_cleanup):
        // curl_multi_cleanup → multi_done → onprogress dereferences L.
        cleanup_signals();
        cleanup_signal_events();
        cleanup_http_curl();
        cleanup_openssl();
        cleanup_dnsbase();
        event_mgr_workers_stop_threads();

        looping = 0;
        initialized = 0;
        return 0;
    }

    return -1;
}

void event_mgr_cleanup() {
    if (initialized) {
        full_cleanup();
    }
}

void event_mgr_loop_cleanup() {
    // Free worker bases now — by this point the caller has run lua_close()
    // so all bevs created on these bases have been removed.
    event_mgr_workers_free_bases();
    cleanup_eventbase();
}

// Check if we are currently in a loop without starting one
int event_mgr_is_looping() {
    return looping;
}
