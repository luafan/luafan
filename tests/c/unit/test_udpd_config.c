#include "../framework/test_framework.h"
#include "udpd_common.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

// ============================================================================
// Test Cases for udpd_config.c
// ============================================================================

TEST_CASE(udpd_config_init) {
    udpd_config_t config;
    int result = udpd_config_init(&config);

    TEST_ASSERT_EQUAL(0, result);

    // Verify UDP-specific defaults
    TEST_ASSERT_EQUAL(0, config.broadcast_enabled);
    TEST_ASSERT_EQUAL(0, config.multicast_enabled);
    TEST_ASSERT_NULL(config.multicast_group);
    TEST_ASSERT_EQUAL(1, config.multicast_ttl);
    TEST_ASSERT_EQUAL(1, config.reuse_addr);
    TEST_ASSERT_EQUAL(0, config.reuse_port);

    // Buffer sizes are now in base config (UDP sets defaults if TCP doesn't)
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.receive_buffer_size);
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.send_buffer_size);

    // Verify base TCP config is initialized (spot check other fields)
    TEST_ASSERT_EQUAL(0, config.base.keepalive_enabled);
    TEST_ASSERT_EQUAL(7200, config.base.keepalive_idle);

    // Test NULL pointer
    result = udpd_config_init(NULL);
    TEST_ASSERT_EQUAL(-1, result);

    udpd_config_cleanup(&config);
}

TEST_CASE(udpd_config_set_defaults) {
    udpd_config_t config;
    memset(&config, 0xFF, sizeof(config)); // Fill with garbage

    int result = udpd_config_set_defaults(&config);
    TEST_ASSERT_EQUAL(0, result);

    // Verify UDP defaults are properly set
    TEST_ASSERT_EQUAL(0, config.broadcast_enabled);
    TEST_ASSERT_EQUAL(0, config.multicast_enabled);
    TEST_ASSERT_NULL(config.multicast_group);
    TEST_ASSERT_EQUAL(1, config.multicast_ttl);
    TEST_ASSERT_EQUAL(1, config.reuse_addr);
    TEST_ASSERT_EQUAL(0, config.reuse_port);

    // Buffer sizes are in base config
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.receive_buffer_size);
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.send_buffer_size);

    // Verify base TCP defaults (from tcpd_config_set_defaults)
    TEST_ASSERT_EQUAL(7200, config.base.keepalive_idle);
    TEST_ASSERT_EQUAL(75, config.base.keepalive_interval);
    TEST_ASSERT_EQUAL(9, config.base.keepalive_count);

    // Test NULL pointer
    result = udpd_config_set_defaults(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

TEST_CASE(udpd_config_from_lua_table_empty) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    udpd_config_t config;

    // Test with empty table
    lua_newtable(L);
    int table_index = lua_gettop(L);

    int result = udpd_config_from_lua_table(L, table_index, &config);
    TEST_ASSERT_EQUAL(0, result);

    // Should have default values
    TEST_ASSERT_EQUAL(0, config.broadcast_enabled);
    TEST_ASSERT_EQUAL(0, config.multicast_enabled);
    TEST_ASSERT_NULL(config.multicast_group);
    TEST_ASSERT_EQUAL(1, config.multicast_ttl);
    TEST_ASSERT_EQUAL(1, config.reuse_addr);
    TEST_ASSERT_EQUAL(0, config.reuse_port);

    // Buffer sizes are in base config
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.receive_buffer_size);
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.send_buffer_size);

    udpd_config_cleanup(&config);
    lua_close(L);
}

