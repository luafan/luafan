
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

struct event_base *event_mgr_base()
{
  if (!base)
  {
    base = event_base_new();
  }

  event_mgr_init();

  return base;
}

struct event_base *event_mgr_base_current()
{
  return base;
}

struct evdns_base *event_mgr_dnsbase()
{
  return dnsbase;
}

static void signal_handler(int sig)
{
  printf("%s: got singal %d\n", __func__, sig);
  switch (sig)
  {
  case SIGINT:
    signal_count++;
    if (signal_count > 1)
    {
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

static void signal_cb(evutil_socket_t fd, short event, void *arg)
{
  struct event *signal = arg;
  printf("%s: got signal %d\n", __func__, EVENT_SIGNAL(signal));

  switch (EVENT_SIGNAL(signal))
  {
  case SIGTERM:
  case SIGHUP:
  case SIGQUIT:
  case SIGINT:
    event_mgr_break();
  default:
    break;
  }
}

void event_mgr_break()
{
  if (base)
  {
    event_base_loopbreak(base);
  }
}

int event_mgr_init()
{
  if (!initialized)
  {
    initialized = 1;

    dnsbase = evdns_base_new(event_mgr_base(), 1);
    evdns_base_set_option(dnsbase, "randomize-case:", "0");

#if FAN_HAS_OPENSSL
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

    event_assign(&signal_int, event_mgr_base_current(), SIGINT, EV_SIGNAL | EV_PERSIST,
                 signal_cb, &signal_int);
    event_add(&signal_int, NULL);

    event_assign(&signal_pipe, event_mgr_base_current(), SIGPIPE,
                 EV_SIGNAL | EV_PERSIST, signal_cb, &signal_pipe);
    event_add(&signal_pipe, NULL);
    return 0;
  }

  return -1;
}

int event_mgr_loop()
{
  if (!looping)
  {
    event_mgr_init();

    looping = 1;

    event_base_dispatch(base);

    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    event_del(&signal_int);
    event_del(&signal_pipe);

#if FAN_HAS_OPENSSL
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_state(0);
    ERR_free_strings();
#endif

    evdns_base_free(dnsbase, 0);
    dnsbase = NULL;

    event_base_free(base);
    base = NULL;

    looping = 0;
    initialized = 0;
    return 0;
  }

  return -1;
}
