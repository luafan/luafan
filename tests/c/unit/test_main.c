#include "test_framework.h"

/* External test suite declarations */

// From test_bytearray.c
extern test_suite_t bytearray_suite;
extern void bytearray_setup(void);
extern void bytearray_teardown(void);

// From test_event_mgr.c
extern test_suite_t event_mgr_suite;
extern void event_mgr_setup(void);
extern void event_mgr_teardown(void);

// From test_utlua.c
extern test_suite_t utlua_suite;
extern void utlua_setup(void);
extern void utlua_teardown(void);

// From test_objectbuf.c
extern test_suite_t objectbuf_suite;
extern void objectbuf_setup(void);
extern void objectbuf_teardown(void);

// From test_tcpd_core_complete.c
extern test_suite_t tcpd_core_complete_suite;

// From test_udpd_config.c
extern test_suite_t udpd_config_suite;
extern void udpd_config_setup(void);
extern void udpd_config_teardown(void);

// From test_udpd_dest.c
extern test_suite_t udpd_dest_suite;
extern void udpd_dest_setup(void);
extern void udpd_dest_teardown(void);

// From test_udpd_dns.c
extern test_suite_t udpd_dns_suite;
extern void udpd_dns_setup(void);
extern void udpd_dns_teardown(void);

// From test_udpd_utils.c
extern test_suite_t udpd_utils_suite;
extern void udpd_utils_setup(void);
extern void udpd_utils_teardown(void);

// From test_udpd_event.c
extern test_suite_t udpd_event_suite;
extern void udpd_event_setup(void);
extern void udpd_event_teardown(void);

// From test_udpd.c
extern test_suite_t udpd_suite;
extern void udpd_setup(void);
extern void udpd_teardown(void);

// From test_http.c
extern test_suite_t http_suite;
extern void http_setup(void);
extern void http_teardown(void);

// From test_httpd.c
extern test_suite_t httpd_suite;
extern void httpd_setup(void);
extern void httpd_teardown(void);

// From test_fifo.c
extern test_suite_t fifo_suite;
extern void fifo_setup(void);
extern void fifo_teardown(void);

// From test_luamariadb.c
extern test_suite_t luamariadb_suite;
extern void luamariadb_setup(void);
extern void luamariadb_teardown(void);

// From test_luafan_posix.c
extern test_suite_t luafan_posix_suite;
extern void luafan_posix_setup(void);
extern void luafan_posix_teardown(void);

// From test_luafan.c
extern test_suite_t luafan_suite;
extern void luafan_setup(void);
extern void luafan_teardown(void);


/* Main function to run all test suites */
int main(void) {
    // Configure test suites
    bytearray_suite.setup = bytearray_setup;
    bytearray_suite.teardown = bytearray_teardown;

    event_mgr_suite.setup = event_mgr_setup;
    event_mgr_suite.teardown = event_mgr_teardown;

    utlua_suite.setup = utlua_setup;
    utlua_suite.teardown = utlua_teardown;

    objectbuf_suite.setup = objectbuf_setup;
    objectbuf_suite.teardown = objectbuf_teardown;

    udpd_config_suite.setup = udpd_config_setup;
    udpd_config_suite.teardown = udpd_config_teardown;

    udpd_dest_suite.setup = udpd_dest_setup;
    udpd_dest_suite.teardown = udpd_dest_teardown;

    udpd_dns_suite.setup = udpd_dns_setup;
    udpd_dns_suite.teardown = udpd_dns_teardown;

    udpd_utils_suite.setup = udpd_utils_setup;
    udpd_utils_suite.teardown = udpd_utils_teardown;

    udpd_event_suite.setup = udpd_event_setup;
    udpd_event_suite.teardown = udpd_event_teardown;

    udpd_suite.setup = udpd_setup;
    udpd_suite.teardown = udpd_teardown;

    http_suite.setup = http_setup;
    http_suite.teardown = http_teardown;

    httpd_suite.setup = httpd_setup;
    httpd_suite.teardown = httpd_teardown;

    fifo_suite.setup = fifo_setup;
    fifo_suite.teardown = fifo_teardown;

    luamariadb_suite.setup = luamariadb_setup;
    luamariadb_suite.teardown = luamariadb_teardown;

    luafan_posix_suite.setup = luafan_posix_setup;
    luafan_posix_suite.teardown = luafan_posix_teardown;

    luafan_suite.setup = luafan_setup;
    luafan_suite.teardown = luafan_teardown;

    // Create array of all test suites
    test_suite_t* suites[] = {
        &bytearray_suite,
        &event_mgr_suite,
        &utlua_suite,
        &objectbuf_suite,
        &tcpd_core_complete_suite,
        &udpd_config_suite,
        &udpd_dest_suite,
        &udpd_dns_suite,
        &udpd_utils_suite,
        &udpd_event_suite,
        &udpd_suite,
        &http_suite,
        &httpd_suite,
        &fifo_suite,
        &luamariadb_suite,
        &luafan_posix_suite,
        &luafan_suite
    };

    // Run all tests
    int failures = run_all_tests(suites, 17);

    return failures > 0 ? 1 : 0;
}