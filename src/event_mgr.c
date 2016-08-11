
#include "event_mgr.h"
#include "utlua.h"
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
    evdns_base_set_option(dnsbase, "randomize-case:", "0");

#if FAN_HAS_OPENSSL
    SSL_library_init();
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    return 0;
  }

  return -1;
}

int event_mgr_loop() {
  if (!looping) {
    event_mgr_init();

    looping = 1;

    event_base_dispatch(base);

#if FAN_HAS_OPENSSL
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_state(0);
    ERR_free_strings();
#endif

    evdns_base_free(dnsbase, 0);
    event_base_free(base);
    base = NULL;
    dnsbase = NULL;
    looping = 0;
    return 0;
  }

  return -1;
}
