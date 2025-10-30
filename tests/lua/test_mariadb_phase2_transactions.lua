#!/usr/bin/env lua

-- MariaDB Phase 2 Tests - Transaction Processing Detailed Testing
-- Advanced transaction operations including rollback, exception handling,
-- autocommit mode behavior, and transaction state consistency
-- Requires Docker MariaDB server running (use: cd tests && ./docker-setup.sh start)

local TestFramework = require('test_framework')
local TestConfig = require('mariadb_test_config')

print("Starting MariaDB Phase 2 - Transaction Processing tests...")

-- Create test suite using shared configuration
local suite = TestConfig.create_test_suite("MariaDB Phase 2 - Transaction Processing Detailed Testing", TestFramework)

--[[
=============================================================================
PHASE 2: TRANSACTION PROCESSING DETAILED TESTING
=============================================================================
]]

-- Test 4: Advanced Transaction Operations
suite:test("advanced_transaction_operations", function()
    local conn = TestConfig.get_connection()

    -- Create test table for transaction testing
    conn:execute("DROP TABLE IF EXISTS test_transactions")
    local create_sql = [[
        CREATE TABLE test_transactions (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100),
            amount DECIMAL(10,2),
            status VARCHAR(20)
        )
    ]]
    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result, "Failed to create test_transactions table")

    -- Test 4.1: Basic transaction operations (manual transaction control)
    print("  Testing basic manual transaction control...")

    -- Test autocommit mode control
    print("    Testing autocommit mode control...")
    local autocommit_off = conn:autocommit(false)
    TestFramework.assert_true(autocommit_off, "Failed to turn off autocommit")
    print("      ✓ Autocommit disabled successfully")

    -- Insert data without committing
    local insert_result1 = conn:execute("INSERT INTO test_transactions (name, amount, status) VALUES ('Transaction1', 100.50, 'pending')")
    TestFramework.assert_not_nil(insert_result1, "Failed to insert first transaction record")

    local insert_result2 = conn:execute("INSERT INTO test_transactions (name, amount, status) VALUES ('Transaction2', 200.75, 'pending')")
    TestFramework.assert_not_nil(insert_result2, "Failed to insert second transaction record")

    print("      ✓ Test data inserted without commit")

    -- Verify data exists in current transaction
    local verify_result = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'pending'")
    TestFramework.assert_not_nil(verify_result, "Failed to execute verification query")

    if type(verify_result) == "userdata" then
        local row = verify_result:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch verification result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 2, "Should have 2 pending transactions in current transaction")
        verify_result:close()
    end

    print("      ✓ Data visible within transaction")

    -- Test manual commit
    print("    Testing manual commit...")
    local commit_result = conn:commit()
    TestFramework.assert_true(commit_result, "Failed to commit transaction")
    print("      ✓ Transaction committed successfully")

    -- Verify data persists after commit
    local verify_commit = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'pending'")
    TestFramework.assert_not_nil(verify_commit, "Failed to execute post-commit verification query")

    if type(verify_commit) == "userdata" then
        local row = verify_commit:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch post-commit result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 2, "Should have 2 pending transactions after commit")
        verify_commit:close()
    end

    print("  ✓ Basic manual transaction control completed successfully")

    -- Test 4.2: Transaction rollback functionality
    print("  Testing transaction rollback functionality...")

    -- Insert more data without committing
    local insert_result3 = conn:execute("INSERT INTO test_transactions (name, amount, status) VALUES ('Transaction3', 300.25, 'rollback_test')")
    TestFramework.assert_not_nil(insert_result3, "Failed to insert rollback test record")

    local insert_result4 = conn:execute("INSERT INTO test_transactions (name, amount, status) VALUES ('Transaction4', 400.00, 'rollback_test')")
    TestFramework.assert_not_nil(insert_result4, "Failed to insert second rollback test record")

    -- Verify rollback test data exists in current transaction
    local verify_rollback_data = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'rollback_test'")
    TestFramework.assert_not_nil(verify_rollback_data, "Failed to execute rollback verification query")

    if type(verify_rollback_data) == "userdata" then
        local row = verify_rollback_data:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch rollback verification result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 2, "Should have 2 rollback test transactions before rollback")
        verify_rollback_data:close()
    end

    print("    ✓ Rollback test data inserted")

    -- Perform rollback
    local rollback_result = conn:rollback()
    TestFramework.assert_true(rollback_result, "Failed to rollback transaction")
    print("    ✓ Transaction rolled back successfully")

    -- Verify rollback test data no longer exists
    local verify_rollback_gone = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'rollback_test'")
    TestFramework.assert_not_nil(verify_rollback_gone, "Failed to execute post-rollback verification query")

    if type(verify_rollback_gone) == "userdata" then
        local row = verify_rollback_gone:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch post-rollback result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 0, "Should have 0 rollback test transactions after rollback")
        verify_rollback_gone:close()
    end

    -- Verify original data still exists
    local verify_original = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'pending'")
    TestFramework.assert_not_nil(verify_original, "Failed to execute original data verification query")

    if type(verify_original) == "userdata" then
        local row = verify_original:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch original data result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 2, "Original committed data should still exist after rollback")
        verify_original:close()
    end

    print("  ✓ Transaction rollback functionality completed successfully")

    -- Test 4.3: Autocommit mode behavior
    print("  Testing autocommit mode behavior...")

    -- Re-enable autocommit
    local autocommit_on = conn:autocommit(true)
    TestFramework.assert_true(autocommit_on, "Failed to turn on autocommit")
    print("    ✓ Autocommit re-enabled")

    -- Insert data with autocommit enabled (should commit automatically)
    local autocommit_insert = conn:execute("INSERT INTO test_transactions (name, amount, status) VALUES ('AutoCommit1', 500.00, 'auto')")
    TestFramework.assert_not_nil(autocommit_insert, "Failed to insert autocommit test record")

    -- Verify autocommit data is immediately persistent (start new transaction to test)
    conn:autocommit(false) -- Turn off autocommit to test isolation
    local verify_autocommit = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'auto'")
    TestFramework.assert_not_nil(verify_autocommit, "Failed to execute autocommit verification query")

    if type(verify_autocommit) == "userdata" then
        local row = verify_autocommit:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch autocommit verification result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 1, "Autocommit data should be immediately persistent")
        verify_autocommit:close()
    end

    conn:rollback() -- Clean up test transaction
    conn:autocommit(true) -- Reset to autocommit mode

    print("  ✓ Autocommit mode behavior completed successfully")

    -- Test 4.4: Transaction state consistency
    print("  Testing transaction state consistency...")

    -- Disable autocommit for consistency testing
    conn:autocommit(false)

    -- Test UPDATE operations in transaction
    local update_result = conn:execute("UPDATE test_transactions SET status = 'updated' WHERE name = 'Transaction1'")
    TestFramework.assert_not_nil(update_result, "Failed to execute UPDATE in transaction")

    -- Verify update is visible in current transaction
    local verify_update = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status = 'updated'")
    TestFramework.assert_not_nil(verify_update, "Failed to execute update verification query")

    if type(verify_update) == "userdata" then
        local row = verify_update:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch update verification result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 1, "Should have 1 updated record in transaction")
        verify_update:close()
    end

    -- Test DELETE operations in transaction
    local delete_result = conn:execute("DELETE FROM test_transactions WHERE name = 'Transaction2'")
    TestFramework.assert_not_nil(delete_result, "Failed to execute DELETE in transaction")

    -- Verify remaining record count
    local verify_delete = conn:execute("SELECT COUNT(*) as count FROM test_transactions WHERE status IN ('updated', 'pending')")
    TestFramework.assert_not_nil(verify_delete, "Failed to execute delete verification query")

    if type(verify_delete) == "userdata" then
        local row = verify_delete:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch delete verification result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 1, "Should have 1 record remaining after delete in transaction")
        verify_delete:close()
    end

    -- Commit the changes
    conn:commit()
    print("    ✓ UPDATE and DELETE operations committed")

    -- Verify final state
    local final_count = conn:execute("SELECT COUNT(*) as count FROM test_transactions")
    TestFramework.assert_not_nil(final_count, "Failed to execute final count query")

    if type(final_count) == "userdata" then
        local row = final_count:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch final count result")
        local count = tonumber(row.count) or 0
        TestFramework.assert_true(count >= 2, "Should have at least 2 records total (including autocommit record)")
        final_count:close()
    end

    print("  ✓ Transaction state consistency completed successfully")

    -- Restore autocommit mode for other tests
    conn:autocommit(true)

    print("✓ Advanced transaction operations completed successfully")
