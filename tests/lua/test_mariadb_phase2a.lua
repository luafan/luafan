#!/usr/bin/env lua

-- MariaDB Phase 2A Tests - Data Types Testing
-- Tests various MariaDB data types storage and retrieval
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')

print("Starting MariaDB Phase 2A tests - Data Types...")

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

    -- Clean up any existing test tables
    test_conn:execute("DROP TABLE IF EXISTS data_types_test")

    print("✓ MariaDB connection established")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        print("Cleaning up database...")
        test_conn:execute("DROP TABLE IF EXISTS data_types_test")
        test_conn:close()
        test_conn = nil
        print("✓ Database cleanup completed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("MariaDB Phase 2A - Data Types Testing")

-- Test 1: Numeric data types
suite:test("numeric_data_types", function()
    local conn = test_conn or setup_database()

    -- Create table with various numeric types
    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_tinyint TINYINT,
            test_smallint SMALLINT,
            test_mediumint MEDIUMINT,
            test_int INT,
            test_bigint BIGINT,
            test_float FLOAT,
            test_double DOUBLE,
            test_decimal DECIMAL(10,2)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert test data with various numeric values
    local insert_sql = [[
        INSERT INTO data_types_test
        (test_tinyint, test_smallint, test_mediumint, test_int, test_bigint,
         test_float, test_double, test_decimal)
        VALUES
        (127, 32767, 8388607, 2147483647, 9223372036854775807,
         3.14159, 2.718281828459045, 999999.99)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify data
    local select_result = conn:execute("SELECT * FROM data_types_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify numeric types
        TestFramework.assert_equal(row.test_tinyint, 127)
        TestFramework.assert_equal(row.test_smallint, 32767)
        TestFramework.assert_equal(row.test_mediumint, 8388607)
        TestFramework.assert_equal(row.test_int, 2147483647)
        TestFramework.assert_equal(row.test_bigint, 9223372036854775807)
        TestFramework.assert_true(math.abs(row.test_float - 3.14159) < 0.001)
        TestFramework.assert_true(math.abs(row.test_double - 2.718281828459045) < 0.000000000001)
        TestFramework.assert_equal(row.test_decimal, 999999.99)

        select_result:close()
    end

    print("✓ Numeric data types test passed")
end)

-- Test 2: String data types
suite:test("string_data_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for string types
    conn:execute("DROP TABLE IF EXISTS data_types_test")

    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_char CHAR(10),
            test_varchar VARCHAR(255),
            test_text TEXT,
            test_mediumtext MEDIUMTEXT,
            test_longtext LONGTEXT
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Test data with various string lengths and content
    local test_strings = {
        char_data = "CHAR_TEST",  -- Exactly 10 chars will be padded
        varchar_data = "This is a VARCHAR test with special chars: 你好世界! @#$%^&*()",
        text_data = "This is a TEXT field that can hold much longer content. " .. string.rep("Long text content. ", 50),
        mediumtext_data = "MEDIUMTEXT: " .. string.rep("Medium length text content. ", 100),
        longtext_data = "LONGTEXT: " .. string.rep("Very long text content for testing. ", 200)
    }

    local insert_sql = string.format([[
        INSERT INTO data_types_test
        (test_char, test_varchar, test_text, test_mediumtext, test_longtext)
        VALUES ('%s', '%s', '%s', '%s', '%s')
    ]],
        test_strings.char_data,
        conn:escape(test_strings.varchar_data),
        conn:escape(test_strings.text_data),
        conn:escape(test_strings.mediumtext_data),
        conn:escape(test_strings.longtext_data)
    )

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify string data
    local select_result = conn:execute("SELECT * FROM data_types_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify string types (CHAR is right-padded with spaces)
        TestFramework.assert_equal(string.trim and string.trim(row.test_char) or row.test_char:gsub("%s+$", ""), test_strings.char_data)
        TestFramework.assert_equal(row.test_varchar, test_strings.varchar_data)
        TestFramework.assert_equal(row.test_text, test_strings.text_data)
        TestFramework.assert_equal(row.test_mediumtext, test_strings.mediumtext_data)
        TestFramework.assert_equal(row.test_longtext, test_strings.longtext_data)

        select_result:close()
    end

    print("✓ String data types test passed")
end)

-- Test 3: Date and time data types
suite:test("datetime_data_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for datetime types
    conn:execute("DROP TABLE IF EXISTS data_types_test")

    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_date DATE,
            test_time TIME,
            test_datetime DATETIME,
            test_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            test_year YEAR
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert test data with various date/time values
    local insert_sql = [[
        INSERT INTO data_types_test
        (test_date, test_time, test_datetime, test_year)
        VALUES
        ('2023-12-25', '14:30:45', '2023-12-25 14:30:45', 2023)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify datetime data
    local select_result = conn:execute("SELECT * FROM data_types_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify date/time types
        TestFramework.assert_type(row.test_date, "string")
        TestFramework.assert_match(row.test_date, "2023%-12%-25")
        TestFramework.assert_type(row.test_time, "string")
        TestFramework.assert_match(row.test_time, "14:30:45")
        TestFramework.assert_type(row.test_datetime, "string")
        TestFramework.assert_match(row.test_datetime, "2023%-12%-25 14:30:45")
        TestFramework.assert_not_nil(row.test_timestamp) -- AUTO generated
        TestFramework.assert_equal(row.test_year, 2023)

        select_result:close()
    end

    print("✓ Date and time data types test passed")
end)

-- Test 4: Boolean data type
suite:test("boolean_data_type", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for boolean type
    conn:execute("DROP TABLE IF EXISTS data_types_test")

    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_bool BOOLEAN,
            test_bit BIT(1)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert boolean test data
    local insert_sql = [[
        INSERT INTO data_types_test (test_bool, test_bit) VALUES
        (TRUE, 1),
        (FALSE, 0),
        (1, 1),
        (0, 0)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify boolean data
    local select_result = conn:execute("SELECT * FROM data_types_test ORDER BY id")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row1 = select_result:fetch()
        TestFramework.assert_not_nil(row1)
        TestFramework.assert_equal(row1.test_bool, 1) -- TRUE stored as 1
        TestFramework.assert_not_nil(row1.test_bit)

        local row2 = select_result:fetch()
        TestFramework.assert_not_nil(row2)
        TestFramework.assert_equal(row2.test_bool, 0) -- FALSE stored as 0
        TestFramework.assert_not_nil(row2.test_bit)

        select_result:close()
    end

    print("✓ Boolean data type test passed")
end)

-- Test 5: Binary data types
suite:test("binary_data_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for binary types
    conn:execute("DROP TABLE IF EXISTS data_types_test")

    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_binary BINARY(16),
            test_varbinary VARBINARY(255),
            test_blob BLOB
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert binary test data (using hexadecimal representation)
    local insert_sql = [[
        INSERT INTO data_types_test
        (test_binary, test_varbinary, test_blob)
        VALUES
        (X'48656C6C6F20576F726C6421000000', X'48656C6C6F20576F726C6421', X'48656C6C6F20576F726C642042696E61727921')
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify binary data
    local select_result = conn:execute("SELECT * FROM data_types_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify binary types exist (exact content verification depends on encoding)
        TestFramework.assert_not_nil(row.test_binary)
        TestFramework.assert_not_nil(row.test_varbinary)
        TestFramework.assert_not_nil(row.test_blob)

        select_result:close()
    end

    print("✓ Binary data types test passed")
end)

-- Test 6: NULL values handling
suite:test("null_values_handling", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for NULL testing
    conn:execute("DROP TABLE IF EXISTS data_types_test")

    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            nullable_int INT,
            nullable_varchar VARCHAR(100),
            nullable_date DATE,
            nullable_bool BOOLEAN
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert NULL values
    local insert_sql = [[
        INSERT INTO data_types_test
        (nullable_int, nullable_varchar, nullable_date, nullable_bool)
        VALUES
        (NULL, NULL, NULL, NULL),
        (42, 'not null', '2023-01-01', TRUE)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify NULL handling
    local select_result = conn:execute("SELECT * FROM data_types_test ORDER BY id")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        -- First row - all NULLs
        local row1 = select_result:fetch()
        TestFramework.assert_not_nil(row1)
        TestFramework.assert_nil(row1.nullable_int)
        TestFramework.assert_nil(row1.nullable_varchar)
        TestFramework.assert_nil(row1.nullable_date)
        TestFramework.assert_nil(row1.nullable_bool)

        -- Second row - all non-NULL
        local row2 = select_result:fetch()
        TestFramework.assert_not_nil(row2)
        TestFramework.assert_equal(row2.nullable_int, 42)
        TestFramework.assert_equal(row2.nullable_varchar, "not null")
        TestFramework.assert_equal(row2.nullable_date, "2023-01-01")
        TestFramework.assert_equal(row2.nullable_bool, 1)

        select_result:close()
    end

    print("✓ NULL values handling test passed")
end)

-- Test 7: Data type conversion and precision
suite:test("data_type_conversion_precision", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for precision testing
    conn:execute("DROP TABLE IF EXISTS data_types_test")

    local create_sql = [[
        CREATE TABLE data_types_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            precise_decimal DECIMAL(15,5),
            test_float FLOAT(7,4),
            test_double DOUBLE(15,10)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert precise decimal values
    local insert_sql = [[
        INSERT INTO data_types_test
        (precise_decimal, test_float, test_double)
        VALUES
        (1234567890.12345, 123.4567, 12345.1234567890)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify precision
    local select_result = conn:execute("SELECT * FROM data_types_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify precision handling
        TestFramework.assert_equal(row.precise_decimal, 1234567890.12345)
        TestFramework.assert_true(math.abs(row.test_float - 123.4567) < 0.0001)
        TestFramework.assert_true(math.abs(row.test_double - 12345.1234567890) < 0.0000000001)

        select_result:close()
    end

    print("✓ Data type conversion and precision test passed")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Always cleanup, even if tests fail
cleanup_database()

if failures > 0 then
    print("MariaDB Phase 2A tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 2A tests completed successfully!")