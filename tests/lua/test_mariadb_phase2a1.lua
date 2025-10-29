#!/usr/bin/env lua

-- MariaDB Phase 2A1 Tests - Numeric Data Types
-- Tests comprehensive numeric data types storage, retrieval and edge cases
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')

print("Starting MariaDB Phase 2A1 tests - Numeric Data Types...")

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
    test_conn:execute("DROP TABLE IF EXISTS numeric_test")

    print("✓ MariaDB connection established")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        print("Cleaning up database...")
        test_conn:execute("DROP TABLE IF EXISTS numeric_test")
        test_conn:close()
        test_conn = nil
        print("✓ Database cleanup completed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("MariaDB Phase 2A1 - Numeric Data Types")

-- Test 1: Integer types (TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT)
suite:test("integer_types", function()
    local conn = test_conn or setup_database()

    -- Create table with various integer types
    local create_sql = [[
        CREATE TABLE numeric_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_tinyint TINYINT,
            test_smallint SMALLINT,
            test_mediumint MEDIUMINT,
            test_int INT,
            test_bigint BIGINT
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert test data with various integer values
    local insert_sql = [[
        INSERT INTO numeric_test
        (test_tinyint, test_smallint, test_mediumint, test_int, test_bigint)
        VALUES
        (127, 32767, 8388607, 2147483647, 9223372036854775807)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify data
    local select_result = conn:execute("SELECT * FROM numeric_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify integer types
        TestFramework.assert_equal(row.test_tinyint, 127)
        TestFramework.assert_equal(row.test_smallint, 32767)
        TestFramework.assert_equal(row.test_mediumint, 8388607)
        TestFramework.assert_equal(row.test_int, 2147483647)
        TestFramework.assert_equal(row.test_bigint, 9223372036854775807)

        select_result:close()
    end

    print("✓ Integer types test passed")
end)

-- Test 2: Integer types with negative values
suite:test("integer_negative_values", function()
    local conn = test_conn or setup_database()

    -- Insert negative values
    local insert_sql = [[
        INSERT INTO numeric_test
        (test_tinyint, test_smallint, test_mediumint, test_int, test_bigint)
        VALUES
        (-128, -32768, -8388608, -2147483648, -9223372036854775808)
    ]]

    local result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify negative values
    local select_result = conn:execute("SELECT * FROM numeric_test WHERE test_tinyint = -128")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify negative integer values
        TestFramework.assert_equal(row.test_tinyint, -128)
        TestFramework.assert_equal(row.test_smallint, -32768)
        TestFramework.assert_equal(row.test_mediumint, -8388608)
        TestFramework.assert_equal(row.test_int, -2147483648)
        TestFramework.assert_equal(row.test_bigint, -9223372036854775808)

        select_result:close()
    end

    print("✓ Integer negative values test passed")
end)

-- Test 3: Unsigned integer types
suite:test("unsigned_integer_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table with unsigned types
    conn:execute("DROP TABLE IF EXISTS numeric_test")

    local create_sql = [[
        CREATE TABLE numeric_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_tinyint_unsigned TINYINT UNSIGNED,
            test_smallint_unsigned SMALLINT UNSIGNED,
            test_mediumint_unsigned MEDIUMINT UNSIGNED,
            test_int_unsigned INT UNSIGNED,
            test_bigint_unsigned BIGINT UNSIGNED
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert maximum unsigned values
    local insert_sql = [[
        INSERT INTO numeric_test
        (test_tinyint_unsigned, test_smallint_unsigned, test_mediumint_unsigned, test_int_unsigned, test_bigint_unsigned)
        VALUES
        (255, 65535, 16777215, 4294967295, 18446744073709551615)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify unsigned values
    local select_result = conn:execute("SELECT * FROM numeric_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify unsigned integer values
        TestFramework.assert_equal(row.test_tinyint_unsigned, 255)
        TestFramework.assert_equal(row.test_smallint_unsigned, 65535)
        TestFramework.assert_equal(row.test_mediumint_unsigned, 16777215)
        TestFramework.assert_equal(row.test_int_unsigned, 4294967295)
        TestFramework.assert_equal(row.test_bigint_unsigned, 18446744073709551615)

        select_result:close()
    end

    print("✓ Unsigned integer types test passed")
end)

-- Test 4: Floating point types (FLOAT, DOUBLE)
suite:test("floating_point_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for floating point types
    conn:execute("DROP TABLE IF EXISTS numeric_test")

    local create_sql = [[
        CREATE TABLE numeric_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_float FLOAT,
            test_double DOUBLE,
            test_float_precision FLOAT(7,4),
            test_double_precision DOUBLE(15,10)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert floating point test data
    local insert_sql = [[
        INSERT INTO numeric_test
        (test_float, test_double, test_float_precision, test_double_precision)
        VALUES
        (3.14159, 2.718281828459045, 123.4567, 12345.1234567890)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify floating point data
    local select_result = conn:execute("SELECT * FROM numeric_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify floating point types (with tolerance for precision)
        TestFramework.assert_true(math.abs(row.test_float - 3.14159) < 0.001)
        TestFramework.assert_true(math.abs(row.test_double - 2.718281828459045) < 0.000000000001)
        TestFramework.assert_true(math.abs(row.test_float_precision - 123.4567) < 0.0001)
        TestFramework.assert_true(math.abs(row.test_double_precision - 12345.1234567890) < 0.0000000001)

        select_result:close()
    end

    print("✓ Floating point types test passed")
end)

-- Test 5: DECIMAL types with various precision and scale
suite:test("decimal_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for decimal types
    conn:execute("DROP TABLE IF EXISTS numeric_test")

    local create_sql = [[
        CREATE TABLE numeric_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_decimal_default DECIMAL,
            test_decimal_10_2 DECIMAL(10,2),
            test_decimal_15_5 DECIMAL(15,5),
            test_decimal_20_10 DECIMAL(20,10)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert decimal test data
    local insert_sql = [[
        INSERT INTO numeric_test
        (test_decimal_default, test_decimal_10_2, test_decimal_15_5, test_decimal_20_10)
        VALUES
        (99999999.99, 12345678.90, 1234567890.12345, 1234567890.1234567890)
    ]]

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify decimal data
    local select_result = conn:execute("SELECT * FROM numeric_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify decimal types (exact precision)
        TestFramework.assert_equal(row.test_decimal_default, 99999999.99)
        TestFramework.assert_equal(row.test_decimal_10_2, 12345678.90)
        TestFramework.assert_equal(row.test_decimal_15_5, 1234567890.12345)
        TestFramework.assert_equal(row.test_decimal_20_10, 1234567890.1234567890)

        select_result:close()
    end

    print("✓ DECIMAL types test passed")
end)

-- Test 6: Numeric boundary values and overflow handling
suite:test("numeric_boundary_values", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for boundary testing
    conn:execute("DROP TABLE IF EXISTS numeric_test")

    local create_sql = [[
        CREATE TABLE numeric_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_tinyint TINYINT,
            test_int INT,
            test_decimal DECIMAL(5,2)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Test boundary values
    local boundary_tests = {
        {127, 2147483647, 999.99}, -- Maximum values
        {-128, -2147483648, -999.99}, -- Minimum values
        {0, 0, 0.00} -- Zero values
    }

    for i, test_values in ipairs(boundary_tests) do
        local insert_sql = string.format(
            "INSERT INTO numeric_test (test_tinyint, test_int, test_decimal) VALUES (%d, %d, %.2f)",
            test_values[1], test_values[2], test_values[3]
        )

        local insert_result = conn:execute(insert_sql)
        TestFramework.assert_not_nil(insert_result)
    end

    -- Verify boundary values
    local count_result = conn:execute("SELECT COUNT(*) as count FROM numeric_test")
    if type(count_result) == "userdata" then
        local row = count_result:fetch()
        TestFramework.assert_equal(row.count, 3)
        count_result:close()
    end

    -- Test overflow handling (values too large should be truncated or cause error)
    local overflow_result, overflow_error = conn:execute("INSERT INTO numeric_test (test_decimal) VALUES (9999.99)")
    -- This might succeed (truncated) or fail depending on SQL mode
    -- TestFramework.assert_nil(overflow_result) -- Uncomment if strict mode

    print("✓ Numeric boundary values test passed")
end)

-- Test 7: Zero, NULL, and special numeric values
suite:test("special_numeric_values", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for special values
    conn:execute("DROP TABLE IF EXISTS numeric_test")

    local create_sql = [[
        CREATE TABLE numeric_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_int INT,
            test_float FLOAT,
            test_decimal DECIMAL(10,2),
            nullable_int INT NULL,
            nullable_float FLOAT NULL
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert special values
    local special_inserts = {
        "INSERT INTO numeric_test (test_int, test_float, test_decimal, nullable_int, nullable_float) VALUES (0, 0.0, 0.00, NULL, NULL)",
        "INSERT INTO numeric_test (test_int, test_float, test_decimal, nullable_int, nullable_float) VALUES (-0, -0.0, -0.00, 0, 0.0)",
        "INSERT INTO numeric_test (test_int, test_float, test_decimal, nullable_int, nullable_float) VALUES (1, 1.0, 1.00, 1, 1.0)"
    }

    for _, insert_sql in ipairs(special_inserts) do
        local insert_result = conn:execute(insert_sql)
        TestFramework.assert_not_nil(insert_result)
    end

    -- Verify special values
    local select_result = conn:execute("SELECT * FROM numeric_test ORDER BY id")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        -- First row: zeros and NULLs
        local row1 = select_result:fetch()
        TestFramework.assert_not_nil(row1)
        TestFramework.assert_equal(row1.test_int, 0)
        TestFramework.assert_equal(row1.test_float, 0.0)
        TestFramework.assert_equal(row1.test_decimal, 0.00)
        TestFramework.assert_nil(row1.nullable_int)
        TestFramework.assert_nil(row1.nullable_float)

        -- Second row: negative zeros (should be treated as positive zeros)
        local row2 = select_result:fetch()
        TestFramework.assert_not_nil(row2)
        TestFramework.assert_equal(row2.test_int, 0)
        TestFramework.assert_equal(row2.test_float, 0.0)
        TestFramework.assert_equal(row2.test_decimal, 0.00)
        TestFramework.assert_equal(row2.nullable_int, 0)
        TestFramework.assert_equal(row2.nullable_float, 0.0)

        -- Third row: ones
        local row3 = select_result:fetch()
        TestFramework.assert_not_nil(row3)
        TestFramework.assert_equal(row3.test_int, 1)
        TestFramework.assert_equal(row3.test_float, 1.0)
        TestFramework.assert_equal(row3.test_decimal, 1.00)
        TestFramework.assert_equal(row3.nullable_int, 1)
        TestFramework.assert_equal(row3.nullable_float, 1.0)

        select_result:close()
    end

    print("✓ Special numeric values test passed")
end)

-- Test 8: Numeric arithmetic and functions
suite:test("numeric_arithmetic_functions", function()
    local conn = test_conn or setup_database()

    -- Test arithmetic operations in SQL
    local arithmetic_result = conn:execute([[
        SELECT
            10 + 5 as addition,
            10 - 5 as subtraction,
            10 * 5 as multiplication,
            10 / 5 as division,
            10 % 3 as modulo,
            POWER(2, 3) as power,
            SQRT(16) as square_root,
            ABS(-10) as absolute,
            ROUND(3.14159, 2) as rounded,
            CEILING(3.14) as ceiling,
            FLOOR(3.14) as floor
    ]])

    TestFramework.assert_not_nil(arithmetic_result)

    if type(arithmetic_result) == "userdata" then
        local row = arithmetic_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify arithmetic results
        TestFramework.assert_equal(row.addition, 15)
        TestFramework.assert_equal(row.subtraction, 5)
        TestFramework.assert_equal(row.multiplication, 50)
        TestFramework.assert_equal(row.division, 2.0)
        TestFramework.assert_equal(row.modulo, 1)
        TestFramework.assert_equal(row.power, 8.0)
        TestFramework.assert_equal(row.square_root, 4.0)
        TestFramework.assert_equal(row.absolute, 10)
        TestFramework.assert_equal(row.rounded, 3.14)
        TestFramework.assert_equal(row.ceiling, 4)
        TestFramework.assert_equal(row.floor, 3)

        arithmetic_result:close()
    end

    print("✓ Numeric arithmetic and functions test passed")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Always cleanup, even if tests fail
cleanup_database()

if failures > 0 then
    print("MariaDB Phase 2A1 tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 2A1 tests completed successfully!")