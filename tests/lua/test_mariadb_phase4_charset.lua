#!/usr/bin/env lua

-- MariaDB Phase 4 Tests - Character Set and Collation Support
-- Character set, collation, and Unicode support testing including
-- multi-language data handling and encoding verification
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')
local TestConfig = require('mariadb_test_config')

print("Starting MariaDB Phase 4 - Character Set and Collation tests...")

-- Create test suite using shared configuration
local suite = TestConfig.create_test_suite("MariaDB Phase 4 - Character Set and Collation Support", TestFramework)

--[[
=============================================================================
PHASE 4: CHARACTER SET AND COLLATION SUPPORT
=============================================================================
]]

-- Test 8: Character Set and Collation Support
suite:test("charset_collation_support", function()
    local conn = TestConfig.get_connection()

    -- Test 8.1: Connection character set verification and changes
    print("  Testing connection character set verification and changes...")

    -- Get current character set
    local charset_result = conn:execute("SELECT @@character_set_connection, @@collation_connection")
    TestFramework.assert_not_nil(charset_result, "Should be able to query character set")

    local current_charset = nil
    local current_collation = nil
    if type(charset_result) == "userdata" then
        local row = charset_result:fetch()
        if row then
            current_charset = row["@@character_set_connection"]
            current_collation = row["@@collation_connection"]
            print("    ✓ Current charset: " .. (current_charset or "unknown") ..
                  ", collation: " .. (current_collation or "unknown"))
        end
        charset_result:close()
    end

    -- Test character set change functionality
    local charset_change_success = pcall(function()
        return conn:setcharset("utf8mb4")
    end)

    if charset_change_success then
        print("    ✓ Character set change to utf8mb4 succeeded")

        -- Verify the change
        local verify_result = conn:execute("SELECT @@character_set_connection")
        if type(verify_result) == "userdata" then
            local row = verify_result:fetch()
            if row then
                local new_charset = row["@@character_set_connection"]
                TestFramework.assert_equal(new_charset, "utf8mb4", "Character set should be changed to utf8mb4")
            end
            verify_result:close()
        end
    else
        print("    ⚠ Character set change not supported or failed")
    end

    -- Test 8.2: Multi-character set data insertion and retrieval
    print("  Testing multi-character set data insertion and retrieval...")

    -- Create test table with different character sets
    conn:execute("DROP TABLE IF EXISTS test_charset")
    local create_sql = [[
        CREATE TABLE test_charset (
            id INT AUTO_INCREMENT PRIMARY KEY,
            text_utf8 TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci,
            text_latin1 TEXT CHARACTER SET latin1 COLLATE latin1_swedish_ci,
            text_binary BLOB,
            description VARCHAR(100),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]
    local create_result = conn:execute(create_sql)
    TestFramework.assert_not_nil(create_result, "Failed to create charset test table")

    -- Test UTF8 data insertion
    local utf8_data = "Unicode test: 中文测试 🌟 αβγ δεζ"
    local latin1_data = "Latin1 test: Hello World áéíóú"
    local binary_data = "\x48\x65\x6C\x6C\x6F\x00\x57\x6F\x72\x6C\x64" -- "Hello\0World"

    local insert_stmt = conn:prepare("INSERT INTO test_charset (text_utf8, text_latin1, text_binary, description) VALUES (?, ?, ?, ?)")
    insert_stmt:bind(utf8_data, latin1_data, binary_data, "multi_charset_test")
    local insert_result = insert_stmt:execute()
    insert_stmt:close()
    TestFramework.assert_not_nil(insert_result, "Failed to insert multi-charset data")

    -- Verify data retrieval
    local select_result = conn:execute("SELECT text_utf8, text_latin1, text_binary, description FROM test_charset WHERE description = 'multi_charset_test'")
    TestFramework.assert_not_nil(select_result, "Failed to select multi-charset data")

    if type(select_result) == "userdata" then
        local row = select_result:fetch()
        TestFramework.assert_not_nil(row, "Should retrieve inserted data")

        if row then
            TestFramework.assert_equal(row.text_utf8, utf8_data, "UTF8 data should match")
            TestFramework.assert_equal(row.text_latin1, latin1_data, "Latin1 data should match")
            TestFramework.assert_equal(row.text_binary, binary_data, "Binary data should match")
            print("    ✓ Multi-charset data inserted and retrieved successfully")
        end
        select_result:close()
    end

    -- Test 8.3: Unicode support validation
    print("  Testing Unicode support validation...")

    local unicode_test_cases = {
        {name = "chinese", data = "中文测试数据"},
        {name = "japanese", data = "日本語テスト"},
        {name = "korean", data = "한국어 테스트"},
        {name = "emoji", data = "😀😃😄😁😆😅😂🤣"},
        {name = "arabic", data = "العربية اختبار"},
        {name = "russian", data = "Русский тест"},
        {name = "greek", data = "Ελληνικά δοκιμή"}
    }

    for i, test_case in ipairs(unicode_test_cases) do
        local unicode_stmt = conn:prepare("INSERT INTO test_charset (text_utf8, description) VALUES (?, ?)")
        unicode_stmt:bind(test_case.data, "unicode_" .. test_case.name)
        local unicode_insert = unicode_stmt:execute()
        unicode_stmt:close()
        TestFramework.assert_not_nil(unicode_insert, "Failed to insert " .. test_case.name .. " Unicode data")
    end

    -- Verify Unicode data integrity
    local unicode_count = conn:execute("SELECT COUNT(*) as count FROM test_charset WHERE description LIKE 'unicode_%'")
    if type(unicode_count) == "userdata" then
        local row = unicode_count:fetch()
        TestFramework.assert_equal(tonumber(row.count), #unicode_test_cases, "Should have all Unicode test cases")
        unicode_count:close()
    end

    print("    ✓ Unicode support validation completed for " .. #unicode_test_cases .. " languages")

    -- Test 8.4: Final data count verification
    local final_count = conn:execute("SELECT COUNT(*) as total FROM test_charset")
    if type(final_count) == "userdata" then
        local row = final_count:fetch()
        local total_records = tonumber(row.total)
        TestFramework.assert_true(total_records >= 8, "Should have substantial test data from charset tests")
        print("    ✓ Total test records created: " .. total_records)
        final_count:close()
    end

    print("✓ Character set and collation support tests completed successfully")
end)

--[[
=============================================================================
TEST EXECUTION
=============================================================================
]]

-- Run the Phase 4 test suite
local failures = TestFramework.run_suite(suite)

if failures > 0 then
    print("MariaDB Phase 4 - Character Set and Collation tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 4 - Character Set and Collation tests completed successfully!")