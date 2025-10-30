#!/usr/bin/env lua

-- MariaDB Test Configuration Module
-- Shared configuration and utilities for MariaDB comprehensive tests
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local M = {}

-- MariaDB connection configuration (same as original tests)
M.DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

-- Global connection variable for test reuse
M.test_conn = nil
M.mariadb = nil

-- Test suite setup function
function M.setup_database()
    print("Setting up MariaDB connection for comprehensive tests...")

    M.mariadb = require('fan.mariadb')
    M.test_conn = M.mariadb.connect(M.DB_CONFIG.database, M.DB_CONFIG.user, M.DB_CONFIG.password, M.DB_CONFIG.host, M.DB_CONFIG.port)

    if not M.test_conn then
        error("Failed to connect to MariaDB. Make sure Docker container is running:\ncd tests && ./docker-setup.sh start")
    end

    print("✓ MariaDB connection established for comprehensive tests")
    return M.test_conn
end

-- Test suite cleanup function
function M.cleanup_database()
    if M.test_conn then
        print("Cleaning up comprehensive test database connection...")
        -- Clean up any test tables that might have been created
        local tables_to_drop = {
            "test_comprehensive",
            "test_prepared",
            "test_transactions",
            "test_pool",
            "test_charset",
            "test_cursor",
            "test_cursor_data",
            "test_memory",
            "test_performance",
            "test_security",
            "test_longdata",
            "test_exceptions",
            "test_concurrent",
            "test_race_condition",
            "test_contention"
        }

        for _, table_name in ipairs(tables_to_drop) do
            pcall(function() M.test_conn:execute("DROP TABLE IF EXISTS " .. table_name) end)
        end

        M.test_conn:close()
        M.test_conn = nil
        print("✓ Comprehensive test database connection closed")
    end
end

-- Create test suite with standard setup/teardown
function M.create_test_suite(suite_name, TestFramework)
    local suite = TestFramework.create_suite(suite_name)

    -- Set up suite-level setup and teardown
    suite:set_setup(M.setup_database)
    suite:set_teardown(M.cleanup_database)

    -- Add before_each and after_each for memory management
    suite:set_before_each(function()
        collectgarbage("collect")
    end)

    suite:set_after_each(function()
        collectgarbage("collect")
    end)

    return suite
end

-- Get current connection or setup if needed
function M.get_connection()
    return M.test_conn or M.setup_database()
end

-- Common test utilities
M.utils = {}

-- Generate test data for performance tests
function M.utils.generate_test_data(count, prefix)
    local data = {}
    for i = 1, count do
        table.insert(data, {
            name = (prefix or "test") .. "_" .. i,
            value = i * 10,
            category = "category_" .. ((i % 5) + 1)
        })
    end
    return data
end

-- Create common test table structures
function M.utils.create_basic_test_table(conn, table_name)
    conn:execute("DROP TABLE IF EXISTS " .. table_name)
    local create_sql = string.format([[
        CREATE TABLE %s (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100),
            value INT,
            category VARCHAR(50),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]], table_name)
    return conn:execute(create_sql)
end

return M