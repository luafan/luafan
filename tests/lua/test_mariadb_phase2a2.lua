#!/usr/bin/env lua

-- MariaDB Phase 2A2 Tests - String Data Types
-- Tests comprehensive string data types storage, retrieval and character handling
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')

print("Starting MariaDB Phase 2A2 tests - String Data Types...")

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
    test_conn:execute("DROP TABLE IF EXISTS string_test")

    print("✓ MariaDB connection established")
    return test_conn
end

local function cleanup_database()
    if test_conn then
        print("Cleaning up database...")
        test_conn:execute("DROP TABLE IF EXISTS string_test")
        test_conn:close()
        test_conn = nil
        print("✓ Database cleanup completed")
    end
end

-- Create test suite
local suite = TestFramework.create_suite("MariaDB Phase 2A2 - String Data Types")

-- Test 1: CHAR and VARCHAR types
suite:test("char_varchar_types", function()
    local conn = test_conn or setup_database()

    -- Create table with CHAR and VARCHAR types
    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_char_5 CHAR(5),
            test_char_10 CHAR(10),
            test_varchar_50 VARCHAR(50),
            test_varchar_255 VARCHAR(255)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert test data with various string lengths
    local test_data = {
        char_5 = "HELLO",      -- Exactly 5 characters
        char_10 = "TEST",      -- Less than 10 characters (will be padded)
        varchar_50 = "This is a VARCHAR test string",
        varchar_255 = "This is a longer VARCHAR string that tests the 255 character limit with various content including numbers 12345 and symbols !@#$%"
    }

    local insert_sql = string.format([[
        INSERT INTO string_test (test_char_5, test_char_10, test_varchar_50, test_varchar_255)
        VALUES ('%s', '%s', '%s', '%s')
    ]],
        test_data.char_5,
        test_data.char_10,
        conn:escape(test_data.varchar_50),
        conn:escape(test_data.varchar_255)
    )

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify data
    local select_result = conn:execute("SELECT * FROM string_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify CHAR and VARCHAR types
        TestFramework.assert_equal(row.test_char_5, test_data.char_5)
        -- CHAR fields are right-padded with spaces, need to trim for comparison
        TestFramework.assert_equal(row.test_char_10:gsub("%s+$", ""), test_data.char_10)
        TestFramework.assert_equal(row.test_varchar_50, test_data.varchar_50)
        TestFramework.assert_equal(row.test_varchar_255, test_data.varchar_255)

        select_result:close()
    end

    print("✓ CHAR and VARCHAR types test passed")
end)

-- Test 2: TEXT types (TEXT, MEDIUMTEXT, LONGTEXT)
suite:test("text_types", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for TEXT types
    conn:execute("DROP TABLE IF EXISTS string_test")

    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_text TEXT,
            test_mediumtext MEDIUMTEXT,
            test_longtext LONGTEXT
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Create test strings of different lengths
    local short_text = "This is a TEXT field test with reasonable length content."
    local medium_text = "This is a MEDIUMTEXT field test. " .. string.rep("Medium length content repeated multiple times. ", 100)
    local long_text = "This is a LONGTEXT field test. " .. string.rep("Very long content for testing LONGTEXT capacity. ", 500)

    local insert_sql = string.format([[
        INSERT INTO string_test (test_text, test_mediumtext, test_longtext)
        VALUES ('%s', '%s', '%s')
    ]],
        conn:escape(short_text),
        conn:escape(medium_text),
        conn:escape(long_text)
    )

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify TEXT data
    local select_result = conn:execute("SELECT * FROM string_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify TEXT types
        TestFramework.assert_equal(row.test_text, short_text)
        TestFramework.assert_equal(row.test_mediumtext, medium_text)
        TestFramework.assert_equal(row.test_longtext, long_text)

        -- Verify lengths
        TestFramework.assert_equal(#row.test_text, #short_text)
        TestFramework.assert_equal(#row.test_mediumtext, #medium_text)
        TestFramework.assert_equal(#row.test_longtext, #long_text)

        select_result:close()
    end

    print("✓ TEXT types test passed")
end)

-- Test 3: String length limits and truncation
suite:test("string_length_limits", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for length testing
    conn:execute("DROP TABLE IF EXISTS string_test")

    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_char_3 CHAR(3),
            test_varchar_10 VARCHAR(10)
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Test exact length matches
    local exact_char_result = conn:execute("INSERT INTO string_test (test_char_3, test_varchar_10) VALUES ('ABC', '1234567890')")
    TestFramework.assert_not_nil(exact_char_result)

    -- Test strings shorter than limit
    local short_result = conn:execute("INSERT INTO string_test (test_char_3, test_varchar_10) VALUES ('AB', '12345')")
    TestFramework.assert_not_nil(short_result)

    -- Test empty strings
    local empty_result = conn:execute("INSERT INTO string_test (test_char_3, test_varchar_10) VALUES ('', '')")
    TestFramework.assert_not_nil(empty_result)

    -- Verify all insertions
    local select_result = conn:execute("SELECT * FROM string_test ORDER BY id")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        -- Exact length
        local row1 = select_result:fetch()
        TestFramework.assert_not_nil(row1)
        TestFramework.assert_equal(row1.test_char_3, "ABC")
        TestFramework.assert_equal(row1.test_varchar_10, "1234567890")

        -- Short length (CHAR will be padded)
        local row2 = select_result:fetch()
        TestFramework.assert_not_nil(row2)
        TestFramework.assert_equal(row2.test_char_3:gsub("%s+$", ""), "AB")
        TestFramework.assert_equal(row2.test_varchar_10, "12345")

        -- Empty strings
        local row3 = select_result:fetch()
        TestFramework.assert_not_nil(row3)
        TestFramework.assert_equal(row3.test_char_3:gsub("%s+$", ""), "")
        TestFramework.assert_equal(row3.test_varchar_10, "")

        select_result:close()
    end

    print("✓ String length limits test passed")
end)

-- Test 4: Special characters and escaping
suite:test("special_characters_escaping", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for special character testing
    conn:execute("DROP TABLE IF EXISTS string_test")

    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_special_chars VARCHAR(255),
            test_quotes VARCHAR(255),
            test_newlines TEXT
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Test strings with special characters
    local special_chars = "!@#$%^&*()_+-=[]{}|;:,.<>?/~`"
    local quotes_string = "Single 'quotes' and \"double quotes\" and `backticks`"
    local newlines_string = "First line\nSecond line\r\nThird line\tWith tab"

    local insert_sql = string.format([[
        INSERT INTO string_test (test_special_chars, test_quotes, test_newlines)
        VALUES ('%s', '%s', '%s')
    ]],
        conn:escape(special_chars),
        conn:escape(quotes_string),
        conn:escape(newlines_string)
    )

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify special characters
    local select_result = conn:execute("SELECT * FROM string_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify special characters are preserved
        TestFramework.assert_equal(row.test_special_chars, special_chars)
        TestFramework.assert_equal(row.test_quotes, quotes_string)
        TestFramework.assert_equal(row.test_newlines, newlines_string)

        select_result:close()
    end

    print("✓ Special characters and escaping test passed")
end)

-- Test 5: Unicode and UTF-8 support
suite:test("unicode_utf8_support", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table with UTF-8 charset
    conn:execute("DROP TABLE IF EXISTS string_test")

    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_chinese VARCHAR(255) CHARACTER SET utf8mb4,
            test_emoji VARCHAR(255) CHARACTER SET utf8mb4,
            test_mixed TEXT CHARACTER SET utf8mb4
        ) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Test strings with Unicode characters
    local chinese_text = "你好世界！这是中文测试。"
    local emoji_text = "Hello 👋 World 🌍 with emojis 😀🎉🚀"
    local mixed_text = "Mixed: English, 中文, العربية, русский, 日本語, 🌟✨"

    local insert_sql = string.format([[
        INSERT INTO string_test (test_chinese, test_emoji, test_mixed)
        VALUES ('%s', '%s', '%s')
    ]],
        conn:escape(chinese_text),
        conn:escape(emoji_text),
        conn:escape(mixed_text)
    )

    result = conn:execute(insert_sql)
    TestFramework.assert_not_nil(result)

    -- Retrieve and verify Unicode data
    local select_result = conn:execute("SELECT * FROM string_test WHERE id = 1")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify Unicode characters are preserved
        TestFramework.assert_equal(row.test_chinese, chinese_text)
        TestFramework.assert_equal(row.test_emoji, emoji_text)
        TestFramework.assert_equal(row.test_mixed, mixed_text)

        select_result:close()
    end

    print("✓ Unicode and UTF-8 support test passed")
end)

-- Test 6: String functions and operations
suite:test("string_functions_operations", function()
    local conn = test_conn or setup_database()

    -- Test various string functions in SQL
    local functions_result = conn:execute([[
        SELECT
            CONCAT('Hello', ' ', 'World') as concatenation,
            LENGTH('Hello World') as string_length,
            CHAR_LENGTH('Hello 世界') as char_length,
            UPPER('hello world') as uppercase,
            LOWER('HELLO WORLD') as lowercase,
            SUBSTRING('Hello World', 7, 5) as substring,
            LEFT('Hello World', 5) as left_part,
            RIGHT('Hello World', 5) as right_part,
            REPLACE('Hello World', 'World', 'Universe') as replacement,
            TRIM('  Hello World  ') as trimmed,
            REVERSE('Hello') as reversed
    ]])

    TestFramework.assert_not_nil(functions_result)

    if type(functions_result) == "userdata" then
        local row = functions_result:fetch()
        TestFramework.assert_not_nil(row)

        -- Verify string function results
        TestFramework.assert_equal(row.concatenation, "Hello World")
        TestFramework.assert_equal(row.string_length, 11)
        TestFramework.assert_equal(row.char_length, 8) -- 'Hello 世界' has 8 characters
        TestFramework.assert_equal(row.uppercase, "HELLO WORLD")
        TestFramework.assert_equal(row.lowercase, "hello world")
        TestFramework.assert_equal(row.substring, "World")
        TestFramework.assert_equal(row.left_part, "Hello")
        TestFramework.assert_equal(row.right_part, "World")
        TestFramework.assert_equal(row.replacement, "Hello Universe")
        TestFramework.assert_equal(row.trimmed, "Hello World")
        TestFramework.assert_equal(row.reversed, "olleH")

        functions_result:close()
    end

    print("✓ String functions and operations test passed")
end)

-- Test 7: String comparison and collation
suite:test("string_comparison_collation", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for collation testing
    conn:execute("DROP TABLE IF EXISTS string_test")

    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            test_case_sensitive VARCHAR(50) COLLATE utf8mb4_bin,
            test_case_insensitive VARCHAR(50) COLLATE utf8mb4_general_ci
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert test data for comparison
    local inserts = {
        "INSERT INTO string_test (test_case_sensitive, test_case_insensitive) VALUES ('Hello', 'Hello')",
        "INSERT INTO string_test (test_case_sensitive, test_case_insensitive) VALUES ('hello', 'hello')",
        "INSERT INTO string_test (test_case_sensitive, test_case_insensitive) VALUES ('HELLO', 'HELLO')"
    }

    for _, insert_sql in ipairs(inserts) do
        local insert_result = conn:execute(insert_sql)
        TestFramework.assert_not_nil(insert_result)
    end

    -- Test case-sensitive comparison
    local case_sensitive_result = conn:execute("SELECT COUNT(*) as count FROM string_test WHERE test_case_sensitive = 'Hello'")
    if type(case_sensitive_result) == "userdata" then
        local row = case_sensitive_result:fetch()
        TestFramework.assert_equal(row.count, 1) -- Only exact match
        case_sensitive_result:close()
    end

    -- Test case-insensitive comparison
    local case_insensitive_result = conn:execute("SELECT COUNT(*) as count FROM string_test WHERE test_case_insensitive = 'hello'")
    if type(case_insensitive_result) == "userdata" then
        local row = case_insensitive_result:fetch()
        TestFramework.assert_equal(row.count, 3) -- All variations match
        case_insensitive_result:close()
    end

    print("✓ String comparison and collation test passed")
end)

-- Test 8: NULL string handling
suite:test("null_string_handling", function()
    local conn = test_conn or setup_database()

    -- Drop and recreate table for NULL testing
    conn:execute("DROP TABLE IF EXISTS string_test")

    local create_sql = [[
        CREATE TABLE string_test (
            id INT AUTO_INCREMENT PRIMARY KEY,
            nullable_varchar VARCHAR(100),
            nullable_text TEXT,
            not_null_varchar VARCHAR(100) NOT NULL DEFAULT ''
        )
    ]]

    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result)

    -- Insert NULL values and empty strings
    local null_inserts = {
        "INSERT INTO string_test (nullable_varchar, nullable_text, not_null_varchar) VALUES (NULL, NULL, 'not null')",
        "INSERT INTO string_test (nullable_varchar, nullable_text, not_null_varchar) VALUES ('', '', '')",
        "INSERT INTO string_test (nullable_varchar, nullable_text, not_null_varchar) VALUES ('test', 'test text', 'test')"
    }

    for _, insert_sql in ipairs(null_inserts) do
        local insert_result = conn:execute(insert_sql)
        TestFramework.assert_not_nil(insert_result)
    end

    -- Verify NULL vs empty string handling
    local select_result = conn:execute("SELECT * FROM string_test ORDER BY id")
    TestFramework.assert_not_nil(select_result)

    if type(select_result) == "userdata" then
        -- NULL values
        local row1 = select_result:fetch()
        TestFramework.assert_not_nil(row1)
        TestFramework.assert_nil(row1.nullable_varchar)
        TestFramework.assert_nil(row1.nullable_text)
        TestFramework.assert_equal(row1.not_null_varchar, "not null")

        -- Empty strings
        local row2 = select_result:fetch()
        TestFramework.assert_not_nil(row2)
        TestFramework.assert_equal(row2.nullable_varchar, "")
        TestFramework.assert_equal(row2.nullable_text, "")
        TestFramework.assert_equal(row2.not_null_varchar, "")

        -- Regular values
        local row3 = select_result:fetch()
        TestFramework.assert_not_nil(row3)
        TestFramework.assert_equal(row3.nullable_varchar, "test")
        TestFramework.assert_equal(row3.nullable_text, "test text")
        TestFramework.assert_equal(row3.not_null_varchar, "test")

        select_result:close()
    end

    print("✓ NULL string handling test passed")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Always cleanup, even if tests fail
cleanup_database()

if failures > 0 then
    print("MariaDB Phase 2A2 tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 2A2 tests completed successfully!")