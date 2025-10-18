#include "../framework/test_framework.h"
#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

// ============================================================================
// Test Cases for udpd_dns.c
// ============================================================================

TEST_CASE(udpd_dns_request_create) {
    // Test valid parameters
    udpd_dns_request_t *request = udpd_dns_request_create("example.com", 80);
    TEST_ASSERT_NOT_NULL(request);

    TEST_ASSERT_NOT_NULL(request->hostname);
    TEST_ASSERT_STRING_EQUAL("example.com", request->hostname);
    TEST_ASSERT_EQUAL(80, request->port);
    TEST_ASSERT_NULL(request->mainthread);
    TEST_ASSERT_EQUAL(0, request->_ref_);
    TEST_ASSERT_EQUAL(0, request->yielded);
    TEST_ASSERT_NULL(request->callback);
    TEST_ASSERT_NULL(request->callback_ctx);

    udpd_dns_request_cleanup(request);
}

TEST_CASE(udpd_dns_request_create_invalid_params) {
    // Test NULL hostname
    udpd_dns_request_t *request = udpd_dns_request_create(NULL, 80);
    TEST_ASSERT_NULL(request);

    // Test invalid port (zero)
    request = udpd_dns_request_create("example.com", 0);
    TEST_ASSERT_NULL(request);

    // Test invalid port (negative)
    request = udpd_dns_request_create("example.com", -1);
    TEST_ASSERT_NULL(request);

    // Test invalid port (too large)
    request = udpd_dns_request_create("example.com", 65536);
    TEST_ASSERT_NULL(request);

    // Test empty hostname
    request = udpd_dns_request_create("", 80);
    TEST_ASSERT_NOT_NULL(request);  // Empty hostname is allowed
    udpd_dns_request_cleanup(request);
}

TEST_CASE(udpd_dns_request_cleanup) {
    // Test normal cleanup
    udpd_dns_request_t *request = udpd_dns_request_create("test.com", 443);
    TEST_ASSERT_NOT_NULL(request);

    udpd_dns_request_cleanup(request);
    // No crash = success

    // Test cleanup with NULL (should not crash)
    udpd_dns_request_cleanup(NULL);
}

TEST_CASE(udpd_dns_request_edge_cases) {
    // Test various hostname formats
    const char *hostnames[] = {
        "localhost",
        "127.0.0.1",
        "example.org",
        "sub.domain.example.com",
        "very-long-hostname-with-many-parts.example.co.uk",
        "a.b",
        "1",
        NULL
    };

    for (int i = 0; hostnames[i]; i++) {
        udpd_dns_request_t *request = udpd_dns_request_create(hostnames[i], 8080);
        TEST_ASSERT_NOT_NULL(request);
        TEST_ASSERT_STRING_EQUAL(hostnames[i], request->hostname);
        udpd_dns_request_cleanup(request);
    }

    // Test various port numbers
    int ports[] = {1, 22, 80, 443, 8080, 65535, -1};  // -1 marks end
    for (int i = 0; ports[i] > 0; i++) {
        udpd_dns_request_t *request = udpd_dns_request_create("example.com", ports[i]);
        TEST_ASSERT_NOT_NULL(request);
        TEST_ASSERT_EQUAL(ports[i], request->port);
        udpd_dns_request_cleanup(request);
    }
}

TEST_CASE(udpd_dns_resolve_sync_localhost) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    // Test localhost resolution
    int result = udpd_dns_resolve_sync("127.0.0.1", 8080, &addr, &addrlen);
    if (result == 0) {  // Only test if resolution succeeded
        TEST_ASSERT_EQUAL(AF_INET, addr.ss_family);

        struct sockaddr_in *addr_in = (struct sockaddr_in*)&addr;
        TEST_ASSERT_EQUAL(htons(8080), addr_in->sin_port);
        TEST_ASSERT_EQUAL(htonl(INADDR_LOOPBACK), addr_in->sin_addr.s_addr);
        TEST_ASSERT_EQUAL(sizeof(struct sockaddr_in), addrlen);
    }
}

