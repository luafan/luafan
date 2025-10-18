#include "../framework/test_framework.h"
#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Mock Lua state for testing
static lua_State *mock_L = NULL;

// Helper to create a mock Lua state (local version)
static lua_State *create_mock_lua_state_udpd() {
    // For testing purposes, we'll use NULL as placeholder
    // In real code, this would be a proper Lua state
    return (lua_State *)0x12345678;  // Mock pointer
}

// Helper to create mock UDP connection
typedef struct {
    udpd_base_conn_t base;
} udpd_conn_t;

// ============================================================================
// Test Cases for udpd.c (Main UDP functionality)
// ============================================================================

TEST_CASE(udpd_conn_structure_initialization) {
    // Test that UDP connection structure inherits properly from base
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    int result = udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);
    TEST_ASSERT_EQUAL(0, result);

    // Verify inheritance structure
    TEST_ASSERT_EQUAL(UDPD_CONN_TYPE_CLIENT, conn.base.type);
    TEST_ASSERT_EQUAL(L, conn.base.mainthread);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.base.state);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_connection_lifecycle) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();

    // Test complete lifecycle: init -> create -> bind -> setup -> cleanup
    int result = udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_SERVER, L);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.base.state);

    // Create socket
    result = udpd_base_conn_create_socket(&conn.base);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_BINDING, conn.base.state);
    TEST_ASSERT_TRUE(conn.base.socket_fd >= 0);

    // Bind to any port
    conn.base.bind_port = 0;  // Let system choose
    result = udpd_base_conn_bind(&conn.base);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_BOUND, conn.base.state);
    TEST_ASSERT_TRUE(conn.base.bind_port > 0);

    // Setup events (no callbacks)
    result = udpd_base_conn_setup_events(&conn.base);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_READY, conn.base.state);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.base.state);
    TEST_ASSERT_EQUAL(-1, conn.base.socket_fd);
}

TEST_CASE(udpd_client_connection_setup) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);

    // Set up client connection with target host
    conn.base.host = strdup("127.0.0.1");
    conn.base.port = 8080;

    // Create address for target
    int result = udpd_create_address_from_string(conn.base.host, conn.base.port,
                                               &conn.base.addr, &conn.base.addrlen);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(AF_INET, conn.base.addr.ss_family);

    // Create and setup socket
    result = udpd_base_conn_create_socket(&conn.base);
    TEST_ASSERT_EQUAL(0, result);

    result = udpd_base_conn_setup_events(&conn.base);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_READY, conn.base.state);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_server_connection_setup) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_SERVER, L);

    // Set up server connection
    conn.base.bind_host = strdup("0.0.0.0");
    conn.base.bind_port = 0;  // System chooses port

    // Create, bind and setup socket
    int result = udpd_base_conn_create_socket(&conn.base);
    TEST_ASSERT_EQUAL(0, result);

    result = udpd_base_conn_bind(&conn.base);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(conn.base.bind_port > 0);

    result = udpd_base_conn_setup_events(&conn.base);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_READY, conn.base.state);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_broadcast_connection_setup) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_BROADCAST, L);

    // Enable broadcast in configuration
    conn.base.config.broadcast_enabled = 1;

    // Create and setup socket
    int result = udpd_base_conn_create_socket(&conn.base);
    TEST_ASSERT_EQUAL(0, result);

    // Verify socket has broadcast option set
    int broadcast_val;
    socklen_t opt_len = sizeof(broadcast_val);
    result = getsockopt(conn.base.socket_fd, SOL_SOCKET, SO_BROADCAST,
                       &broadcast_val, &opt_len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, broadcast_val);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_socket_options_application) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);

    // Configure socket options
    conn.base.config.base.send_buffer_size = 32768;
    conn.base.config.base.receive_buffer_size = 32768;
    conn.base.config.reuse_addr = 1;

    // Create socket and apply options
    int result = udpd_base_conn_create_socket(&conn.base);
    TEST_ASSERT_EQUAL(0, result);

    // Verify buffer sizes were set
    int send_buf, recv_buf;
    socklen_t opt_len = sizeof(int);

    result = getsockopt(conn.base.socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buf, &opt_len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(send_buf >= 32768);

    result = getsockopt(conn.base.socket_fd, SOL_SOCKET, SO_RCVBUF, &recv_buf, &opt_len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(recv_buf >= 32768);

    // Verify reuse address option
    int reuse_val;
    result = getsockopt(conn.base.socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_val, &opt_len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, reuse_val);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_packet_size_validation) {
    // Test packet size validation function
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(1));
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(1024));
    TEST_ASSERT_EQUAL(1, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE));

    // Invalid sizes
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(0));
    TEST_ASSERT_EQUAL(0, udpd_validate_packet_size(UDPD_MAX_PACKET_SIZE + 1));
}

