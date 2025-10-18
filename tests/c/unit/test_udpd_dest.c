#include "../framework/test_framework.h"
#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

// ============================================================================
// Test Cases for udpd_dest.c
// ============================================================================

TEST_CASE(udpd_dest_create_from_sockaddr) {
    // Create IPv4 address
    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.1.100", &addr_in.sin_addr);

    udpd_dest_t *dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);

    // Verify address was copied correctly
    TEST_ASSERT_EQUAL(AF_INET, dest->addr.ss_family);
    TEST_ASSERT_EQUAL(sizeof(addr_in), dest->addrlen);
    TEST_ASSERT_EQUAL(8080, dest->port);

    // Verify address content
    struct sockaddr_in *stored = (struct sockaddr_in*)&dest->addr;
    TEST_ASSERT_EQUAL(addr_in.sin_addr.s_addr, stored->sin_addr.s_addr);
    TEST_ASSERT_EQUAL(addr_in.sin_port, stored->sin_port);

    udpd_dest_cleanup(dest);
}

TEST_CASE(udpd_dest_create_invalid_params) {
    // Test NULL address
    udpd_dest_t *dest = udpd_dest_create(NULL, sizeof(struct sockaddr_in));
    TEST_ASSERT_NULL(dest);

    // Test zero length
    struct sockaddr_in addr_in;
    dest = udpd_dest_create((struct sockaddr*)&addr_in, 0);
    TEST_ASSERT_NULL(dest);

    // Test oversized address (should fail)
    char big_addr[1024];  // Much larger than sockaddr_storage
    dest = udpd_dest_create((struct sockaddr*)big_addr, sizeof(big_addr));
    TEST_ASSERT_NULL(dest);
}

TEST_CASE(udpd_dest_create_from_string_ipv4) {
    udpd_dest_t *dest = udpd_dest_create_from_string("127.0.0.1", 3000);
    TEST_ASSERT_NOT_NULL(dest);

    // Verify basic properties
    TEST_ASSERT_EQUAL(3000, dest->port);
    TEST_ASSERT_NOT_NULL(dest->host);
    TEST_ASSERT_STRING_EQUAL("127.0.0.1", dest->host);

    // Verify address was resolved
    TEST_ASSERT_EQUAL(AF_INET, dest->addr.ss_family);
    struct sockaddr_in *addr_in = (struct sockaddr_in*)&dest->addr;
    TEST_ASSERT_EQUAL(htons(3000), addr_in->sin_port);

    // Check that localhost was resolved correctly
    uint32_t expected = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL(expected, addr_in->sin_addr.s_addr);

    udpd_dest_cleanup(dest);
}

TEST_CASE(udpd_dest_create_from_string_invalid) {
    // Test NULL host
    udpd_dest_t *dest = udpd_dest_create_from_string(NULL, 8080);
    TEST_ASSERT_NULL(dest);

    // Test invalid port
    dest = udpd_dest_create_from_string("localhost", 0);
    TEST_ASSERT_NULL(dest);

    dest = udpd_dest_create_from_string("localhost", -1);
    TEST_ASSERT_NULL(dest);

    // Test invalid hostname (should fail DNS resolution)
    dest = udpd_dest_create_from_string("invalid.nonexistent.domain.test", 8080);
    TEST_ASSERT_NULL(dest);
}

TEST_CASE(udpd_dest_cleanup) {
    // Create destination
    udpd_dest_t *dest = udpd_dest_create_from_string("127.0.0.1", 5000);
    TEST_ASSERT_NOT_NULL(dest);

    // Test normal cleanup
    udpd_dest_cleanup(dest);
    // No crash = success

    // Test cleanup with NULL (should not crash)
    udpd_dest_cleanup(NULL);
}

