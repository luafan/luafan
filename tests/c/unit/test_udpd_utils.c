#include "../framework/test_framework.h"
#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

// ============================================================================
// Test Cases for udpd_utils.c
// ============================================================================

TEST_CASE(udpd_validate_packet_size) {
    // Test valid packet sizes
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(1));
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(100));
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(1024));
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE));

    // Test invalid packet sizes
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(0));
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE + 1));
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(SIZE_MAX));

    // Test edge case
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE - 1));
}

TEST_CASE(udpd_is_address_family_supported) {
    // Test supported families
    TEST_ASSERT_EQUAL(1, udpd_is_address_family_supported(AF_INET));
    TEST_ASSERT_EQUAL(1, udpd_is_address_family_supported(AF_INET6));

    // Test unsupported families
    TEST_ASSERT_EQUAL(0, udpd_is_address_family_supported(AF_UNIX));
    TEST_ASSERT_EQUAL(0, udpd_is_address_family_supported(AF_UNSPEC));
    TEST_ASSERT_EQUAL(0, udpd_is_address_family_supported(-1));
    TEST_ASSERT_EQUAL(0, udpd_is_address_family_supported(999));
}

TEST_CASE(udpd_is_ip_address) {
    // Test valid IPv4 addresses
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("127.0.0.1"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("192.168.1.1"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("0.0.0.0"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("255.255.255.255"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("10.0.0.1"));

    // Test valid IPv6 addresses
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("::1"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("::"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("2001:db8::1"));
    TEST_ASSERT_EQUAL(1, udpd_is_ip_address("fe80::1"));

    // Test invalid addresses
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address("example.com"));
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address("localhost"));
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address("not.an.ip"));
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address("256.1.1.1"));  // Invalid IPv4
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address("1.2.3"));       // Incomplete IPv4
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address(""));
    TEST_ASSERT_EQUAL(0, udpd_is_ip_address(NULL));
}

TEST_CASE(udpd_create_address_from_string_ipv4) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    // Test valid IPv4 address
    int result = udpd_create_address_from_string("192.168.1.100", 8080, &addr, &addrlen);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(AF_INET, addr.ss_family);
    TEST_ASSERT_EQUAL(sizeof(struct sockaddr_in), addrlen);

    struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
    TEST_ASSERT_EQUAL(htons(8080), addr_in->sin_port);

    // Verify IP address
    char ip_str[INET_ADDRSTRLEN];
    const char *converted = inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
    TEST_ASSERT_NOT_NULL(converted);
    TEST_ASSERT_STRING_EQUAL("192.168.1.100", converted);
}

