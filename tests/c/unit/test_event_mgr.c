#include "test_framework.h"
#include "event_mgr.h"
#include <event2/event.h>
#include <unistd.h>
#include <sys/time.h>

/* Test event base creation and initialization */
TEST_CASE(test_event_mgr_base_creation) {
    // Get event base - should create if not exists
    struct event_base *base1 = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base1);

    // Second call should return same base
    struct event_base *base2 = event_mgr_base();
    TEST_ASSERT_EQUAL(base1, base2);

    // Current base should match
    struct event_base *current = event_mgr_base_current();
    TEST_ASSERT_EQUAL(base1, current);
}

/* Test DNS base creation */
TEST_CASE(test_event_mgr_dnsbase) {
    // Clean up any previous state first
    event_mgr_cleanup();

    // Initialize event manager should succeed after cleanup
    TEST_ASSERT_EQUAL(0, event_mgr_init());

    // Get DNS base
    struct evdns_base *dnsbase = event_mgr_dnsbase();
    TEST_ASSERT_NOT_NULL(dnsbase);

    // Second call should return same base
    struct evdns_base *dnsbase2 = event_mgr_dnsbase();
    TEST_ASSERT_EQUAL(dnsbase, dnsbase2);
}

/* Test event manager initialization */
TEST_CASE(test_event_mgr_init) {
    // Clean up any previous state
    event_mgr_cleanup();

    // First init should succeed after cleanup
    int result1 = event_mgr_init();
    TEST_ASSERT_EQUAL(0, result1);

    // Second init should fail (already initialized)
    int result2 = event_mgr_init();
    TEST_ASSERT_EQUAL(-1, result2);

    // DNS base should be available after init
    struct evdns_base *dnsbase = event_mgr_dnsbase();
    TEST_ASSERT_NOT_NULL(dnsbase);
}

/* Test event manager break functionality */
TEST_CASE(test_event_mgr_break) {
    // Initialize event manager
    struct event_base *base = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base);

    // Breaking should not crash (we can't easily test the actual break behavior
    // without running a loop, but we can ensure the function executes)
    event_mgr_break();

    // Event base should still be valid
    struct event_base *base_after = event_mgr_base_current();
    TEST_ASSERT_EQUAL(base, base_after);
}

/* Test event loop states */
TEST_CASE(test_event_mgr_loop_states) {
    // Note: We can't actually run the event loop in tests as it would block
    // But we can test the state management around it

    // Loop should initially not be running
    // (We can't directly test this as the variables are static,
    // but we can test the return values)

    // Initialize if not already done
    event_mgr_base();

    // Test that consecutive loop calls would fail
    // (We can't actually call event_mgr_loop() as it would block)
    // Instead we just verify the manager is initialized
    struct event_base *base = event_mgr_base_current();
    TEST_ASSERT_NOT_NULL(base);
}

/* Test cleanup functionality */
TEST_CASE(test_event_mgr_cleanup) {
    // Initialize event manager
    struct event_base *base = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base);

    // Get initial DNS base
    struct evdns_base *dnsbase = event_mgr_dnsbase();
    TEST_ASSERT_NOT_NULL(dnsbase);

    // Test loop cleanup (this only cleans up the base, not full cleanup)
    event_mgr_loop_cleanup();

    // After cleanup, base should be NULL
    struct event_base *base_after = event_mgr_base_current();
    TEST_ASSERT_NULL(base_after);
}

/* Test event base recreation after cleanup */
TEST_CASE(test_event_mgr_base_recreation) {
    // Ensure we start with a clean state
    event_mgr_loop_cleanup();

    // Base should be NULL initially
    struct event_base *base_null = event_mgr_base_current();
    TEST_ASSERT_NULL(base_null);

    // Creating new base should work
    struct event_base *new_base = event_mgr_base();
    TEST_ASSERT_NOT_NULL(new_base);

    // Current should match new base
    struct event_base *current = event_mgr_base_current();
    TEST_ASSERT_EQUAL(new_base, current);
}