TEST_CASE(udpd_dest_equal) {
    // Create two identical destinations
    struct sockaddr_in addr1, addr2;
    memset(&addr1, 0, sizeof(addr1));
    memset(&addr2, 0, sizeof(addr2));

    addr1.sin_family = AF_INET;
    addr1.sin_port = htons(9000);
    inet_pton(AF_INET, "10.0.0.1", &addr1.sin_addr);

    addr2.sin_family = AF_INET;
    addr2.sin_port = htons(9000);
    inet_pton(AF_INET, "10.0.0.1", &addr2.sin_addr);

    udpd_dest_t *dest1 = udpd_dest_create((struct sockaddr*)&addr1, sizeof(addr1));
    udpd_dest_t *dest2 = udpd_dest_create((struct sockaddr*)&addr2, sizeof(addr2));

    TEST_ASSERT_NOT_NULL(dest1);
    TEST_ASSERT_NOT_NULL(dest2);

    // Test equality
    TEST_ASSERT_EQUAL(1, udpd_dest_equal(dest1, dest2));

    // Test different ports
    addr2.sin_port = htons(9001);
    udpd_dest_cleanup(dest2);
    dest2 = udpd_dest_create((struct sockaddr*)&addr2, sizeof(addr2));
    TEST_ASSERT_EQUAL(0, udpd_dest_equal(dest1, dest2));

    // Test NULL parameters
    TEST_ASSERT_EQUAL(0, udpd_dest_equal(NULL, dest1));
    TEST_ASSERT_EQUAL(0, udpd_dest_equal(dest1, NULL));
    TEST_ASSERT_EQUAL(0, udpd_dest_equal(NULL, NULL));

    udpd_dest_cleanup(dest1);
    udpd_dest_cleanup(dest2);
}

TEST_CASE(udpd_dest_get_host_string) {
    // Test with destination created from string (cached hostname)
    udpd_dest_t *dest = udpd_dest_create_from_string("127.0.0.1", 8080);
    if (dest) {  // Only test if creation succeeded
        const char *host = udpd_dest_get_host_string(dest);
        TEST_ASSERT_NOT_NULL(host);
        TEST_ASSERT_STRING_EQUAL("127.0.0.1", host);  // Should return cached hostname
        udpd_dest_cleanup(dest);
    }

    // Test NULL parameter
    const char *host = udpd_dest_get_host_string(NULL);
    TEST_ASSERT_NULL(host);
}

TEST_CASE(udpd_dest_get_port) {
    // Test with string creation
    udpd_dest_t *dest = udpd_dest_create_from_string("127.0.0.1", 54321);
    if (dest) {
        int port = udpd_dest_get_port(dest);
        TEST_ASSERT_EQUAL(54321, port);
        udpd_dest_cleanup(dest);
    }

    // Test NULL parameter
    int port = udpd_dest_get_port(NULL);
    TEST_ASSERT_EQUAL(0, port);
}

TEST_CASE(udpd_dest_to_string) {
    udpd_dest_t *dest = udpd_dest_create_from_string("10.20.30.40", 6666);
    TEST_ASSERT_NOT_NULL(dest);

    char *str = udpd_dest_to_string(dest);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_STRING_EQUAL("10.20.30.40:6666", str);

    free(str);
    udpd_dest_cleanup(dest);

    // Test NULL parameter
    str = udpd_dest_to_string(NULL);
    TEST_ASSERT_NULL(str);
}

TEST_CASE(udpd_dest_copy) {
    // Create original destination
    udpd_dest_t *original = udpd_dest_create_from_string("192.168.1.1", 4000);
    TEST_ASSERT_NOT_NULL(original);

    // Copy destination
    udpd_dest_t *copy = udpd_dest_copy(original);
    TEST_ASSERT_NOT_NULL(copy);

    // Verify they are equal but different objects
    TEST_ASSERT_EQUAL(1, udpd_dest_equal(original, copy));
    TEST_ASSERT_NOT_EQUAL(original, copy);  // Different pointers

    // Verify hostname was copied (not shared)
    TEST_ASSERT_NOT_NULL(copy->host);
    TEST_ASSERT_STRING_EQUAL(original->host, copy->host);
    TEST_ASSERT_NOT_EQUAL(original->host, copy->host);  // Different string pointers

    // Verify port and address family
    TEST_ASSERT_EQUAL(original->port, copy->port);
    TEST_ASSERT_EQUAL(original->addr.ss_family, copy->addr.ss_family);

    udpd_dest_cleanup(original);
    udpd_dest_cleanup(copy);

    // Test NULL parameter
    copy = udpd_dest_copy(NULL);
    TEST_ASSERT_NULL(copy);
}