TEST_CASE(udpd_create_address_from_string_invalid_params) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    // Test NULL host
    int result = udpd_create_address_from_string(NULL, 8080, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    // Test NULL addr
    result = udpd_create_address_from_string("127.0.0.1", 8080, NULL, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    // Test NULL addrlen
    result = udpd_create_address_from_string("127.0.0.1", 8080, &addr, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid port
    result = udpd_create_address_from_string("127.0.0.1", 0, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    result = udpd_create_address_from_string("127.0.0.1", -1, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    // Test non-IP hostname
    result = udpd_create_address_from_string("example.com", 8080, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_sockaddr_to_string_ipv4) {
    // Create IPv4 address
    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(9999);
    inet_pton(AF_INET, "10.20.30.40", &addr_in.sin_addr);

    char *str = udpd_sockaddr_to_string((struct sockaddr*)&addr_in, sizeof(addr_in));
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_STRING_EQUAL("10.20.30.40:9999", str);

    free(str);
}

TEST_CASE(udpd_sockaddr_to_string_invalid) {
    // Test NULL address
    char *str = udpd_sockaddr_to_string(NULL, sizeof(struct sockaddr_in));
    TEST_ASSERT_NULL(str);

    // Test unsupported address family
    struct sockaddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.sa_family = AF_UNIX;

    str = udpd_sockaddr_to_string(&addr, sizeof(addr));
    TEST_ASSERT_NULL(str);
}

TEST_CASE(udpd_socket_set_nonblock_invalid) {
    // Test invalid file descriptor
    int result = udpd_socket_set_nonblock(-1);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_socket_set_reuse_addr_invalid) {
    // Test invalid file descriptor
    int result = udpd_socket_set_reuse_addr(-1);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_socket_set_broadcast_invalid) {
    // Test invalid file descriptor
    int result = udpd_socket_set_broadcast(-1);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_socket_bind_interface_invalid) {
    // Test invalid file descriptor
    int result = udpd_socket_bind_interface(-1, 1);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid interface
    result = udpd_socket_bind_interface(1, 0);
    TEST_ASSERT_EQUAL(-1, result);

    result = udpd_socket_bind_interface(1, -1);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_get_socket_port_invalid) {
    // Test invalid file descriptor
    int port = udpd_get_socket_port(-1);
    TEST_ASSERT_EQUAL(0, port);
}

TEST_CASE(udpd_error_bind_failed) {
    tcpd_error_t error = udpd_error_bind_failed("127.0.0.1", 8080);

    TEST_ASSERT_EQUAL(TCPD_ERROR_CONNECTION_RESET, error.type);
    TEST_ASSERT_NOT_NULL(error.message);

    // Check that message contains expected content
    TEST_ASSERT_TRUE(strstr(error.message, "127.0.0.1") != NULL);
    TEST_ASSERT_TRUE(strstr(error.message, "8080") != NULL);
    TEST_ASSERT_TRUE(strstr(error.message, "Failed to bind") != NULL);

    if (error.message) {
        free(error.message);
    }
}

TEST_CASE(udpd_format_connection_info) {
    // Create a mock connection structure
    udpd_base_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    conn.host = strdup("example.com");
    conn.port = 8080;
    conn.bind_host = strdup("0.0.0.0");
    conn.bind_port = 12345;
    conn.socket_fd = 5;
    conn.state = UDPD_CONN_READY;

    char *info = udpd_format_connection_info(&conn);
    TEST_ASSERT_NOT_NULL(info);

    // Check that info contains expected components
    TEST_ASSERT_TRUE(strstr(info, "UDP") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "example.com") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "8080") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "0.0.0.0") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "12345") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "fd=5") != NULL);

    free(info);
    free(conn.host);
    free(conn.bind_host);

    // Test with NULL connection
    info = udpd_format_connection_info(NULL);
    TEST_ASSERT_NULL(info);
}

TEST_CASE(udpd_format_connection_info_minimal) {
    // Test with minimal connection info (NULL hosts)
    udpd_base_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    conn.host = NULL;
    conn.port = 80;
    conn.bind_host = NULL;
    conn.bind_port = 0;
    conn.socket_fd = -1;
    conn.state = UDPD_CONN_DISCONNECTED;

    char *info = udpd_format_connection_info(&conn);
    TEST_ASSERT_NOT_NULL(info);

    // Should contain default values
    TEST_ASSERT_TRUE(strstr(info, "unknown") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "any") != NULL);

    free(info);
}

TEST_CASE(udpd_create_address_edge_cases) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    // Test edge case IPv4 addresses
    const char *edge_ips[] = {
        "0.0.0.0",
        "127.0.0.1",
        "255.255.255.255",
        "224.0.0.1",  // Multicast
        NULL
    };

    for (int i = 0; edge_ips[i]; i++) {
        int result = udpd_create_address_from_string(edge_ips[i], 1234, &addr, &addrlen);
        TEST_ASSERT_EQUAL(0, result);
        TEST_ASSERT_EQUAL(AF_INET, addr.ss_family);

        struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
        TEST_ASSERT_EQUAL(htons(1234), addr_in->sin_port);
    }
}

TEST_CASE(udpd_utils_memory_management) {
    // Test memory allocation/deallocation cycles for string functions
    for (int i = 0; i < 50; i++) {
        // Test sockaddr_to_string
        struct sockaddr_in addr_in;
        memset(&addr_in, 0, sizeof(addr_in));
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(8000 + i);
        inet_pton(AF_INET, "192.168.1.1", &addr_in.sin_addr);

        char *str = udpd_sockaddr_to_string((struct sockaddr*)&addr_in, sizeof(addr_in));
        TEST_ASSERT_NOT_NULL(str);
        free(str);

        // Test format_connection_info
        udpd_base_conn_t conn;
        memset(&conn, 0, sizeof(conn));
        conn.host = strdup("test.com");
        conn.port = 9000 + i;
        conn.socket_fd = i;

        char *info = udpd_format_connection_info(&conn);
        TEST_ASSERT_NOT_NULL(info);
        free(info);
        free(conn.host);

        // Test error message
        tcpd_error_t error = udpd_error_bind_failed("localhost", 3000 + i);
        TEST_ASSERT_NOT_NULL(error.message);
        free(error.message);
    }
}

