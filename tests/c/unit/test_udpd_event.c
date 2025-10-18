#include "../framework/test_framework.h"
#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Mock Lua state for testing
static lua_State *mock_L = NULL;

// Helper to create a mock Lua state
lua_State *create_mock_lua_state() {
    // For testing purposes, we'll use NULL as placeholder
    // In real code, this would be a proper Lua state
    return (lua_State *)0x12345678;  // Mock pointer
}

// ============================================================================
// Test Cases for udpd_event.c
// ============================================================================

TEST_CASE(udpd_base_conn_init) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Test successful initialization
    int result = udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);
    TEST_ASSERT_EQUAL(0, result);

    // Verify initialization values
    TEST_ASSERT_EQUAL(-1, conn.socket_fd);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.state);
    TEST_ASSERT_EQUAL(UDPD_CONN_TYPE_CLIENT, conn.type);
    TEST_ASSERT_EQUAL(L, conn.mainthread);
    TEST_ASSERT_EQUAL(LUA_NOREF, conn.selfRef);
    TEST_ASSERT_EQUAL(LUA_NOREF, conn.onReadRef);
    TEST_ASSERT_EQUAL(LUA_NOREF, conn.onSendReadyRef);

    // Verify string fields are NULL
    TEST_ASSERT_NULL(conn.host);
    TEST_ASSERT_NULL(conn.bind_host);
    TEST_ASSERT_EQUAL(0, conn.port);
    TEST_ASSERT_EQUAL(0, conn.bind_port);
    TEST_ASSERT_EQUAL(0, conn.interface);

    // Verify event pointers are NULL
    TEST_ASSERT_NULL(conn.read_ev);
    TEST_ASSERT_NULL(conn.write_ev);
    TEST_ASSERT_NULL(conn.dns_request);

    // Clean up (skip Lua cleanup for mock)
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_init_invalid_params) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Test NULL connection
    int result = udpd_base_conn_init(NULL, UDPD_CONN_TYPE_CLIENT, L);
    TEST_ASSERT_EQUAL(-1, result);

    // Test NULL Lua state
    result = udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_base_conn_create_socket) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize connection
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);

    // Test socket creation
    int result = udpd_base_conn_create_socket(&conn);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(conn.socket_fd >= 0);
    TEST_ASSERT_EQUAL(UDPD_CONN_BINDING, conn.state);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_create_socket_invalid) {
    // Test NULL connection
    int result = udpd_base_conn_create_socket(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_base_conn_bind_to_any) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize connection
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);
    conn.bind_port = 0;  // Let system choose port

    // Create socket
    int result = udpd_base_conn_create_socket(&conn);
    TEST_ASSERT_EQUAL(0, result);

    // Test bind to INADDR_ANY
    result = udpd_base_conn_bind(&conn);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_BOUND, conn.state);
    TEST_ASSERT_TRUE(conn.bind_port > 0);  // Should have assigned port

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_bind_invalid_params) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize connection but don't create socket
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);

    // Test bind without socket
    int result = udpd_base_conn_bind(&conn);
    TEST_ASSERT_EQUAL(-1, result);

    // Test NULL connection
    result = udpd_base_conn_bind(NULL);
    TEST_ASSERT_EQUAL(-1, result);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_setup_events_no_callbacks) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize and create socket
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);
    udpd_base_conn_create_socket(&conn);
    udpd_base_conn_bind(&conn);

    // Test event setup without callbacks
    int result = udpd_base_conn_setup_events(&conn);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(UDPD_CONN_READY, conn.state);
    TEST_ASSERT_NULL(conn.read_ev);  // No callback, no event
    TEST_ASSERT_NULL(conn.write_ev); // No callback, no event

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_setup_events_invalid) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Test NULL connection
    int result = udpd_base_conn_setup_events(NULL);
    TEST_ASSERT_EQUAL(-1, result);

    // Test connection without socket
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);
    result = udpd_base_conn_setup_events(&conn);
    TEST_ASSERT_EQUAL(-1, result);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_request_send_ready_invalid) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Test NULL connection
    int result = udpd_base_conn_request_send_ready(NULL);
    TEST_ASSERT_EQUAL(-1, result);

    // Test connection without write event
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);
    result = udpd_base_conn_request_send_ready(&conn);
    TEST_ASSERT_EQUAL(-1, result);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_cleanup_null) {
    // Test cleanup with NULL (should not crash)
    udpd_base_conn_cleanup(NULL);
    // No crash = success
}