TEST_CASE(udpd_dest_is_multicast) {
    // Test multicast address (224.1.1.1)
    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(5000);
    inet_pton(AF_INET, "224.1.1.1", &addr_in.sin_addr);

    udpd_dest_t *dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(1, udpd_dest_is_multicast(dest));
    udpd_dest_cleanup(dest);

    // Test another multicast address (239.255.255.255)
    inet_pton(AF_INET, "239.255.255.255", &addr_in.sin_addr);
    dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(1, udpd_dest_is_multicast(dest));
    udpd_dest_cleanup(dest);

    // Test non-multicast address (192.168.1.1)
    inet_pton(AF_INET, "192.168.1.1", &addr_in.sin_addr);
    dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(0, udpd_dest_is_multicast(dest));
    udpd_dest_cleanup(dest);

    // Test edge case - not multicast (223.255.255.255)
    inet_pton(AF_INET, "223.255.255.255", &addr_in.sin_addr);
    dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(0, udpd_dest_is_multicast(dest));
    udpd_dest_cleanup(dest);

    // Test NULL parameter
    TEST_ASSERT_EQUAL(0, udpd_dest_is_multicast(NULL));
}

TEST_CASE(udpd_dest_is_broadcast) {
    // Test broadcast address (255.255.255.255)
    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(5000);
    addr_in.sin_addr.s_addr = INADDR_BROADCAST;

    udpd_dest_t *dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(1, udpd_dest_is_broadcast(dest));
    udpd_dest_cleanup(dest);

    // Test non-broadcast address
    inet_pton(AF_INET, "192.168.1.255", &addr_in.sin_addr);
    dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(0, udpd_dest_is_broadcast(dest));
    udpd_dest_cleanup(dest);

    // Test NULL parameter
    TEST_ASSERT_EQUAL(0, udpd_dest_is_broadcast(NULL));
}

