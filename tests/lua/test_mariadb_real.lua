#!/usr/bin/env lua

-- Real MariaDB Integration Tests
-- Tests the complete MariaDB functionality with actual database

local TestFramework = require('test_framework')

print("Starting real MariaDB integration tests...")

-- MariaDB connection configuration
local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

-- Test data
local TEST_USERS = {
    {name = "Alice", email = "alice@test.com", age = 25, score = 95.5, is_active = true, bio = "Software engineer"},
    {name = "Bob", email = "bob@test.com", age = 30, score = 88.0, is_active = false, bio = "Data scientist"},
    {name = "Charlie", email = "charlie@test.com", age = 35, score = 92.3, is_active = true, bio = "Product manager"}
}

-- Global connection for cleanup
local test_conn = nil

-- Test suite setup/teardown
local function setup_database()
    print("Setting up MariaDB test database...")

    -- Try to connect to MariaDB
    local mariadb = require('fan.mariadb')
    test_conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user, DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)

    if not test_conn then
        error("Failed to connect to MariaDB. Make sure Docker container is running:\ndocker run -d --name test-mariadb -e MYSQL_ROOT_PASSWORD=root_password -e MYSQL_DATABASE=test_db -e MYSQL_USER=test_user -e MYSQL_PASSWORD=test_password -p 3306:3306 mariadb:latest")
    end

    -- Drop and recreate test table to ensure correct schema
    test_conn:execute("DROP TABLE IF EXISTS users")

    local create_table_sql = [[
        CREATE TABLE users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255) UNIQUE,
            age INT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            score DOUBLE,
            is_active BOOLEAN DEFAULT TRUE,
            bio TEXT
        )
    ]]

    local result = test_conn:execute(create_table_sql)
    if not result then
        error("Failed to create test table")
    end

    print("✓ MariaDB test database setup completed")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        print("Cleaning up MariaDB test database...")
        test_conn:execute("DROP TABLE IF EXISTS users")
        test_conn:close()
        test_conn = nil
        print("✓ MariaDB test database cleanup completed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("Real MariaDB Integration Tests")

-- Test MariaDB module availability
suite:test("mariadb_module_availability", function()
    local mariadb = require('fan.mariadb')
    TestFramework.assert_type(mariadb, "table")
    TestFramework.assert_type(mariadb.connect, "function")
    print("✓ MariaDB module loaded successfully")
end)

-- Test basic connection
suite:test("mariadb_connection", function()
    local conn = setup_database()
    TestFramework.assert_not_nil(conn)
    TestFramework.assert_type(conn.execute, "function")
    TestFramework.assert_type(conn.close, "function")
    print("✓ MariaDB connection established successfully")
end)

-- Test basic query execution
suite:test("basic_query_execution", function()
    local conn = test_conn or setup_database()

    -- Test simple SELECT
    local result = conn:execute("SELECT 1 as test_value, 'hello' as test_string")
    TestFramework.assert_not_nil(result)

    if type(result) == "userdata" then
        -- Result is a cursor, fetch the row
        local row = result:fetch()
        TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.test_value, 1)
        TestFramework.assert_equal(row.test_string, "hello")
        result:close()
    end

    print("✓ Basic query execution successful")
end)

