#!/usr/bin/env lua

-- MariaDB Phase 6 Tests - Performance and Security Testing
-- Performance stress testing and security validation including SQL injection prevention,
-- connection security, and performance benchmarking
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')
local TestConfig = require('mariadb_test_config')

print("Starting MariaDB Phase 6 - Performance and Security tests...")

-- Create test suite using shared configuration
local suite = TestConfig.create_test_suite("MariaDB Phase 6 - Performance and Security Testing", TestFramework)

--[[
=============================================================================
PHASE 6: PERFORMANCE AND SECURITY TESTING
=============================================================================
]]

-- Test 12: Performance and Stress Testing
suite:test("performance_stress_testing", function()
    local conn = TestConfig.get_connection()

    -- Test 12.1: Large data volume operations and performance benchmarking
    print("  Testing large data volume operations and performance benchmarking...")

    -- Create performance test table
    conn:execute("DROP TABLE IF EXISTS test_performance")
    local create_perf_table = [[
        CREATE TABLE test_performance (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(255),
            data_payload TEXT,
            numeric_value DOUBLE,
            category_id INT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            INDEX idx_category (category_id),
            INDEX idx_numeric (numeric_value)
        )
    ]]
    local create_result = conn:execute(create_perf_table)
    TestFramework.assert_not_nil(create_result, "Failed to create performance test table")

    -- Performance test parameters
    local performance_records = 500
    local batch_size = 50

    -- Test 12.1.1: Bulk insert performance
    local start_time = os.clock()
    local successful_inserts = 0

    for i = 1, performance_records do
        local name = "PerfTest_" .. i
        local payload = string.rep("DataPayload" .. i, 5) -- ~50 chars
        local numeric_val = i * 2.5
        local category = (i % 10) + 1

        local stmt = conn:prepare("INSERT INTO test_performance (name, data_payload, numeric_value, category_id) VALUES (?, ?, ?, ?)")
        stmt:bind(name, payload, numeric_val, category)
        local insert_result = stmt:execute()
        stmt:close()

        if insert_result then
            successful_inserts = successful_inserts + 1
        end

        -- Progress reporting
        if i % 100 == 0 then
            local elapsed = os.clock() - start_time
            local rate = i / elapsed
            print("    ✓ Inserted " .. i .. " records (" .. string.format("%.1f", rate) .. " records/sec)")
        end
    end

    local insert_time = os.clock() - start_time
    local insert_rate = successful_inserts / insert_time

    TestFramework.assert_equal(successful_inserts, performance_records, "All inserts should succeed")
    print("    ✓ Bulk insert performance: " .. successful_inserts .. " records in " ..
          string.format("%.3f", insert_time) .. "s (" .. string.format("%.1f", insert_rate) .. " records/sec)")

    -- Test 12.1.2: Query performance benchmarking
    print("  Testing query performance benchmarking...")

    local query_tests = {
        {name = "full_scan", query = "SELECT COUNT(*) as count FROM test_performance"},
        {name = "indexed_filter", query = "SELECT COUNT(*) as count FROM test_performance WHERE category_id = 5"},
        {name = "range_query", query = "SELECT COUNT(*) as count FROM test_performance WHERE numeric_value BETWEEN 500 AND 1000"}
    }

    for _, test in ipairs(query_tests) do
        local query_start = os.clock()
        local query_result = conn:execute(test.query)
        TestFramework.assert_not_nil(query_result, "Query " .. test.name .. " should succeed")

        if type(query_result) == "userdata" then
            local row_count = 0
            local row = query_result:fetch()
            while row do
                row_count = row_count + 1
                row = query_result:fetch()
            end
            query_result:close()

            local query_time = os.clock() - query_start
            print("    ✓ Query " .. test.name .. ": " .. row_count .. " results in " ..
                  string.format("%.4f", query_time) .. "s")

            -- Performance thresholds (reasonable for test environment)
            TestFramework.assert_true(query_time < 2.0, "Query " .. test.name .. " should complete within 2 seconds")
        end
    end

    print("✓ Performance and stress testing completed successfully")
end)