TEST_CASE(udpd_dns_resolve_sync_invalid_params) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    // Test NULL hostname
    int result = udpd_dns_resolve_sync(NULL, 8080, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    // Test NULL addr
    result = udpd_dns_resolve_sync("127.0.0.1", 8080, NULL, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    // Test NULL addrlen
    result = udpd_dns_resolve_sync("127.0.0.1", 8080, &addr, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid hostname
    result = udpd_dns_resolve_sync("invalid.nonexistent.domain.test", 8080, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_dns_resolve_sync_various_addresses) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    // Test various IP addresses that should resolve synchronously
    const char *test_ips[] = {
        "0.0.0.0",
        "127.0.0.1",
        "255.255.255.255",
        NULL
    };

    for (int i = 0; test_ips[i]; i++) {
        int result = udpd_dns_resolve_sync(test_ips[i], 9000, &addr, &addrlen);
        if (result == 0) {
            TEST_ASSERT_EQUAL(AF_INET, addr.ss_family);
            TEST_ASSERT_EQUAL(sizeof(struct sockaddr_in), addrlen);

            struct sockaddr_in *addr_in = (struct sockaddr_in*)&addr;
            TEST_ASSERT_EQUAL(htons(9000), addr_in->sin_port);
        }
    }
}

TEST_CASE(udpd_dns_async_request_structure) {
    // Test setting up async request structure
    udpd_dns_request_t *request = udpd_dns_request_create("async.example.com", 5000);
    TEST_ASSERT_NOT_NULL(request);

    // Mock callback function
    void (*mock_callback)(int, struct evutil_addrinfo*, void*) = NULL;
    void *mock_context = (void*)0x12345678;

    // Test setting callback parameters
    request->callback = mock_callback;
    request->callback_ctx = mock_context;

    TEST_ASSERT_EQUAL(mock_callback, request->callback);
    TEST_ASSERT_EQUAL(mock_context, request->callback_ctx);

    udpd_dns_request_cleanup(request);
}

TEST_CASE(udpd_dns_request_memory_management) {
    // Test multiple create/cleanup cycles
    for (int i = 0; i < 100; i++) {
        char hostname[64];
        snprintf(hostname, sizeof(hostname), "host%d.example.com", i);

        udpd_dns_request_t *request = udpd_dns_request_create(hostname, 3000 + i);
        TEST_ASSERT_NOT_NULL(request);
        TEST_ASSERT_STRING_EQUAL(hostname, request->hostname);
        TEST_ASSERT_EQUAL(3000 + i, request->port);

        udpd_dns_request_cleanup(request);
    }
}

TEST_CASE(udpd_dns_port_boundaries) {
    // Test port boundary values
    udpd_dns_request_t *request;

    // Test minimum valid port
    request = udpd_dns_request_create("example.com", 1);
    TEST_ASSERT_NOT_NULL(request);
    TEST_ASSERT_EQUAL(1, request->port);
    udpd_dns_request_cleanup(request);

    // Test maximum valid port
    request = udpd_dns_request_create("example.com", 65535);
    TEST_ASSERT_NOT_NULL(request);
    TEST_ASSERT_EQUAL(65535, request->port);
    udpd_dns_request_cleanup(request);

    // Test common ports
    int common_ports[] = {21, 22, 23, 25, 53, 80, 110, 143, 443, 993, 995, -1};
    for (int i = 0; common_ports[i] > 0; i++) {
        request = udpd_dns_request_create("example.com", common_ports[i]);
        TEST_ASSERT_NOT_NULL(request);
        TEST_ASSERT_EQUAL(common_ports[i], request->port);
        udpd_dns_request_cleanup(request);
    }
}

TEST_CASE(udpd_dns_hostname_variations) {
    // Test various hostname string patterns
    struct {
        const char *hostname;
        int should_succeed;
    } test_cases[] = {
        {"a", 1},                              // Single character
        {"ab", 1},                             // Two characters
        {"localhost", 1},                      // Common hostname
        {"www.example.com", 1},                // Standard FQDN
        {"sub.domain.example.org", 1},         // Subdomain
        {"test-host", 1},                      // Hyphenated
        {"host_with_underscores", 1},          // Underscores (allowed in some contexts)
        {"192.168.1.1", 1},                   // IPv4 address
        {"example.com.", 1},                  // Trailing dot
        {"UPPERCASE.EXAMPLE.COM", 1},         // Uppercase
        {NULL, 0}
    };

    for (int i = 0; test_cases[i].hostname || test_cases[i].should_succeed == 0; i++) {
        if (!test_cases[i].hostname && i > 0) break;

        udpd_dns_request_t *request = udpd_dns_request_create(test_cases[i].hostname, 8080);

        if (test_cases[i].should_succeed) {
            TEST_ASSERT_NOT_NULL(request);
            if (request) {
                TEST_ASSERT_STRING_EQUAL(test_cases[i].hostname, request->hostname);
                udpd_dns_request_cleanup(request);
            }
        } else {
            TEST_ASSERT_NULL(request);
        }
    }
}

TEST_CASE(udpd_dns_initialization_state) {
    // Test that all fields are properly initialized
    udpd_dns_request_t *request = udpd_dns_request_create("init.test.com", 12345);
    TEST_ASSERT_NOT_NULL(request);

    // Verify all fields have expected initial values
    TEST_ASSERT_NOT_NULL(request->hostname);
    TEST_ASSERT_EQUAL(12345, request->port);
    TEST_ASSERT_NULL(request->mainthread);
    TEST_ASSERT_EQUAL(0, request->_ref_);
    TEST_ASSERT_EQUAL(0, request->yielded);
    TEST_ASSERT_NULL(request->callback);
    TEST_ASSERT_NULL(request->callback_ctx);

    udpd_dns_request_cleanup(request);
}

// ============================================================================
// Test Suite Setup/Teardown
// ============================================================================

void udpd_dns_setup(void) {
    // Setup for UDP DNS tests
}

void udpd_dns_teardown(void) {
    // Teardown for UDP DNS tests
}

// ============================================================================
// Test Suite Definition
// ============================================================================

TEST_SUITE_BEGIN(udpd_dns)
    TEST_SUITE_ADD(udpd_dns_request_create)
    TEST_SUITE_ADD(udpd_dns_request_create_invalid_params)
    TEST_SUITE_ADD(udpd_dns_request_cleanup)
    TEST_SUITE_ADD(udpd_dns_request_edge_cases)
    TEST_SUITE_ADD(udpd_dns_resolve_sync_localhost)
    TEST_SUITE_ADD(udpd_dns_resolve_sync_invalid_params)
    TEST_SUITE_ADD(udpd_dns_resolve_sync_various_addresses)
    TEST_SUITE_ADD(udpd_dns_async_request_structure)
    TEST_SUITE_ADD(udpd_dns_request_memory_management)
    TEST_SUITE_ADD(udpd_dns_port_boundaries)
    TEST_SUITE_ADD(udpd_dns_hostname_variations)
    TEST_SUITE_ADD(udpd_dns_initialization_state)
TEST_SUITE_END(udpd_dns)

TEST_SUITE_ADD_NAME(udpd_dns_request_create)
TEST_SUITE_ADD_NAME(udpd_dns_request_create_invalid_params)
TEST_SUITE_ADD_NAME(udpd_dns_request_cleanup)
TEST_SUITE_ADD_NAME(udpd_dns_request_edge_cases)
TEST_SUITE_ADD_NAME(udpd_dns_resolve_sync_localhost)
TEST_SUITE_ADD_NAME(udpd_dns_resolve_sync_invalid_params)
TEST_SUITE_ADD_NAME(udpd_dns_resolve_sync_various_addresses)
TEST_SUITE_ADD_NAME(udpd_dns_async_request_structure)
TEST_SUITE_ADD_NAME(udpd_dns_request_memory_management)
TEST_SUITE_ADD_NAME(udpd_dns_port_boundaries)
TEST_SUITE_ADD_NAME(udpd_dns_hostname_variations)
TEST_SUITE_ADD_NAME(udpd_dns_initialization_state)
TEST_SUITE_FINISH(udpd_dns)