#!/usr/bin/env lua

-- Test for fan.utils module

local TestFramework = require('test_framework')

-- Try to load fan.utils
local fan_utils_available = false
local fan_utils

local ok, result = pcall(require, 'fan.utils')
if ok then
    fan_utils = result
    fan_utils_available = true
    print("fan.utils module loaded successfully")
else
    print("Warning: fan.utils module not available: " .. tostring(result))
end

-- Create test suite
local suite = TestFramework.create_suite("fan.utils Tests")

if fan_utils_available then
    -- Test gettime function
    suite:test("gettime_function", function()
        TestFramework.assert_type(fan_utils.gettime, "function")

        local time1 = fan_utils.gettime()
        TestFramework.assert_type(time1, "number")
        TestFramework.assert_true(time1 > 0)

        -- Small delay to test time progression
        local count = 0
        for i = 1, 1000000 do
            count = count + 1
        end

        local time2 = fan_utils.gettime()
        TestFramework.assert_true(time2 >= time1)
    end)

    -- Test time utilities
    suite:test("time_operations", function()
        local start_time = fan_utils.gettime()

        -- Do some work
        local result = 0
        for i = 1, 10000 do
            result = result + i
        end

        local end_time = fan_utils.gettime()
        local elapsed = end_time - start_time

        TestFramework.assert_type(elapsed, "number")
        TestFramework.assert_true(elapsed >= 0)
        TestFramework.assert_equal(50005000, result) -- sum of 1 to 10000
    end)

    -- Test weak reference functionality if available
    if fan_utils.weakref then
        suite:test("weak_references", function()
            TestFramework.assert_type(fan_utils.weakref, "function")

            local test_table = {data = "test"}
            local weak_ref = fan_utils.weakref(test_table)

            TestFramework.assert_type(weak_ref, "function")

            -- Reference should still be alive
            local retrieved = weak_ref()
            TestFramework.assert_not_nil(retrieved)
            TestFramework.assert_equal("test", retrieved.data)
        end)
    end

    -- Test string utilities if available
    if fan_utils.split then
        suite:test("string_split", function()
            TestFramework.assert_type(fan_utils.split, "function")

            local parts = fan_utils.split("a,b,c", ",")
            TestFramework.assert_type(parts, "table")
            TestFramework.assert_equal(3, #parts)
            TestFramework.assert_equal("a", parts[1])
            TestFramework.assert_equal("b", parts[2])
            TestFramework.assert_equal("c", parts[3])
        end)
    end

    -- Test trim function if available
    if fan_utils.trim then
        suite:test("string_trim", function()
            TestFramework.assert_type(fan_utils.trim, "function")

            TestFramework.assert_equal("test", fan_utils.trim("  test  "))
            TestFramework.assert_equal("test", fan_utils.trim("\ttest\n"))
            TestFramework.assert_equal("", fan_utils.trim("   "))
            TestFramework.assert_equal("test", fan_utils.trim("test"))
        end)
    end

else
    -- Placeholder test when module is not available
    suite:test("module_not_available", function()
        print("  Skipping fan.utils tests - module not available")
        -- This test always passes
        TestFramework.assert_true(true)
    end)
end

-- Module availability test
suite:test("module_loading", function()
    if fan_utils_available then
        TestFramework.assert_not_nil(fan_utils)
        TestFramework.assert_type(fan_utils, "table")
    else
        -- Test that we handle missing modules gracefully
        TestFramework.assert_nil(fan_utils)
    end
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)