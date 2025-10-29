#!/usr/bin/env lua

-- MariaDB Phase 1 Tests - Simple Connection and Basic Table Operations
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')

print("Starting MariaDB Phase 1 tests...")

-- MariaDB connection configuration
local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

-- Global connection for cleanup
local test_conn = nil

-- Test suite setup/teardown
local function setup_database()
    print("Connecting to MariaDB...")

    local mariadb = require('fan.mariadb')
    test_conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user, DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)

    if not test_conn then
        error("Failed to connect to MariaDB. Make sure Docker container is running:\ncd tests && ./docker-setup.sh start")
    end

    print("✓ MariaDB connection established")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        print("Cleaning up database connection...")
        test_conn:close()
        test_conn = nil
        print("✓ Database connection closed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("MariaDB Phase 1 - Connection & Basic Table Operations")

-- Test 1: MariaDB module loading
suite:test("mariadb_module_load", function()
    local mariadb = require('fan.mariadb')
    TestFramework.assert_type(mariadb, "table")
    TestFramework.assert_type(mariadb.connect, "function")
    print("✓ MariaDB module loaded successfully")
end)

-- Test 2: Basic connection
suite:test("mariadb_basic_connection", function()
    local conn = setup_database()
    TestFramework.assert_not_nil(conn)
    TestFramework.assert_type(conn.execute, "function")
    TestFramework.assert_type(conn.close, "function")
    print("✓ Database connection successful")
end)

-- Test 3: Simple SELECT query
suite:test("simple_select_query", function()
    local conn = test_conn or setup_database()

    local result = conn:execute("SELECT 1 as test_value, 'hello' as test_string")
    TestFramework.assert_not_nil(result)
    print("✓ Simple SELECT query executed")
end)

-- Test 4: Create table
suite:test("create_table", function()
    local conn = test_conn or setup_database()

    -- Drop table if exists first
    conn:execute("DROP TABLE IF EXISTS test_users")

    -- Create new table
    local create_sql = [[
        CREATE TABLE test_users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)
    print("✓ Table created successfully")
end)

-- Test 5: Check table exists
suite:test("check_table_exists", function()
    local conn = test_conn or setup_database()

    local result = conn:execute("SHOW TABLES LIKE 'test_users'")
    TestFramework.assert_not_nil(result)
    print("✓ Table existence verified")
end)

-- Test 6: Basic INSERT
suite:test("basic_insert", function()
    local conn = test_conn or setup_database()

    local insert_sql = "INSERT INTO test_users (name, email) VALUES ('Alice', 'alice@test.com')"
    local result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)
    print("✓ Basic INSERT successful")
end)

-- Test 7: Basic SELECT from table
suite:test("basic_select_from_table", function()
    local conn = test_conn or setup_database()

    local result = conn:execute("SELECT COUNT(*) as count FROM test_users")
    TestFramework.assert_not_nil(result)
    print("✓ SELECT from table successful")
end)

-- Test 8: Drop table
suite:test("drop_table", function()
    local conn = test_conn or setup_database()

    local result = conn:execute("DROP TABLE IF EXISTS test_users")
    TestFramework.assert_not_nil(result)
    print("✓ Table dropped successfully")
end)

-- Test 9: Connection ping
suite:test("connection_ping", function()
    local conn = test_conn or setup_database()

    local ping_result = conn:ping()
    TestFramework.assert_true(ping_result)
    print("✓ Connection ping successful")
end)

-- Test 10: Error handling - invalid SQL
suite:test("error_handling_invalid_sql", function()
    local conn = test_conn or setup_database()

    local result, error_msg = conn:execute("INVALID SQL STATEMENT")
    TestFramework.assert_nil(result)
    TestFramework.assert_type(error_msg, "string")
    print("✓ Error handling works correctly")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Always cleanup, even if tests fail
cleanup_database()

if failures > 0 then
    print("MariaDB Phase 1 tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 1 tests completed successfully!")