TEST_CASE(udpd_dest_is_loopback) {
    // Test loopback address (127.0.0.1)
    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(5000);
    inet_pton(AF_INET, "127.0.0.1", &addr_in.sin_addr);

    udpd_dest_t *dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(1, udpd_dest_is_loopback(dest));
    udpd_dest_cleanup(dest);

    // Test another loopback address (127.1.2.3)
    inet_pton(AF_INET, "127.1.2.3", &addr_in.sin_addr);
    dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(1, udpd_dest_is_loopback(dest));
    udpd_dest_cleanup(dest);

    // Test non-loopback address
    inet_pton(AF_INET, "192.168.1.1", &addr_in.sin_addr);
    dest = udpd_dest_create((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(0, udpd_dest_is_loopback(dest));
    udpd_dest_cleanup(dest);

    // Test NULL parameter
    TEST_ASSERT_EQUAL(0, udpd_dest_is_loopback(NULL));
}

TEST_CASE(udpd_dest_edge_cases) {
    // Test minimum and maximum valid ports
    udpd_dest_t *dest = udpd_dest_create_from_string("127.0.0.1", 1);
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(1, dest->port);
    udpd_dest_cleanup(dest);

    dest = udpd_dest_create_from_string("127.0.0.1", 65535);
    TEST_ASSERT_NOT_NULL(dest);
    TEST_ASSERT_EQUAL(65535, dest->port);
    udpd_dest_cleanup(dest);

    // Test different address formats
    dest = udpd_dest_create_from_string("0.0.0.0", 8080);
    TEST_ASSERT_NOT_NULL(dest);
    const char *host = udpd_dest_get_host_string(dest);
    TEST_ASSERT_STRING_EQUAL("0.0.0.0", host);
    udpd_dest_cleanup(dest);
}

TEST_CASE(udpd_dest_memory_management) {
    // Test multiple create/cleanup cycles
    for (int i = 0; i < 100; i++) {
        udpd_dest_t *dest = udpd_dest_create_from_string("127.0.0.1", 8000 + i);
        TEST_ASSERT_NOT_NULL(dest);

        // Verify it works correctly
        TEST_ASSERT_EQUAL(8000 + i, dest->port);

        udpd_dest_cleanup(dest);
    }

    // Test copy/cleanup cycles
    udpd_dest_t *original = udpd_dest_create_from_string("10.0.0.1", 9000);
    TEST_ASSERT_NOT_NULL(original);

    for (int i = 0; i < 50; i++) {
        udpd_dest_t *copy = udpd_dest_copy(original);
        TEST_ASSERT_NOT_NULL(copy);
        TEST_ASSERT_EQUAL(1, udpd_dest_equal(original, copy));
        udpd_dest_cleanup(copy);
    }

    udpd_dest_cleanup(original);
}

// ============================================================================
// Test Suite Setup/Teardown
// ============================================================================

void udpd_dest_setup(void) {
    // Setup for UDP destination tests
}

void udpd_dest_teardown(void) {
    // Teardown for UDP destination tests
}

// ============================================================================
// Test Suite Definition
// ============================================================================

TEST_SUITE_BEGIN(udpd_dest)
    TEST_SUITE_ADD(udpd_dest_create_from_sockaddr)
    TEST_SUITE_ADD(udpd_dest_create_invalid_params)
    TEST_SUITE_ADD(udpd_dest_create_from_string_ipv4)
    TEST_SUITE_ADD(udpd_dest_create_from_string_invalid)
    TEST_SUITE_ADD(udpd_dest_cleanup)
    TEST_SUITE_ADD(udpd_dest_equal)
    TEST_SUITE_ADD(udpd_dest_get_host_string)
    TEST_SUITE_ADD(udpd_dest_get_port)
    TEST_SUITE_ADD(udpd_dest_to_string)
    TEST_SUITE_ADD(udpd_dest_copy)
    TEST_SUITE_ADD(udpd_dest_is_multicast)
    TEST_SUITE_ADD(udpd_dest_is_broadcast)
    TEST_SUITE_ADD(udpd_dest_is_loopback)
    TEST_SUITE_ADD(udpd_dest_edge_cases)
    TEST_SUITE_ADD(udpd_dest_memory_management)
TEST_SUITE_END(udpd_dest)

TEST_SUITE_ADD_NAME(udpd_dest_create_from_sockaddr)
TEST_SUITE_ADD_NAME(udpd_dest_create_invalid_params)
TEST_SUITE_ADD_NAME(udpd_dest_create_from_string_ipv4)
TEST_SUITE_ADD_NAME(udpd_dest_create_from_string_invalid)
TEST_SUITE_ADD_NAME(udpd_dest_cleanup)
TEST_SUITE_ADD_NAME(udpd_dest_equal)
TEST_SUITE_ADD_NAME(udpd_dest_get_host_string)
TEST_SUITE_ADD_NAME(udpd_dest_get_port)
TEST_SUITE_ADD_NAME(udpd_dest_to_string)
TEST_SUITE_ADD_NAME(udpd_dest_copy)
TEST_SUITE_ADD_NAME(udpd_dest_is_multicast)
TEST_SUITE_ADD_NAME(udpd_dest_is_broadcast)
TEST_SUITE_ADD_NAME(udpd_dest_is_loopback)
TEST_SUITE_ADD_NAME(udpd_dest_edge_cases)
TEST_SUITE_ADD_NAME(udpd_dest_memory_management)
TEST_SUITE_FINISH(udpd_dest)