-- Test 13: Security Testing
suite:test("security_testing", function()
    local conn = TestConfig.get_connection()

    -- Test 13.1: SQL injection prevention through prepared statements
    print("  Testing SQL injection prevention through prepared statements...")

    -- Create security test table
    conn:execute("DROP TABLE IF EXISTS test_security")
    local create_security_table = [[
        CREATE TABLE test_security (
            id INT AUTO_INCREMENT PRIMARY KEY,
            username VARCHAR(100),
            email VARCHAR(255),
            user_data TEXT,
            access_level INT DEFAULT 1,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]
    local create_result = conn:execute(create_security_table)
    TestFramework.assert_not_nil(create_result, "Failed to create security test table")

    -- Insert legitimate test data
    conn:execute("INSERT INTO test_security (username, email, user_data, access_level) VALUES ('admin', 'admin@test.com', 'Administrator account', 9)")
    conn:execute("INSERT INTO test_security (username, email, user_data, access_level) VALUES ('user1', 'user1@test.com', 'Regular user account', 1)")
    conn:execute("INSERT INTO test_security (username, email, user_data, access_level) VALUES ('guest', 'guest@test.com', 'Guest account', 0)")

    -- Test 13.1.1: SQL injection attempts with prepared statements
    local injection_attempts = {
        "'; DROP TABLE test_security; --",
        "' OR '1'='1",
        "' UNION SELECT * FROM test_security --",
        "admin'; UPDATE test_security SET access_level=9 WHERE username='guest'; --",
        "' OR 1=1 LIMIT 1 --"
    }

    print("    Testing SQL injection prevention with malicious inputs...")
    for i, malicious_input in ipairs(injection_attempts) do
        -- Test with prepared statement (should be safe)
        local safe_stmt = conn:prepare("SELECT id, username, access_level FROM test_security WHERE username = ?")
        TestFramework.assert_not_nil(safe_stmt, "Should create prepared statement for injection test " .. i)

        safe_stmt:bind(malicious_input)
        local safe_result = safe_stmt:execute()

        if type(safe_result) == "userdata" then
            local row_count = 0
            local row = safe_result:fetch()
            while row do
                row_count = row_count + 1
                -- Should not find any legitimate matches for injection strings
                TestFramework.assert_not_equal(row.username, "admin", "Injection should not match legitimate users")
                row = safe_result:fetch()
            end
            safe_result:close()
            print("    ✓ Injection attempt " .. i .. " safely handled (found " .. row_count .. " results)")
        end

        safe_stmt:close()
    end

    -- Verify data integrity after injection attempts
    local integrity_check = conn:execute("SELECT COUNT(*) as count FROM test_security")
    if type(integrity_check) == "userdata" then
        local row = integrity_check:fetch()
        TestFramework.assert_equal(tonumber(row.count), 3, "Should still have exactly 3 users after injection attempts")
        integrity_check:close()
    end

    -- Test 13.2: Parameter sanitization and validation
    print("  Testing parameter sanitization and validation...")

    -- Test with various potentially problematic inputs
    local sanitization_tests = {
        {name = "null_bytes", input = "user\x00admin", expected_safe = true},
        {name = "long_input", input = string.rep("A", 200), expected_safe = true},
        {name = "special_chars", input = "user<>\"'&", expected_safe = true}
    }

    for _, test in ipairs(sanitization_tests) do
        local sanitize_stmt = conn:prepare("SELECT COUNT(*) as count FROM test_security WHERE username = ?")
        TestFramework.assert_not_nil(sanitize_stmt, "Should create statement for sanitization test: " .. test.name)

        sanitize_stmt:bind(test.input)
        local sanitize_result = sanitize_stmt:execute()

        if type(sanitize_result) == "userdata" then
            local row = sanitize_result:fetch()
            TestFramework.assert_not_nil(row, "Sanitization test " .. test.name .. " should return result")
            -- The query should execute safely even with problematic input
            TestFramework.assert_true(tonumber(row.count) >= 0, "Should get valid count result")
            sanitize_result:close()
        end

        sanitize_stmt:close()
        print("    ✓ Sanitization test '" .. test.name .. "' passed")
    end

    -- Test 13.3: Connection security and authentication verification
    print("  Testing connection security and authentication verification...")

    -- Test connection with invalid credentials (should fail)
    local invalid_auth_tests = {
        {desc = "wrong_password", user = TestConfig.DB_CONFIG.user, password = "wrongpassword"},
        {desc = "wrong_user", user = "nonexistentuser", password = TestConfig.DB_CONFIG.password}
    }

    for _, test in ipairs(invalid_auth_tests) do
        local auth_success, auth_conn = pcall(function()
            return TestConfig.mariadb.connect(TestConfig.DB_CONFIG.database, test.user, test.password, TestConfig.DB_CONFIG.host, TestConfig.DB_CONFIG.port)
        end)

        -- Check if authentication properly failed (either pcall failed or connection is nil)
        local auth_failed = not auth_success or auth_conn == nil
        TestFramework.assert_true(auth_failed, "Authentication should fail for " .. test.desc)

        if auth_conn then
            auth_conn:close() -- Clean up if somehow succeeded
        end

        print("    ✓ Authentication test '" .. test.desc .. "' correctly failed")
    end

    -- Verify current connection is still valid after auth tests
    local connection_check = conn:execute("SELECT 'connection_valid' as status")
    TestFramework.assert_not_nil(connection_check, "Main connection should remain valid after auth tests")
    if type(connection_check) == "userdata" then
        connection_check:close()
    end

    -- Final security verification - ensure we have the 3 base users
    local final_count = conn:execute("SELECT COUNT(*) as count FROM test_security")
    if type(final_count) == "userdata" then
        local row = final_count:fetch()
        local count = tonumber(row.count)
        TestFramework.assert_equal(count, 3, "Should have exactly 3 base users after all security tests")
        final_count:close()
    end

    print("✓ Security testing completed successfully")
end)

--[[
=============================================================================
TEST EXECUTION
=============================================================================
]]

-- Run the Phase 6 test suite
local failures = TestFramework.run_suite(suite)

if failures > 0 then
    print("MariaDB Phase 6 - Performance and Security tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 6 - Performance and Security tests completed successfully!")