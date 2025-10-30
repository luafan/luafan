#!/usr/bin/env lua

-- MariaDB Phase 5 Tests - Cursor and Memory Management
-- Cursor operations and memory management testing including result set traversal,
-- garbage collection verification, and resource cleanup validation
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')
local TestConfig = require('mariadb_test_config')

print("Starting MariaDB Phase 5 - Cursor and Memory Management tests...")

-- Create test suite using shared configuration
local suite = TestConfig.create_test_suite("MariaDB Phase 5 - Cursor and Memory Management", TestFramework)

--[[
=============================================================================
PHASE 5: CURSOR AND MEMORY MANAGEMENT
=============================================================================
]]

-- Test 10: Cursor Operations
suite:test("cursor_operations", function()
    local conn = TestConfig.get_connection()

    -- Test 10.1: Basic result set cursor traversal
    print("  Testing basic result set cursor traversal...")

    -- Create test table with substantial data for cursor operations
    conn:execute("DROP TABLE IF EXISTS test_cursor_data")
    local create_cursor_table = [[
        CREATE TABLE test_cursor_data (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100),
            value INT,
            category VARCHAR(50),
            data_size ENUM('small', 'medium', 'large'),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]
    local create_result = conn:execute(create_cursor_table)
    TestFramework.assert_not_nil(create_result, "Failed to create cursor test table")

    -- Insert test data for cursor operations
    local categories = {"A", "B", "C", "D"}
    local sizes = {"small", "medium", "large"}
    local total_records = 50

    for i = 1, total_records do
        local category = categories[(i % #categories) + 1]
        local size = sizes[(i % #sizes) + 1]
        local cursor_stmt = conn:prepare("INSERT INTO test_cursor_data (name, value, category, data_size) VALUES (?, ?, ?, ?)")
        cursor_stmt:bind("record_" .. i, i * 10, category, size)
        local insert_result = cursor_stmt:execute()
        cursor_stmt:close()
        TestFramework.assert_not_nil(insert_result, "Failed to insert cursor test record " .. i)
    end

    print("    ✓ Created test table with " .. total_records .. " records")

    -- Test 10.2: Forward-only cursor behavior and basic traversal
    print("  Testing forward-only cursor behavior and basic traversal...")

    local cursor_result = conn:execute("SELECT id, name, value, category FROM test_cursor_data ORDER BY id")
    TestFramework.assert_not_nil(cursor_result, "Failed to create cursor result set")

    if type(cursor_result) == "userdata" then
        local row_count = 0
        local previous_id = 0

        -- Traverse cursor forward-only
        local row = cursor_result:fetch()
        while row do
            row_count = row_count + 1
            local current_id = tonumber(row.id)

            -- Verify forward-only traversal order
            TestFramework.assert_true(current_id > previous_id, "Cursor should traverse forward-only")
            TestFramework.assert_not_nil(row.name, "Each row should have name field")
            TestFramework.assert_not_nil(row.value, "Each row should have value field")
            TestFramework.assert_not_nil(row.category, "Each row should have category field")

            previous_id = current_id
            row = cursor_result:fetch()
        end

        cursor_result:close()
        TestFramework.assert_equal(row_count, total_records, "Should traverse all records")
        print("    ✓ Forward cursor traversed " .. row_count .. " records successfully")
    end

    -- Test 10.3: Large result set batch processing simulation
    print("  Testing large result set batch processing simulation...")

    local batch_size = 10
    local total_batches = 0
    local total_processed = 0

    local large_result = conn:execute("SELECT id, name, value FROM test_cursor_data ORDER BY id")
    TestFramework.assert_not_nil(large_result, "Failed to create large result set")

    if type(large_result) == "userdata" then
        local batch_records = {}
        local row = large_result:fetch()

        while row do
            table.insert(batch_records, row)

            -- Process batch when it reaches batch_size
            if #batch_records >= batch_size then
                total_batches = total_batches + 1
                total_processed = total_processed + #batch_records

                -- Simulate batch processing
                local batch_sum = 0
                for _, record in ipairs(batch_records) do
                    batch_sum = batch_sum + tonumber(record.value)
                end

                TestFramework.assert_true(batch_sum > 0, "Batch " .. total_batches .. " should have positive sum")

                -- Clear batch for next iteration
                batch_records = {}
            end

            row = large_result:fetch()
        end

        -- Process remaining records in final partial batch
        if #batch_records > 0 then
            total_batches = total_batches + 1
            total_processed = total_processed + #batch_records
        end

        large_result:close()
        TestFramework.assert_equal(total_processed, total_records, "Should process all records in batches")
        print("    ✓ Batch processing completed: " .. total_batches .. " batches, " .. total_processed .. " total records")
    end

    print("✓ Cursor operations tests completed successfully")
end)

-- Test 11: Memory Management
suite:test("memory_management", function()
    local conn = TestConfig.get_connection()

    -- Test 11.1: Garbage collection verification and memory cleanup
    print("  Testing garbage collection verification and memory cleanup...")

    -- Force initial garbage collection
    collectgarbage("collect")
    local initial_memory = collectgarbage("count")
    print("    ✓ Initial memory usage: " .. string.format("%.2f", initial_memory) .. " KB")

    -- Create test table for memory management tests
    conn:execute("DROP TABLE IF EXISTS test_memory")
    local create_memory_table = [[
        CREATE TABLE test_memory (
            id INT AUTO_INCREMENT PRIMARY KEY,
            data_field TEXT,
            binary_field BLOB,
            number_field DOUBLE,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]
    local create_result = conn:execute(create_memory_table)
    TestFramework.assert_not_nil(create_result, "Failed to create memory test table")

    -- Test memory usage during data operations
    local operations_count = 100
    for i = 1, operations_count do
        local large_text = string.rep("MemoryTest" .. i, 50) -- ~500 chars each
        local binary_data = string.rep("\x00\x01\x02\x03", 25) -- 100 bytes each

        local memory_stmt = conn:prepare("INSERT INTO test_memory (data_field, binary_field, number_field) VALUES (?, ?, ?)")
        memory_stmt:bind(large_text, binary_data, i * 3.14159)
        local insert_result = memory_stmt:execute()
        memory_stmt:close()
        TestFramework.assert_not_nil(insert_result, "Memory test insert " .. i .. " should succeed")

        -- Periodic garbage collection during operations
        if i % 25 == 0 then
            collectgarbage("collect")
            local current_memory = collectgarbage("count")
            print("    ✓ Memory after " .. i .. " operations: " .. string.format("%.2f", current_memory) .. " KB")
        end
    end

    -- Final garbage collection and memory check
    collectgarbage("collect")
    local post_operations_memory = collectgarbage("count")
    print("    ✓ Post-operations memory: " .. string.format("%.2f", post_operations_memory) .. " KB")

    -- Test 11.2: Large data processing memory usage
    print("  Testing large data processing memory usage...")

    -- Process large result sets to test memory efficiency
    local large_query = "SELECT id, data_field, binary_field FROM test_memory ORDER BY id"
    local large_result = conn:execute(large_query)
    TestFramework.assert_not_nil(large_result, "Failed to create large result set for memory test")

    if type(large_result) == "userdata" then
        local processed_records = 0
        local memory_samples = {}

        local row = large_result:fetch()
        while row do
            processed_records = processed_records + 1

            -- Verify we can access large data fields
            TestFramework.assert_not_nil(row.data_field, "Should have data field")
            TestFramework.assert_not_nil(row.binary_field, "Should have binary field")

            -- Periodic memory sampling
            if processed_records % 20 == 0 then
                collectgarbage("collect")
                local sample_memory = collectgarbage("count")
                table.insert(memory_samples, sample_memory)
            end

            row = large_result:fetch()
        end

        large_result:close()
        TestFramework.assert_equal(processed_records, operations_count, "Should process all records")

        -- Analyze memory usage pattern
        if #memory_samples > 1 then
            local max_memory = math.max(table.unpack(memory_samples))
            local min_memory = math.min(table.unpack(memory_samples))
            local memory_variance = max_memory - min_memory
            print("    ✓ Memory variance during processing: " .. string.format("%.2f", memory_variance) .. " KB")

            -- Memory should not grow excessively during processing
            TestFramework.assert_true(memory_variance < 10000, "Memory variance should be reasonable (< 10MB)")
        end
    end

    -- Test 11.3: Final memory cleanup verification
    print("  Testing final memory cleanup verification...")

    -- Clean up all test data
    conn:execute("DELETE FROM test_memory")

    -- Force final garbage collection
    collectgarbage("collect")
    local final_memory = collectgarbage("count")

    -- Calculate total memory usage during test
    local total_memory_used = final_memory - initial_memory
    print("    ✓ Final memory usage: " .. string.format("%.2f", final_memory) .. " KB")
    print("    ✓ Total memory delta: " .. string.format("%.2f", total_memory_used) .. " KB")

    -- Verify connection is still functional after memory tests
    local functionality_test = conn:execute("SELECT 'memory_tests_complete' as status")
    TestFramework.assert_not_nil(functionality_test, "Connection should remain functional after memory tests")

    if type(functionality_test) == "userdata" then
        local row = functionality_test:fetch()
        TestFramework.assert_equal(row.status, "memory_tests_complete", "Should get expected status")
        functionality_test:close()
    end

    print("✓ Memory management tests completed successfully")
end)

--[[
=============================================================================
TEST EXECUTION
=============================================================================
]]

-- Run the Phase 5 test suite
local failures = TestFramework.run_suite(suite)

if failures > 0 then
    print("MariaDB Phase 5 - Cursor and Memory Management tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 5 - Cursor and Memory Management tests completed successfully!")