-- Test INSERT operations
suite:test("insert_operations", function()
    local conn = test_conn or setup_database()

    -- Insert test data
    for _, user in ipairs(TEST_USERS) do
        local sql = string.format(
            "INSERT INTO users (name, email, age, score, is_active, bio) VALUES ('%s', '%s', %d, %.2f, %s, '%s')",
            user.name, user.email, user.age, user.score, user.is_active and "TRUE" or "FALSE", user.bio
        )
        local result = conn:execute(sql)
        TestFramework.assert_not_nil(result, "INSERT should succeed for " .. user.name)
    end

    -- Verify insert count
    local count_result = conn:execute("SELECT COUNT(*) as count FROM users")
    if type(count_result) == "userdata" then
        local row = count_result:fetch()
        TestFramework.assert_equal(row.count, #TEST_USERS)
        count_result:close()
    end

    print("✓ INSERT operations successful")
end)

-- Test SELECT operations with various data types
suite:test("select_operations", function()
    local conn = test_conn or setup_database()

    -- Select all users
    local result = conn:execute("SELECT id, name, email, age, score, is_active, bio, created_at FROM users ORDER BY name")
    TestFramework.assert_not_nil(result)

    if type(result) == "userdata" then
        local users_found = {}
        local row = result:fetch()

        while row do
            table.insert(users_found, row)
            TestFramework.assert_type(row.id, "number")
            TestFramework.assert_type(row.name, "string")
            TestFramework.assert_type(row.email, "string")
            TestFramework.assert_type(row.age, "number")
            TestFramework.assert_type(row.score, "number")
            TestFramework.assert_type(row.bio, "string")
            TestFramework.assert_not_nil(row.created_at)

            row = result:fetch()
        end

        TestFramework.assert_equal(#users_found, #TEST_USERS)
        result:close()
    end

    print("✓ SELECT operations with various data types successful")
end)

-- Test prepared statements
suite:test("prepared_statements", function()
    local conn = test_conn or setup_database()

    -- Prepare a statement
    local stmt = conn:prepare("SELECT name, age FROM users WHERE age > ? AND is_active = ?")
    TestFramework.assert_not_nil(stmt)
    TestFramework.assert_type(stmt.bind, "function")
    TestFramework.assert_type(stmt.execute, "function")

    -- Bind parameters and execute
    stmt:bind(25, true)  -- age > 25 AND is_active = true
    local result = stmt:execute()
    TestFramework.assert_not_nil(result)

    stmt:close()
    print("✓ Prepared statements functionality successful")
end)

-- Test transactions
suite:test("transaction_operations", function()
    local conn = test_conn or setup_database()

    -- Test autocommit off
    local result = conn:autocommit(false)
    TestFramework.assert_true(result)

    -- Insert a test record
    local insert_result = conn:execute("INSERT INTO users (name, email, age) VALUES ('Transaction Test', 'trans@test.com', 40)")
    TestFramework.assert_not_nil(insert_result)

    -- Rollback
    local rollback_result = conn:rollback()
    TestFramework.assert_true(rollback_result)

    -- Verify rollback worked
    local count_result = conn:execute("SELECT COUNT(*) as count FROM users WHERE name = 'Transaction Test'")
    if type(count_result) == "userdata" then
        local row = count_result:fetch()
        TestFramework.assert_equal(row.count, 0)
        count_result:close()
    end

    -- Re-enable autocommit
    conn:autocommit(true)

    print("✓ Transaction operations successful")
end)

-- Test connection management
suite:test("connection_management", function()
    local conn = test_conn or setup_database()

    -- Test ping
    local ping_result = conn:ping()
    TestFramework.assert_true(ping_result)

    -- Test escape string
    local unsafe_string = "It's a test with 'quotes' and \"double quotes\""
    local escaped = conn:escape(unsafe_string)
    TestFramework.assert_type(escaped, "string")
    TestFramework.assert_not_equal(escaped, unsafe_string)

    print("✓ Connection management operations successful")
end)

-- Test error handling
suite:test("error_handling", function()
    local conn = test_conn or setup_database()

    -- Test invalid SQL
    local result, error_msg = conn:execute("INVALID SQL STATEMENT")
    TestFramework.assert_nil(result)
    TestFramework.assert_type(error_msg, "string")

    -- Test duplicate key error
    local dup_result, dup_error = conn:execute("INSERT INTO users (name, email) VALUES ('Test', 'alice@test.com')")  -- alice@test.com already exists
    TestFramework.assert_nil(dup_result)
    TestFramework.assert_type(dup_error, "string")

    print("✓ Error handling successful")
end)

-- Performance test
suite:test("performance_test", function()
    local conn = test_conn or setup_database()

    local start_time = os.clock()

    -- Insert 100 records in a transaction
    conn:autocommit(false)
    for i = 1, 100 do
        local sql = string.format("INSERT INTO users (name, email, age) VALUES ('User%d', 'user%d@perf.test', %d)",
                                i, i, 20 + (i % 40))
        conn:execute(sql)
    end
    conn:commit()
    conn:autocommit(true)

    local insert_time = os.clock() - start_time

    -- Select all records
    start_time = os.clock()
    local result = conn:execute("SELECT COUNT(*) as total FROM users")
    if type(result) == "userdata" then
        local row = result:fetch()
        TestFramework.assert_true(row.total >= 100)
        result:close()
    end
    local select_time = os.clock() - start_time

    print(string.format("✓ Performance test: Insert=%.3fms, Select=%.3fms",
                       insert_time * 1000, select_time * 1000))
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Always cleanup, even if tests fail
cleanup_database()

if failures > 0 then
    print("MariaDB integration tests failed!")
    os.exit(1)
end

print("✅ All MariaDB integration tests completed successfully!")