TEST_CASE(udpd_base_conn_cleanup_initialized) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize with allocated strings
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);
    conn.host = strdup("test.example.com");
    conn.bind_host = strdup("0.0.0.0");

    // Test cleanup
    conn.mainthread = NULL;  // Skip Lua cleanup for mock
    udpd_base_conn_cleanup(&conn);

    // Verify cleanup
    TEST_ASSERT_EQUAL(-1, conn.socket_fd);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.state);
    TEST_ASSERT_NULL(conn.host);
    TEST_ASSERT_NULL(conn.bind_host);
    TEST_ASSERT_NULL(conn.read_ev);
    TEST_ASSERT_NULL(conn.write_ev);
    TEST_ASSERT_NULL(conn.dns_request);
}

TEST_CASE(udpd_connection_cleanup_on_disconnect) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize connection
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);
    conn.state = UDPD_CONN_READY;

    // Test disconnect cleanup
    udpd_connection_cleanup_on_disconnect(&conn);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.state);

    // Test with NULL (should not crash)
    udpd_connection_cleanup_on_disconnect(NULL);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_process_received_data_invalid_params) {
    udpd_base_conn_t conn;
    struct sockaddr_storage addr;
    const char *data = "test data";

    // Test NULL connection
    udpd_process_received_data(NULL, data, 9, &addr, sizeof(addr));
    // No crash = success

    // Test NULL data
    udpd_process_received_data(&conn, NULL, 9, &addr, sizeof(addr));
    // No crash = success

    // Test zero length
    udpd_process_received_data(&conn, data, 0, &addr, sizeof(addr));
    // No crash = success
}

