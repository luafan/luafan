
#include "event_mgr.h"

#include "utlua.h"
#include <lua.h>

#include <signal.h>

static struct event_base *base = NULL;
static struct evdns_base *dnsbase = NULL;

static int signal_count = 0;
static struct event signal_int;
static struct event signal_pipe;

static int looping = 0;
static int initialized = 0;

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
    printf("%s: got singal %d\n", __func__, sig);
    switch (sig) {
        case SIGINT:
            signal_count++;
            if (signal_count > 1) {
                printf("force exit.\n");
                exit(0);
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
        evdns_base_free(dnsbase, 0);
        dnsbase = NULL;
    }
}

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
    cleanup_openssl();
    cleanup_dnsbase();
    cleanup_eventbase();
    reset_state();
}

int event_mgr_init() {
    if (!initialized) {
        initialized = 1;

        dnsbase = evdns_base_new(event_mgr_base(), 0);
        evdns_base_set_option(dnsbase, "randomize-case:", "0");

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

        // Full cleanup after loop exits
        full_cleanup();
        return 0;
    }

    return -1;
}

int event_mgr_loop_later_cleanup() {
    if (!looping) {
        event_mgr_init();

        looping = 1;

        event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY);

        // Partial cleanup - keep event base for later cleanup
        cleanup_signals();
        cleanup_signal_events();
        cleanup_openssl();
        cleanup_dnsbase();

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
    cleanup_eventbase();
}
