#!/usr/bin/env lua

-- Example Lua test demonstrating the test framework

local TestFramework = require('test_framework')

-- Create test suite
local suite = TestFramework.create_suite("Example Tests")

-- Basic assertion tests
suite:test("basic_assertions", function()
    TestFramework.assert_equal(42, 42)
    TestFramework.assert_true(true)
    TestFramework.assert_false(false)
    TestFramework.assert_not_nil("test")
    TestFramework.assert_nil(nil)
end)

-- String tests
suite:test("string_operations", function()
    local test_str = "Hello, World!"
    TestFramework.assert_equal(test_str, "Hello, World!")
    TestFramework.assert_type(test_str, "string")
    TestFramework.assert_match(test_str, "World")
    TestFramework.assert_equal(13, string.len(test_str))
end)

-- Table tests
suite:test("table_operations", function()
    local test_table = {a = 1, b = 2, c = 3}
    TestFramework.assert_type(test_table, "table")
    TestFramework.assert_equal(1, test_table.a)
    TestFramework.assert_equal(2, test_table.b)
    TestFramework.assert_equal(3, test_table.c)
    TestFramework.assert_nil(test_table.d)
end)

-- Number operations
suite:test("number_operations", function()
    TestFramework.assert_equal(4, 2 + 2)
    TestFramework.assert_equal(8, 2 * 4)
    TestFramework.assert_equal(2, 10 / 5)
    TestFramework.assert_true(10 > 5)
    TestFramework.assert_false(5 > 10)
end)

-- Error handling test
suite:test("error_handling", function()
    TestFramework.assert_error(function()
        error("Test error")
    end, "Test error")

    -- Test that function doesn't throw error
    local function safe_function()
        return "safe"
    end
    TestFramework.assert_equal("safe", safe_function())
end)

-- Function tests
suite:test("function_operations", function()
    local function add(a, b)
        return a + b
    end

    TestFramework.assert_type(add, "function")
    TestFramework.assert_equal(5, add(2, 3))
    TestFramework.assert_equal(0, add(-5, 5))
end)

-- Setup and teardown demonstration
local setup_called = false
local teardown_called = false

suite:set_setup(function()
    print("  Setup: Initializing test environment")
    setup_called = true
end)

suite:set_teardown(function()
    print("  Teardown: Cleaning up test environment")
    teardown_called = true
end)

suite:test("setup_teardown_verification", function()
    TestFramework.assert_true(setup_called)
    -- teardown_called will be false here since teardown runs after all tests
end)

-- Before/after each test hooks
local before_each_count = 0
local after_each_count = 0

suite:set_before_each(function()
    before_each_count = before_each_count + 1
end)

suite:set_after_each(function()
    after_each_count = after_each_count + 1
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Verify setup/teardown were called
assert(setup_called, "Setup should have been called")

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)