TEST_CASE(udpd_utils_boundary_values) {
    // Test packet size boundaries
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(1));
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE));
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(0));
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE + 1));

    // Test port boundaries in address creation
    struct sockaddr_storage addr;
    socklen_t addrlen;

    int result = udpd_create_address_from_string("127.0.0.1", 1, &addr, &addrlen);
    TEST_ASSERT_EQUAL(0, result);

    result = udpd_create_address_from_string("127.0.0.1", 65535, &addr, &addrlen);
    TEST_ASSERT_EQUAL(0, result);

    result = udpd_create_address_from_string("127.0.0.1", 0, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);

    result = udpd_create_address_from_string("127.0.0.1", 65536, &addr, &addrlen);
    TEST_ASSERT_EQUAL(-1, result);
}

// ============================================================================
// Test Suite Setup/Teardown
// ============================================================================

void udpd_utils_setup(void) {
    // Setup for UDP utils tests
}

void udpd_utils_teardown(void) {
    // Teardown for UDP utils tests
}

// ============================================================================
// Test Suite Definition
// ============================================================================

TEST_SUITE_BEGIN(udpd_utils)
    TEST_SUITE_ADD(udpd_validate_packet_size)
    TEST_SUITE_ADD(udpd_is_address_family_supported)
    TEST_SUITE_ADD(udpd_is_ip_address)
    TEST_SUITE_ADD(udpd_create_address_from_string_ipv4)
    TEST_SUITE_ADD(udpd_create_address_from_string_invalid_params)
    TEST_SUITE_ADD(udpd_sockaddr_to_string_ipv4)
    TEST_SUITE_ADD(udpd_sockaddr_to_string_invalid)
    TEST_SUITE_ADD(udpd_socket_set_nonblock_invalid)
    TEST_SUITE_ADD(udpd_socket_set_reuse_addr_invalid)
    TEST_SUITE_ADD(udpd_socket_set_broadcast_invalid)
    TEST_SUITE_ADD(udpd_socket_bind_interface_invalid)
    TEST_SUITE_ADD(udpd_get_socket_port_invalid)
    TEST_SUITE_ADD(udpd_error_bind_failed)
    TEST_SUITE_ADD(udpd_format_connection_info)
    TEST_SUITE_ADD(udpd_format_connection_info_minimal)
    TEST_SUITE_ADD(udpd_create_address_edge_cases)
    TEST_SUITE_ADD(udpd_utils_memory_management)
    TEST_SUITE_ADD(udpd_utils_boundary_values)
TEST_SUITE_END(udpd_utils)

TEST_SUITE_ADD_NAME(udpd_validate_packet_size)
TEST_SUITE_ADD_NAME(udpd_is_address_family_supported)
TEST_SUITE_ADD_NAME(udpd_is_ip_address)
TEST_SUITE_ADD_NAME(udpd_create_address_from_string_ipv4)
TEST_SUITE_ADD_NAME(udpd_create_address_from_string_invalid_params)
TEST_SUITE_ADD_NAME(udpd_sockaddr_to_string_ipv4)
TEST_SUITE_ADD_NAME(udpd_sockaddr_to_string_invalid)
TEST_SUITE_ADD_NAME(udpd_socket_set_nonblock_invalid)
TEST_SUITE_ADD_NAME(udpd_socket_set_reuse_addr_invalid)
TEST_SUITE_ADD_NAME(udpd_socket_set_broadcast_invalid)
TEST_SUITE_ADD_NAME(udpd_socket_bind_interface_invalid)
TEST_SUITE_ADD_NAME(udpd_get_socket_port_invalid)
TEST_SUITE_ADD_NAME(udpd_error_bind_failed)
TEST_SUITE_ADD_NAME(udpd_format_connection_info)
TEST_SUITE_ADD_NAME(udpd_format_connection_info_minimal)
TEST_SUITE_ADD_NAME(udpd_create_address_edge_cases)
TEST_SUITE_ADD_NAME(udpd_utils_memory_management)
TEST_SUITE_ADD_NAME(udpd_utils_boundary_values)
TEST_SUITE_FINISH(udpd_utils)