TEST_CASE(udpd_send_data_basic) {
    udpd_conn_t server_conn, client_conn;
    memset(&server_conn, 0, sizeof(server_conn));
    memset(&client_conn, 0, sizeof(client_conn));

    lua_State *L = create_mock_lua_state_udpd();

    // Set up server
    udpd_base_conn_init(&server_conn.base, UDPD_CONN_TYPE_SERVER, L);
    server_conn.base.bind_port = 0;
    udpd_base_conn_create_socket(&server_conn.base);
    udpd_base_conn_bind(&server_conn.base);
    int server_port = udpd_get_socket_port(server_conn.base.socket_fd);
    TEST_ASSERT_TRUE(server_port > 0);

    // Set up client
    udpd_base_conn_init(&client_conn.base, UDPD_CONN_TYPE_CLIENT, L);
    client_conn.base.host = strdup("127.0.0.1");
    client_conn.base.port = server_port;
    udpd_create_address_from_string(client_conn.base.host, client_conn.base.port,
                                   &client_conn.base.addr, &client_conn.base.addrlen);
    udpd_base_conn_create_socket(&client_conn.base);

    // Send data from client to server
    const char *test_data = "Hello UDP!";
    ssize_t sent = sendto(client_conn.base.socket_fd, test_data, strlen(test_data), 0,
                         (struct sockaddr *)&client_conn.base.addr, client_conn.base.addrlen);
    TEST_ASSERT_TRUE(sent > 0);
    TEST_ASSERT_EQUAL(strlen(test_data), sent);

    // Receive data on server
    char recv_buffer[1024];
    struct sockaddr_storage from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t received = recvfrom(server_conn.base.socket_fd, recv_buffer, sizeof(recv_buffer), 0,
                               (struct sockaddr *)&from_addr, &from_len);
    TEST_ASSERT_TRUE(received > 0);
    TEST_ASSERT_EQUAL(strlen(test_data), received);

    recv_buffer[received] = '\0';
    TEST_ASSERT_STRING_EQUAL(test_data, recv_buffer);

    // Clean up
    server_conn.base.mainthread = NULL;
    client_conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&server_conn.base);
    udpd_base_conn_cleanup(&client_conn.base);
}

TEST_CASE(udpd_connection_info_formatting) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);

    conn.base.host = strdup("example.com");
    conn.base.port = 8080;
    conn.base.bind_host = strdup("0.0.0.0");
    conn.base.bind_port = 12345;
    conn.base.socket_fd = 5;
    conn.base.state = UDPD_CONN_READY;

    char *info = udpd_format_connection_info(&conn.base);
    TEST_ASSERT_NOT_NULL(info);

    // Check that info contains expected components
    TEST_ASSERT_TRUE(strstr(info, "UDP") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "example.com") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "8080") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "0.0.0.0") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "12345") != NULL);
    TEST_ASSERT_TRUE(strstr(info, "fd=5") != NULL);

    free(info);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_error_handling) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);

    // Test error creation for bind failure
    tcpd_error_t error = udpd_error_bind_failed("127.0.0.1", 8080);
    TEST_ASSERT_EQUAL(TCPD_ERROR_CONNECTION_RESET, error.type);
    TEST_ASSERT_NOT_NULL(error.message);
    TEST_ASSERT_TRUE(strstr(error.message, "127.0.0.1") != NULL);
    TEST_ASSERT_TRUE(strstr(error.message, "8080") != NULL);

    if (error.message) {
        free(error.message);
    }

    // Test socket error conversion
    error = udpd_error_from_socket_error(ECONNRESET);
    TEST_ASSERT_EQUAL(TCPD_ERROR_CONNECTION_RESET, error.type);
    tcpd_error_cleanup(&error);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_multiple_connections) {
    // Test multiple UDP connections
    const int num_connections = 5;
    udpd_conn_t connections[num_connections];
    lua_State *L = create_mock_lua_state_udpd();

    // Create multiple server connections
    for (int i = 0; i < num_connections; i++) {
        memset(&connections[i], 0, sizeof(connections[i]));
        udpd_base_conn_init(&connections[i].base, UDPD_CONN_TYPE_SERVER, L);
        connections[i].base.bind_port = 0;  // Let system choose

        int result = udpd_base_conn_create_socket(&connections[i].base);
        TEST_ASSERT_EQUAL(0, result);

        result = udpd_base_conn_bind(&connections[i].base);
        TEST_ASSERT_EQUAL(0, result);

        int port = udpd_get_socket_port(connections[i].base.socket_fd);
        TEST_ASSERT_TRUE(port > 0);
        connections[i].base.bind_port = port;
    }

    // Verify all connections have different ports
    for (int i = 0; i < num_connections; i++) {
        for (int j = i + 1; j < num_connections; j++) {
            TEST_ASSERT_NOT_EQUAL(connections[i].base.bind_port,
                                connections[j].base.bind_port);
        }
    }

    // Clean up all connections
    for (int i = 0; i < num_connections; i++) {
        connections[i].base.mainthread = NULL;
        udpd_base_conn_cleanup(&connections[i].base);
    }
}