TEST_CASE(udpd_handle_read_error) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize connection
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);
    conn.socket_fd = 5;  // Mock socket fd
    conn.state = UDPD_CONN_READY;

    // Test error handling
    udpd_handle_read_error(&conn, ECONNRESET);
    TEST_ASSERT_EQUAL(UDPD_CONN_ERROR, conn.state);

    // Test with NULL (should not crash)
    udpd_handle_read_error(NULL, EAGAIN);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_common_readcb_invalid_params) {
    // Test callback with NULL context
    udpd_common_readcb(1, EV_READ, NULL);
    // No crash = success

    // Test callback with connection without callback
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);

    udpd_common_readcb(1, EV_READ, &conn);
    // No crash = success

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_common_writecb_invalid_params) {
    // Test callback with NULL context
    udpd_common_writecb(1, EV_WRITE, NULL);
    // No crash = success

    // Test callback with connection without callback
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);

    udpd_common_writecb(1, EV_WRITE, &conn);
    // No crash = success

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_state_transitions) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Test state transitions through connection lifecycle
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.state);

    // Create socket
    udpd_base_conn_create_socket(&conn);
    TEST_ASSERT_EQUAL(UDPD_CONN_BINDING, conn.state);

    // Bind socket
    udpd_base_conn_bind(&conn);
    TEST_ASSERT_EQUAL(UDPD_CONN_BOUND, conn.state);

    // Setup events
    udpd_base_conn_setup_events(&conn);
    TEST_ASSERT_EQUAL(UDPD_CONN_READY, conn.state);

    // Disconnect
    udpd_connection_cleanup_on_disconnect(&conn);
    TEST_ASSERT_EQUAL(UDPD_CONN_DISCONNECTED, conn.state);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_socket_operations) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Initialize connection
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);

    // Test complete socket setup process
    int result = udpd_base_conn_create_socket(&conn);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(conn.socket_fd >= 0);

    // Verify socket type is UDP
    int socktype;
    socklen_t len = sizeof(socktype);
    result = getsockopt(conn.socket_fd, SOL_SOCKET, SO_TYPE, &socktype, &len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(SOCK_DGRAM, socktype);

    // Test bind
    conn.bind_port = 0;  // Let system choose
    result = udpd_base_conn_bind(&conn);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(conn.bind_port > 0);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

TEST_CASE(udpd_base_conn_memory_management) {
    // Test multiple init/cleanup cycles
    for (int i = 0; i < 50; i++) {
        udpd_base_conn_t conn;
        lua_State *L = create_mock_lua_state();

        udpd_base_conn_init(&conn, UDPD_CONN_TYPE_CLIENT, L);

        // Add some allocated strings
        conn.host = strdup("test.example.com");
        conn.bind_host = strdup("localhost");

        // Create socket to test file descriptor cleanup
        if (udpd_base_conn_create_socket(&conn) == 0) {
            TEST_ASSERT_TRUE(conn.socket_fd >= 0);
        }

        // Clean up (skip Lua cleanup for mock)
        conn.mainthread = NULL;
        udpd_base_conn_cleanup(&conn);

        // Verify cleanup
        TEST_ASSERT_NULL(conn.host);
        TEST_ASSERT_NULL(conn.bind_host);
        TEST_ASSERT_EQUAL(-1, conn.socket_fd);
    }
}

TEST_CASE(udpd_base_conn_edge_cases) {
    udpd_base_conn_t conn;
    lua_State *L = create_mock_lua_state();

    // Test initialization with different connection types
    udpd_conn_type_t types[] = {
        UDPD_CONN_TYPE_CLIENT,
        UDPD_CONN_TYPE_SERVER,
        UDPD_CONN_TYPE_BROADCAST
    };

    for (int i = 0; i < 3; i++) {
        udpd_base_conn_init(&conn, types[i], L);
        TEST_ASSERT_EQUAL(types[i], conn.type);

        conn.mainthread = NULL;
        udpd_base_conn_cleanup(&conn);
    }

    // Test bind with specific port
    udpd_base_conn_init(&conn, UDPD_CONN_TYPE_SERVER, L);
    udpd_base_conn_create_socket(&conn);

    conn.bind_port = 0;  // System chooses port
    int result = udpd_base_conn_bind(&conn);
    TEST_ASSERT_EQUAL(0, result);

    int chosen_port = conn.bind_port;
    TEST_ASSERT_TRUE(chosen_port > 0);

    // Clean up
    conn.mainthread = NULL;
    udpd_base_conn_cleanup(&conn);
}

// ============================================================================
// Test Suite Setup/Teardown
// ============================================================================

void udpd_event_setup(void) {
    // Setup for UDP event tests
    mock_L = create_mock_lua_state();
}

void udpd_event_teardown(void) {
    // Teardown for UDP event tests
    mock_L = NULL;
}

// ============================================================================
// Test Suite Definition
// ============================================================================

TEST_SUITE_BEGIN(udpd_event)
    TEST_SUITE_ADD(udpd_base_conn_init)
    TEST_SUITE_ADD(udpd_base_conn_init_invalid_params)
    TEST_SUITE_ADD(udpd_base_conn_create_socket)
    TEST_SUITE_ADD(udpd_base_conn_create_socket_invalid)
    TEST_SUITE_ADD(udpd_base_conn_bind_to_any)
    TEST_SUITE_ADD(udpd_base_conn_bind_invalid_params)
    TEST_SUITE_ADD(udpd_base_conn_setup_events_no_callbacks)
    TEST_SUITE_ADD(udpd_base_conn_setup_events_invalid)
    TEST_SUITE_ADD(udpd_base_conn_request_send_ready_invalid)
    TEST_SUITE_ADD(udpd_base_conn_cleanup_null)
    TEST_SUITE_ADD(udpd_base_conn_cleanup_initialized)
    TEST_SUITE_ADD(udpd_connection_cleanup_on_disconnect)
    TEST_SUITE_ADD(udpd_process_received_data_invalid_params)
    TEST_SUITE_ADD(udpd_handle_read_error)
    TEST_SUITE_ADD(udpd_common_readcb_invalid_params)
    TEST_SUITE_ADD(udpd_common_writecb_invalid_params)
    TEST_SUITE_ADD(udpd_base_conn_state_transitions)
    TEST_SUITE_ADD(udpd_base_conn_socket_operations)
    TEST_SUITE_ADD(udpd_base_conn_memory_management)
    TEST_SUITE_ADD(udpd_base_conn_edge_cases)
TEST_SUITE_END(udpd_event)

TEST_SUITE_ADD_NAME(udpd_base_conn_init)
TEST_SUITE_ADD_NAME(udpd_base_conn_init_invalid_params)
TEST_SUITE_ADD_NAME(udpd_base_conn_create_socket)
TEST_SUITE_ADD_NAME(udpd_base_conn_create_socket_invalid)
TEST_SUITE_ADD_NAME(udpd_base_conn_bind_to_any)
TEST_SUITE_ADD_NAME(udpd_base_conn_bind_invalid_params)
TEST_SUITE_ADD_NAME(udpd_base_conn_setup_events_no_callbacks)
TEST_SUITE_ADD_NAME(udpd_base_conn_setup_events_invalid)
TEST_SUITE_ADD_NAME(udpd_base_conn_request_send_ready_invalid)
TEST_SUITE_ADD_NAME(udpd_base_conn_cleanup_null)
TEST_SUITE_ADD_NAME(udpd_base_conn_cleanup_initialized)
TEST_SUITE_ADD_NAME(udpd_connection_cleanup_on_disconnect)
TEST_SUITE_ADD_NAME(udpd_process_received_data_invalid_params)
TEST_SUITE_ADD_NAME(udpd_handle_read_error)
TEST_SUITE_ADD_NAME(udpd_common_readcb_invalid_params)
TEST_SUITE_ADD_NAME(udpd_common_writecb_invalid_params)
TEST_SUITE_ADD_NAME(udpd_base_conn_state_transitions)
TEST_SUITE_ADD_NAME(udpd_base_conn_socket_operations)
TEST_SUITE_ADD_NAME(udpd_base_conn_memory_management)
TEST_SUITE_ADD_NAME(udpd_base_conn_edge_cases)
TEST_SUITE_FINISH(udpd_event)