TEST_CASE(udpd_config_from_lua_table_full) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    udpd_config_t config;

    // Create table with UDP configuration
    lua_newtable(L);
    int table_index = lua_gettop(L);

    // Set UDP-specific values
    lua_pushboolean(L, 1);
    lua_setfield(L, table_index, "broadcast");

    lua_pushboolean(L, 1);
    lua_setfield(L, table_index, "multicast");

    lua_pushstring(L, "224.1.1.1");
    lua_setfield(L, table_index, "multicast_group");

    lua_pushinteger(L, 32);
    lua_setfield(L, table_index, "multicast_ttl");

    lua_pushboolean(L, 0);
    lua_setfield(L, table_index, "reuse_addr");

    lua_pushboolean(L, 1);
    lua_setfield(L, table_index, "reuse_port");

    lua_pushinteger(L, 4096);
    lua_setfield(L, table_index, "receive_buffer_size");

    lua_pushinteger(L, 8192);
    lua_setfield(L, table_index, "send_buffer_size");

    // Buffer sizes go to base TCP config, UDP uses them from there

    int result = udpd_config_from_lua_table(L, table_index, &config);
    TEST_ASSERT_EQUAL(0, result);

    // Verify UDP values
    TEST_ASSERT_EQUAL(1, config.broadcast_enabled);
    TEST_ASSERT_EQUAL(1, config.multicast_enabled);
    TEST_ASSERT_NOT_NULL(config.multicast_group);
    TEST_ASSERT_STRING_EQUAL("224.1.1.1", config.multicast_group);
    TEST_ASSERT_EQUAL(32, config.multicast_ttl);
    TEST_ASSERT_EQUAL(0, config.reuse_addr);
    TEST_ASSERT_EQUAL(1, config.reuse_port);
    // Buffer sizes are in base config
    TEST_ASSERT_EQUAL(4096, config.base.receive_buffer_size);
    TEST_ASSERT_EQUAL(8192, config.base.send_buffer_size);

    udpd_config_cleanup(&config);
    lua_close(L);
}

TEST_CASE(udpd_config_from_lua_table_invalid_params) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    udpd_config_t config;

    lua_newtable(L);
    int table_index = lua_gettop(L);

    // Test NULL parameters
    int result = udpd_config_from_lua_table(NULL, table_index, &config);
    TEST_ASSERT_EQUAL(-1, result);

    result = udpd_config_from_lua_table(L, table_index, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    lua_close(L);
}

TEST_CASE(udpd_config_validate_valid) {
    udpd_config_t config;
    udpd_config_init(&config);

    // Test default config (should be valid)
    int result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(0, result);

    // Test multicast config
    config.multicast_enabled = 1;
    config.multicast_group = strdup("224.1.1.1");
    config.multicast_ttl = 16;

    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(0, result);

    udpd_config_cleanup(&config);
}

TEST_CASE(udpd_config_validate_invalid) {
    udpd_config_t config;
    udpd_config_init(&config);

    // Test NULL pointer
    int result = udpd_config_validate(NULL);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid buffer sizes
    config.base.receive_buffer_size = 0;
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(-1, result);

    config.base.receive_buffer_size = UDPD_DEFAULT_BUFFER_SIZE;
    config.base.send_buffer_size = UDPD_MAX_PACKET_SIZE + 1;
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid multicast config
    config.base.send_buffer_size = UDPD_DEFAULT_BUFFER_SIZE;
    config.multicast_enabled = 1;
    config.multicast_group = NULL;
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid TTL
    config.multicast_group = strdup("224.1.1.1");
    config.multicast_ttl = 256;
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid multicast address
    free(config.multicast_group);
    config.multicast_group = strdup("192.168.1.1");  // Not multicast
    config.multicast_ttl = 16;
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid address format
    free(config.multicast_group);
    config.multicast_group = strdup("invalid.address");
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(-1, result);

    udpd_config_cleanup(&config);
}