TEST_CASE(udpd_configuration_integration) {
    udpd_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    lua_State *L = create_mock_lua_state_udpd();
    udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);

    // Test configuration integration
    udpd_config_set_defaults(&conn.base.config);
    TEST_ASSERT_TRUE(conn.base.config.base.send_buffer_size > 0);
    TEST_ASSERT_TRUE(conn.base.config.base.receive_buffer_size > 0);

    // Test configuration validation
    int result = udpd_config_validate(&conn.base.config);
    TEST_ASSERT_EQUAL(0, result);

    // Clean up
    conn.base.mainthread = NULL;
    udpd_base_conn_cleanup(&conn.base);
}

TEST_CASE(udpd_memory_stress_test) {
    // Test memory management under stress
    lua_State *L = create_mock_lua_state_udpd();

    for (int i = 0; i < 100; i++) {
        udpd_conn_t conn;
        memset(&conn, 0, sizeof(conn));

        udpd_base_conn_init(&conn.base, UDPD_CONN_TYPE_CLIENT, L);
        conn.base.host = strdup("127.0.0.1");
        conn.base.port = 8000 + i;

        if (udpd_base_conn_create_socket(&conn.base) == 0) {
            TEST_ASSERT_TRUE(conn.base.socket_fd >= 0);
        }

        conn.base.mainthread = NULL;
        udpd_base_conn_cleanup(&conn.base);
    }
}

// ============================================================================
// Test Suite Setup/Teardown
// ============================================================================

void udpd_setup(void) {
    // Setup for main UDP tests
    mock_L = create_mock_lua_state_udpd();
}

void udpd_teardown(void) {
    // Teardown for main UDP tests
    mock_L = NULL;
}

// ============================================================================
// Test Suite Definition
// ============================================================================

TEST_SUITE_BEGIN(udpd)
    TEST_SUITE_ADD(udpd_conn_structure_initialization)
    TEST_SUITE_ADD(udpd_connection_lifecycle)
    TEST_SUITE_ADD(udpd_client_connection_setup)
    TEST_SUITE_ADD(udpd_server_connection_setup)
    TEST_SUITE_ADD(udpd_broadcast_connection_setup)
    TEST_SUITE_ADD(udpd_socket_options_application)
    TEST_SUITE_ADD(udpd_packet_size_validation)
    TEST_SUITE_ADD(udpd_send_data_basic)
    TEST_SUITE_ADD(udpd_connection_info_formatting)
    TEST_SUITE_ADD(udpd_error_handling)
    TEST_SUITE_ADD(udpd_multiple_connections)
    TEST_SUITE_ADD(udpd_configuration_integration)
    TEST_SUITE_ADD(udpd_memory_stress_test)
TEST_SUITE_END(udpd)

TEST_SUITE_ADD_NAME(udpd_conn_structure_initialization)
TEST_SUITE_ADD_NAME(udpd_connection_lifecycle)
TEST_SUITE_ADD_NAME(udpd_client_connection_setup)
TEST_SUITE_ADD_NAME(udpd_server_connection_setup)
TEST_SUITE_ADD_NAME(udpd_broadcast_connection_setup)
TEST_SUITE_ADD_NAME(udpd_socket_options_application)
TEST_SUITE_ADD_NAME(udpd_packet_size_validation)
TEST_SUITE_ADD_NAME(udpd_send_data_basic)
TEST_SUITE_ADD_NAME(udpd_connection_info_formatting)
TEST_SUITE_ADD_NAME(udpd_error_handling)
TEST_SUITE_ADD_NAME(udpd_multiple_connections)
TEST_SUITE_ADD_NAME(udpd_configuration_integration)
TEST_SUITE_ADD_NAME(udpd_memory_stress_test)
TEST_SUITE_FINISH(udpd)