end)

-- Test 5: Transaction Rollback and Exception Handling
suite:test("transaction_rollback_exception_handling", function()
    local conn = TestConfig.get_connection()

    -- Create test table for exception handling tests
    conn:execute("DROP TABLE IF EXISTS test_exceptions")
    local create_sql = [[
        CREATE TABLE test_exceptions (
            id INT AUTO_INCREMENT PRIMARY KEY,
            name VARCHAR(100) UNIQUE,
            value INT,
            category VARCHAR(50),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ]]
    local result = conn:execute(create_sql)
    TestFramework.assert_not_nil(result, "Failed to create test_exceptions table")

    -- Test 5.1: Basic exception handling during transactions
    print("  Testing basic exception handling during transactions...")

    conn:autocommit(false)

    -- Insert valid data first
    local insert1 = conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('valid1', 100, 'normal')")
    TestFramework.assert_not_nil(insert1, "Failed to insert valid data")

    -- Try to insert duplicate unique key (should cause exception)
    local duplicate_success, duplicate_error = pcall(function()
        return conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('valid1', 200, 'duplicate')")
    end)

    -- Check if the operation failed due to duplicate key constraint
    local duplicate_failed = not duplicate_success or duplicate_error == nil
    TestFramework.assert_true(duplicate_failed, "Duplicate key insertion should fail")
    print("    ✓ Duplicate key exception caught: " .. tostring(duplicate_error))

    -- Verify transaction state after exception
    local count_after_error = conn:execute("SELECT COUNT(*) as count FROM test_exceptions")
    if type(count_after_error) == "userdata" then
        local row = count_after_error:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch count after error")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 1, "Should have 1 record after exception (transaction should continue)")
        count_after_error:close()
    end

    -- Test that we can continue the transaction after exception
    local insert2 = conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('valid2', 300, 'normal')")
    TestFramework.assert_not_nil(insert2, "Should be able to continue transaction after exception")

    conn:commit()
    print("    ✓ Transaction continued and committed after exception")

    -- Test 5.2: Transaction rollback after multiple operations
    print("  Testing transaction rollback after multiple operations...")

    conn:autocommit(false)

    -- Insert test data for rollback testing
    local insert3 = conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('rollback1', 400, 'test')")
    TestFramework.assert_not_nil(insert3, "Failed to insert rollback test data 1")

    local insert4 = conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('rollback2', 500, 'test')")
    TestFramework.assert_not_nil(insert4, "Failed to insert rollback test data 2")

    -- Update existing data
    local update1 = conn:execute("UPDATE test_exceptions SET value = 999 WHERE name = 'valid1'")
    TestFramework.assert_not_nil(update1, "Failed to update existing data")

    -- Verify changes are visible in current transaction
    local verify_changes = conn:execute("SELECT COUNT(*) as count FROM test_exceptions WHERE category = 'test' OR value = 999")
    if type(verify_changes) == "userdata" then
        local row = verify_changes:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch changes verification")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 3, "Should see all changes in current transaction")
        verify_changes:close()
    end

    -- Rollback the transaction
    local rollback_result = conn:rollback()
    TestFramework.assert_true(rollback_result, "Failed to rollback transaction")

    -- Verify all changes were rolled back
    local verify_rollback = conn:execute("SELECT COUNT(*) as count FROM test_exceptions WHERE category = 'test' OR value = 999")
    if type(verify_rollback) == "userdata" then
        local row = verify_rollback:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch rollback verification")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 0, "All changes should be rolled back")
        verify_rollback:close()
    end

    print("    ✓ Multiple operations rolled back successfully")

    -- Test 5.3: Exception propagation and transaction state
    print("  Testing exception propagation and transaction state...")

    conn:autocommit(false)

    -- Function to simulate complex operation with potential failure
    local function complex_operation_with_failure()
        -- Insert some data
        conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('complex1', 600, 'complex')")

        -- Simulate a business logic error (not database error)
        error("Simulated business logic failure")

        -- This should not execute
        conn:execute("INSERT INTO test_exceptions (name, value, category) VALUES ('complex2', 700, 'complex')")
    end

    local complex_success, complex_error = pcall(complex_operation_with_failure)
    TestFramework.assert_false(complex_success, "Complex operation should fail")
    TestFramework.assert_true(string.find(complex_error, "business logic failure") ~= nil, "Should propagate business logic error")

    -- Check if partial data from failed operation is in transaction
    local verify_partial = conn:execute("SELECT COUNT(*) as count FROM test_exceptions WHERE category = 'complex'")
    if type(verify_partial) == "userdata" then
        local row = verify_partial:fetch()
        TestFramework.assert_not_nil(row, "Failed to fetch partial data verification")
        local count = tonumber(row.count) or 0
        TestFramework.assert_equal(count, 1, "Should have partial data from failed operation")
        verify_partial:close()
    end

    -- Rollback to clean up partial state
    conn:rollback()
    print("    ✓ Exception propagation and cleanup handled correctly")

    -- Restore autocommit mode for other tests
    conn:autocommit(true)

    print("✓ Transaction rollback and exception handling tests completed successfully")
end)

--[[
=============================================================================
TEST EXECUTION
=============================================================================
]]

-- Run the Phase 2 test suite
local failures = TestFramework.run_suite(suite)

if failures > 0 then
    print("MariaDB Phase 2 - Transaction Processing tests failed!")
    os.exit(1)
end

print("✅ All MariaDB Phase 2 - Transaction Processing tests completed successfully!")