TEST_CASE(udpd_config_copy) {
    udpd_config_t src, dest;
    udpd_config_init(&src);

    // Set up source config
    src.broadcast_enabled = 1;
    src.multicast_enabled = 1;
    src.multicast_group = strdup("224.2.2.2");
    src.multicast_ttl = 64;
    src.reuse_addr = 0;
    src.reuse_port = 1;
    src.base.receive_buffer_size = 8192;
    src.base.send_buffer_size = 16384;
    src.base.keepalive_enabled = 1;

    int result = udpd_config_copy(&dest, &src);
    TEST_ASSERT_EQUAL(0, result);

    // Verify copy
    TEST_ASSERT_EQUAL(src.broadcast_enabled, dest.broadcast_enabled);
    TEST_ASSERT_EQUAL(src.multicast_enabled, dest.multicast_enabled);
    TEST_ASSERT_NOT_NULL(dest.multicast_group);
    TEST_ASSERT_STRING_EQUAL(src.multicast_group, dest.multicast_group);
    TEST_ASSERT_NOT_EQUAL(src.multicast_group, dest.multicast_group); // Different pointers
    TEST_ASSERT_EQUAL(src.multicast_ttl, dest.multicast_ttl);
    TEST_ASSERT_EQUAL(src.reuse_addr, dest.reuse_addr);
    TEST_ASSERT_EQUAL(src.reuse_port, dest.reuse_port);
    TEST_ASSERT_EQUAL(src.base.receive_buffer_size, dest.base.receive_buffer_size);
    TEST_ASSERT_EQUAL(src.base.send_buffer_size, dest.base.send_buffer_size);
    TEST_ASSERT_EQUAL(src.base.keepalive_enabled, dest.base.keepalive_enabled);

    // Test NULL parameters
    result = udpd_config_copy(NULL, &src);
    TEST_ASSERT_EQUAL(-1, result);

    result = udpd_config_copy(&dest, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    udpd_config_cleanup(&src);
    udpd_config_cleanup(&dest);
}

TEST_CASE(udpd_config_cleanup) {
    udpd_config_t config;
    udpd_config_init(&config);

    // Set up config with allocated memory
    config.multicast_group = strdup("224.3.3.3");

    // Test cleanup with NULL (should not crash)
    udpd_config_cleanup(NULL);

    // Test normal cleanup
    udpd_config_cleanup(&config);

    // Verify cleanup
    TEST_ASSERT_NULL(config.multicast_group);
    // All other fields should be zeroed
    TEST_ASSERT_EQUAL(0, config.broadcast_enabled);
    TEST_ASSERT_EQUAL(0, config.multicast_enabled);
    TEST_ASSERT_EQUAL(0, config.multicast_ttl);
}

TEST_CASE(udpd_config_apply_socket_options_basic) {
    udpd_config_t config;
    udpd_config_init(&config);
    evutil_socket_t test_fd = -1;

    // Test NULL pointer
    int result = udpd_config_apply_socket_options(NULL, test_fd);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid socket
    result = udpd_config_apply_socket_options(&config, -1);
    TEST_ASSERT_EQUAL(-1, result);

    udpd_config_cleanup(&config);
}

TEST_CASE(udpd_config_apply_bind_options_basic) {
    udpd_config_t config;
    udpd_config_init(&config);
    evutil_socket_t test_fd = -1;

    // Test NULL pointer
    int result = udpd_config_apply_bind_options(NULL, test_fd);
    TEST_ASSERT_EQUAL(-1, result);

    // Test invalid socket
    result = udpd_config_apply_bind_options(&config, -1);
    TEST_ASSERT_EQUAL(-1, result);

    udpd_config_cleanup(&config);
}

TEST_CASE(udpd_config_multicast_address_validation) {
    udpd_config_t config;
    udpd_config_init(&config);

    // Test various multicast addresses
    struct {
        const char *addr;
        int should_be_valid;
    } test_cases[] = {
        {"224.0.0.1", 1},        // Valid multicast
        {"224.255.255.255", 1},  // Valid multicast
        {"239.0.0.1", 1},        // Valid multicast
        {"239.255.255.255", 1},  // Valid multicast
        {"223.255.255.255", 0},  // Not multicast
        {"240.0.0.1", 0},        // Not multicast
        {"192.168.1.1", 0},      // Unicast
        {"127.0.0.1", 0},        // Loopback
        {"255.255.255.255", 0},  // Broadcast
        {"0.0.0.0", 0},          // Invalid
        {"invalid", 0},          // Invalid format
        {"", 0},                 // Empty
        {NULL, 0}
    };

    config.multicast_enabled = 1;

    for (int i = 0; test_cases[i].addr != NULL || i == 0; i++) {
        if (test_cases[i].addr == NULL && i > 0) break;

        if (config.multicast_group) {
            free(config.multicast_group);
        }
        config.multicast_group = test_cases[i].addr ? strdup(test_cases[i].addr) : NULL;

        int result = udpd_config_validate(&config);
        if (test_cases[i].should_be_valid) {
            TEST_ASSERT_EQUAL(0, result);
        } else {
            TEST_ASSERT_EQUAL(-1, result);
        }
    }

    udpd_config_cleanup(&config);
}

TEST_CASE(udpd_config_edge_cases) {
    udpd_config_t config;

    // Test initialization with zero memory
    memset(&config, 0, sizeof(config));
    int result = udpd_config_set_defaults(&config);
    TEST_ASSERT_EQUAL(0, result);

    // Verify defaults are set correctly even from zero state
    TEST_ASSERT_EQUAL(1, config.reuse_addr);
    TEST_ASSERT_EQUAL(UDPD_DEFAULT_BUFFER_SIZE, config.base.receive_buffer_size);

    // Test boundary values
    config.multicast_ttl = 0;  // Minimum valid TTL
    config.multicast_enabled = 1;
    config.multicast_group = strdup("224.0.0.1");

    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(0, result);

    config.multicast_ttl = 255;  // Maximum valid TTL
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(0, result);

    // Test buffer size boundaries
    config.base.receive_buffer_size = 1;  // Minimum
    config.base.send_buffer_size = UDPD_MAX_PACKET_SIZE;  // Maximum
    result = udpd_config_validate(&config);
    TEST_ASSERT_EQUAL(0, result);

    udpd_config_cleanup(&config);
}

// ============================================================================
// Test Suite Setup/Teardown
// ============================================================================

void udpd_config_setup(void) {
    // Setup for UDP config tests
}

void udpd_config_teardown(void) {
    // Teardown for UDP config tests
}

// ============================================================================
// Test Suite Definition
// ============================================================================

TEST_SUITE_BEGIN(udpd_config)
    TEST_SUITE_ADD(udpd_config_init)
    TEST_SUITE_ADD(udpd_config_set_defaults)
    TEST_SUITE_ADD(udpd_config_from_lua_table_empty)
    TEST_SUITE_ADD(udpd_config_from_lua_table_full)
    TEST_SUITE_ADD(udpd_config_from_lua_table_invalid_params)
    TEST_SUITE_ADD(udpd_config_validate_valid)
    TEST_SUITE_ADD(udpd_config_validate_invalid)
    TEST_SUITE_ADD(udpd_config_copy)
    TEST_SUITE_ADD(udpd_config_cleanup)
    TEST_SUITE_ADD(udpd_config_apply_socket_options_basic)
    TEST_SUITE_ADD(udpd_config_apply_bind_options_basic)
    TEST_SUITE_ADD(udpd_config_multicast_address_validation)
    TEST_SUITE_ADD(udpd_config_edge_cases)
TEST_SUITE_END(udpd_config)

TEST_SUITE_ADD_NAME(udpd_config_init)
TEST_SUITE_ADD_NAME(udpd_config_set_defaults)
TEST_SUITE_ADD_NAME(udpd_config_from_lua_table_empty)
TEST_SUITE_ADD_NAME(udpd_config_from_lua_table_full)
TEST_SUITE_ADD_NAME(udpd_config_from_lua_table_invalid_params)
TEST_SUITE_ADD_NAME(udpd_config_validate_valid)
TEST_SUITE_ADD_NAME(udpd_config_validate_invalid)
TEST_SUITE_ADD_NAME(udpd_config_copy)
TEST_SUITE_ADD_NAME(udpd_config_cleanup)
TEST_SUITE_ADD_NAME(udpd_config_apply_socket_options_basic)
TEST_SUITE_ADD_NAME(udpd_config_apply_bind_options_basic)
TEST_SUITE_ADD_NAME(udpd_config_multicast_address_validation)
TEST_SUITE_ADD_NAME(udpd_config_edge_cases)
TEST_SUITE_FINISH(udpd_config)