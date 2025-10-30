#!/usr/bin/env lua

-- MariaDB Basic Tests - Connection, Simple Queries, CRUD & Simple Data Types
-- Consolidated foundational tests extracted from phase1/phase2a/phase2b sources.
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')

print("Starting MariaDB Basic tests...")

-- MariaDB connection configuration
local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

-- Global connection for reuse/cleanup
local test_conn = nil

-- Setup: establish connection (lazy) and ensure clean basic tables
local function setup_database()
    if test_conn then return test_conn end
    print("Connecting to MariaDB (basic tests)...")
    local mariadb = require('fan.mariadb')
    test_conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user, DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)
    if not test_conn then
        error("Failed to connect to MariaDB. Make sure Docker container is running:\ncd tests && ./docker-setup.sh start")
    end
    print("✔ MariaDB connection established")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        -- Drop any tables created by this suite
        pcall(function() test_conn:execute("DROP TABLE IF EXISTS basic_users") end)
        pcall(function() test_conn:execute("DROP TABLE IF EXISTS basic_types") end)
        print("Cleaning up database connection...")
        test_conn:close()
        test_conn = nil
        print("✔ Database connection closed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("MariaDB Basic - Connection, CRUD & Simple Types")

-- Test 1: Module load
suite:test("mariadb_module_load", function()
    local mariadb = require('fan.mariadb')
    TestFramework.assert_type(mariadb, "table")
    TestFramework.assert_type(mariadb.connect, "function")
    print("✔ MariaDB module loaded")
end)

-- Test 2: Basic connection
suite:test("mariadb_basic_connection", function()
    local conn = setup_database()
    TestFramework.assert_not_nil(conn)
    TestFramework.assert_type(conn.execute, "function")
    TestFramework.assert_type(conn.close, "function")
    print("✔ Basic connection successful")
end)

-- Test 3: Simple SELECT (no tables)
suite:test("simple_select_query", function()
    local conn = setup_database()
    local result = conn:execute("SELECT 1 as test_value, 'hello' as test_string")
    TestFramework.assert_not_nil(result)
    if type(result) == "userdata" then
        local row = result:fetch()
        TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.test_value, 1)
        TestFramework.assert_equal(row.test_string, "hello")
        result:close()
    end
    print("✔ Simple SELECT executed")
end)

-- Test 4: Create basic table
suite:test("create_basic_table", function()
    local conn = setup_database()
    conn:execute("DROP TABLE IF EXISTS basic_users")
    local create_sql = [[
        CREATE TABLE basic_users (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]
    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)
    print("✔ basic_users table created")
end)

-- Test 5: Verify table exists
suite:test("verify_basic_table_exists", function()
    local conn = setup_database()
    local result = conn:execute("SHOW TABLES LIKE 'basic_users'")
    TestFramework.assert_not_nil(result)
    print("✔ Table existence verified")
end)

-- Test 6: Basic INSERT + SELECT COUNT
suite:test("basic_insert_and_count", function()
    local conn = setup_database()
    local insert_sql = "INSERT INTO basic_users (name, email) VALUES ('Alice', 'alice@test.com'), ('Bob', 'bob@test.com')"
    local ins = conn:execute(insert_sql)
    TestFramework.assert_not_nil(ins)
    local count_res = conn:execute("SELECT COUNT(*) as count FROM basic_users")
    TestFramework.assert_not_nil(count_res)
    if type(count_res) == "userdata" then
        local row = count_res:fetch()
        TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.count, 2)
        count_res:close()
    end
    print("✔ Basic INSERT & COUNT successful")
end)

-- Test 7: Simple CRUD cycle (UPDATE + DELETE)
suite:test("simple_crud_cycle", function()
    local conn = setup_database()
    -- UPDATE one row
    local upd = conn:execute("UPDATE basic_users SET email='alice@new.com' WHERE name='Alice'")
    TestFramework.assert_not_nil(upd)
    local sel = conn:execute("SELECT email FROM basic_users WHERE name='Alice'")
    if type(sel) == "userdata" then
        local row = sel:fetch(); TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.email, 'alice@new.com')
        sel:close()
    end
    -- DELETE one row
    local del = conn:execute("DELETE FROM basic_users WHERE name='Bob'")
    TestFramework.assert_not_nil(del)
    local post = conn:execute("SELECT COUNT(*) as count FROM basic_users")
    if type(post) == "userdata" then
        local crow = post:fetch(); TestFramework.assert_not_nil(crow)
        TestFramework.assert_equal(crow.count, 1)
        post:close()
    end
    print("✔ Simple CRUD cycle passed")
end)

-- Test 8: Basic data types (numeric & string)
suite:test("basic_data_types", function()
    local conn = setup_database()
    conn:execute("DROP TABLE IF EXISTS basic_types")
    local create_sql = [[
        CREATE TABLE basic_types (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_int INT,
            test_varchar VARCHAR(50),
            test_decimal DECIMAL(10,2)
        )
    ]]
    local c = conn:execute(create_sql); TestFramework.assert_not_nil(c)
    local ins = conn:execute("INSERT INTO basic_types (test_int, test_varchar, test_decimal) VALUES (42, 'Hello World', 1234.56)")
    TestFramework.assert_not_nil(ins)
    local sel = conn:execute("SELECT * FROM basic_types WHERE id = 1")
    TestFramework.assert_not_nil(sel)
    if type(sel) == "userdata" then
        local row = sel:fetch(); TestFramework.assert_not_nil(row)
        TestFramework.assert_equal(row.test_int, 42)
        TestFramework.assert_equal(row.test_varchar, 'Hello World')
        TestFramework.assert_equal(row.test_decimal, 1234.56)
        sel:close()
    end
    print("✔ Basic data types test passed")
end)

-- Test 9: Connection ping
suite:test("connection_ping", function()
    local conn = setup_database()
    local ping_result = conn:ping()
    TestFramework.assert_true(ping_result)
    print("✔ Connection ping successful")
end)

-- Test 10: Error handling - invalid SQL
suite:test("error_handling_invalid_sql", function()
    local conn = setup_database()
    local result, error_msg = conn:execute("INVALID SQL STATEMENT")
    TestFramework.assert_nil(result)
    TestFramework.assert_type(error_msg, "string")
    print("✔ Invalid SQL error captured")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Cleanup always
cleanup_database()

if failures > 0 then
    print("MariaDB Basic tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Basic tests completed successfully!")