/* Test multiple initialization cycles */
TEST_CASE(test_event_mgr_init_cycles) {
    // Clean state completely
    event_mgr_cleanup();

    // First initialization cycle
    // Note: event_mgr_base() calls event_mgr_init() automatically
    struct event_base *base1 = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base1);

    // Since event_mgr_base() already called init, subsequent call should return -1
    int init_result1 = event_mgr_init();
    TEST_ASSERT_EQUAL(-1, init_result1);

    struct evdns_base *dns1 = event_mgr_dnsbase();
    TEST_ASSERT_NOT_NULL(dns1);

    // Complete cleanup - this should reset the initialized state
    event_mgr_cleanup();

    // After cleanup, current base should be NULL
    struct event_base *current = event_mgr_base_current();
    TEST_ASSERT_NULL(current);

    // Second initialization cycle
    struct event_base *base2 = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base2);

    // After cleanup and reinit, initialization should succeed again
    event_mgr_cleanup(); // Clean first
    int init_result2 = event_mgr_init();
    TEST_ASSERT_EQUAL(0, init_result2);
}

/* Test error conditions */
TEST_CASE(test_event_mgr_error_conditions) {
    // Test break with NULL base
    event_mgr_loop_cleanup();

    // Breaking with NULL base should not crash
    event_mgr_break();

    // Should still be able to create base after
    struct event_base *base = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base);
}

/* Test timer functionality integration */
TEST_CASE(test_event_mgr_timer_integration) {
    // Initialize event manager
    struct event_base *base = event_mgr_base();
    TEST_ASSERT_NOT_NULL(base);

    // Create a simple timer event to test integration
    struct event timer_event;
    struct timeval tv = {0, 1000}; // 1ms

    // This tests that our event base works with libevent APIs
    int result = event_assign(&timer_event, base, -1, EV_TIMEOUT, NULL, NULL);
    TEST_ASSERT_EQUAL(0, result);

    // Add the event (but don't run the loop)
    result = event_add(&timer_event, &tv);
    TEST_ASSERT_EQUAL(0, result);

    // Clean up the event
    event_del(&timer_event);
}

/* Set up test suite */
TEST_SUITE_BEGIN(event_mgr)
    TEST_SUITE_ADD(test_event_mgr_base_creation)
    TEST_SUITE_ADD(test_event_mgr_dnsbase)
    TEST_SUITE_ADD(test_event_mgr_init)
    TEST_SUITE_ADD(test_event_mgr_break)
    TEST_SUITE_ADD(test_event_mgr_loop_states)
    TEST_SUITE_ADD(test_event_mgr_cleanup)
    TEST_SUITE_ADD(test_event_mgr_base_recreation)
    TEST_SUITE_ADD(test_event_mgr_init_cycles)
    TEST_SUITE_ADD(test_event_mgr_error_conditions)
    TEST_SUITE_ADD(test_event_mgr_timer_integration)
TEST_SUITE_END(event_mgr)

TEST_SUITE_ADD_NAME(test_event_mgr_base_creation)
TEST_SUITE_ADD_NAME(test_event_mgr_dnsbase)
TEST_SUITE_ADD_NAME(test_event_mgr_init)
TEST_SUITE_ADD_NAME(test_event_mgr_break)
TEST_SUITE_ADD_NAME(test_event_mgr_loop_states)
TEST_SUITE_ADD_NAME(test_event_mgr_cleanup)
TEST_SUITE_ADD_NAME(test_event_mgr_base_recreation)
TEST_SUITE_ADD_NAME(test_event_mgr_init_cycles)
TEST_SUITE_ADD_NAME(test_event_mgr_error_conditions)
TEST_SUITE_ADD_NAME(test_event_mgr_timer_integration)

TEST_SUITE_FINISH(event_mgr)

/* Test suite setup/teardown functions */
void event_mgr_setup(void) {
    printf("Setting up event_mgr test suite...\n");
    // Reset any previous state completely
    event_mgr_cleanup();
}

void event_mgr_teardown(void) {
    printf("Tearing down event_mgr test suite...\n");
    // Clean up after tests completely
    event_mgr_cleanup();
}