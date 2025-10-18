#!/usr/bin/env lua

-- Test for fan.utils module (modules/fan/utils.lua)

local TestFramework = require('test_framework')
local fan = require "fan"
local utils = require "fan.utils"

-- Create test suite
local suite = TestFramework.create_suite("fan.utils Tests")

print("Testing fan.utils module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(utils)
    TestFramework.assert_type(utils, "table")

    -- Check for essential functions
    TestFramework.assert_type(utils.random_string, "function")
    TestFramework.assert_type(utils.gettime, "function")
    TestFramework.assert_type(utils.split, "function")
    TestFramework.assert_type(utils.weakify, "function")
    TestFramework.assert_type(utils.weakify_object, "function")

    -- Check constants
    TestFramework.assert_type(utils.LETTERS_W, "string")
    TestFramework.assert_true(#utils.LETTERS_W > 0)
end)

-- Test random_string function
suite:test("random_string_basic", function()
    local letters = "abc"
    local result = utils.random_string(letters, 5)

    TestFramework.assert_type(result, "string")
    TestFramework.assert_equal(#result, 5)

    -- Check that all characters are from the letters set
    for i = 1, #result do
        local char = result:sub(i, i)
        TestFramework.assert_true(letters:find(char, 1, true) ~= nil)
    end
end)

-- Test random_string with joiner
suite:test("random_string_with_joiner", function()
    local letters = "abc"
    local result = utils.random_string(letters, 6, "-", 2)

    TestFramework.assert_type(result, "string")
    -- Should be 2-char groups joined by "-": "ab-cd-ef"
    local parts = utils.split(result, "-")
    TestFramework.assert_equal(#parts, 3)

    for _, part in ipairs(parts) do
        TestFramework.assert_equal(#part, 2)
    end
end)

-- Test random_string with default letters
suite:test("random_string_default_letters", function()
    local result = utils.random_string(utils.LETTERS_W, 10)

    TestFramework.assert_type(result, "string")
    TestFramework.assert_equal(#result, 10)

    -- Check that all characters are alphanumeric
    for i = 1, #result do
        local char = result:sub(i, i)
        TestFramework.assert_true(utils.LETTERS_W:find(char, 1, true) ~= nil)
    end
end)

-- Test gettime function
suite:test("gettime_basic", function()
    local time1 = utils.gettime()
    TestFramework.assert_type(time1, "number")
    TestFramework.assert_true(time1 > 0)

    -- Sleep a bit using fan.sleep for precision
    fan.sleep(0.001)

    local time2 = utils.gettime()
    TestFramework.assert_type(time2, "number")
    TestFramework.assert_true(time2 > time1)
end)

-- Test gettime precision
suite:test("gettime_precision", function()
    local times = {}
    for i = 1, 3 do
        times[i] = utils.gettime()
        fan.sleep(0.001)  -- 1ms sleep
    end

    -- Check that times are increasing
    TestFramework.assert_true(times[2] > times[1])
    TestFramework.assert_true(times[3] > times[2])

    -- Check precision (should have sub-second resolution)
    local diff = times[3] - times[1]
    TestFramework.assert_true(diff > 0.001)  -- At least 2ms difference
    TestFramework.assert_true(diff < 1.0)    -- But less than 1 second
end)

-- Test split function
suite:test("split_basic", function()
    local result = utils.split("a,b,c", ",")

    TestFramework.assert_type(result, "table")
    TestFramework.assert_equal(#result, 3)
    TestFramework.assert_equal(result[1], "a")
    TestFramework.assert_equal(result[2], "b")
    TestFramework.assert_equal(result[3], "c")
end)

-- Test split with different patterns
suite:test("split_patterns", function()
    -- Test with space
    local result1 = utils.split("hello world test", " ")
    TestFramework.assert_equal(#result1, 3)
    TestFramework.assert_equal(result1[1], "hello")
    TestFramework.assert_equal(result1[2], "world")
    TestFramework.assert_equal(result1[3], "test")

    -- Test with multi-character pattern
    local result2 = utils.split("a::b::c", "::")
    TestFramework.assert_equal(#result2, 3)
    TestFramework.assert_equal(result2[1], "a")
    TestFramework.assert_equal(result2[2], "b")
    TestFramework.assert_equal(result2[3], "c")
end)

-- Test split edge cases
suite:test("split_edge_cases", function()
    -- Empty string
    local result1 = utils.split("", ",")
    TestFramework.assert_type(result1, "table")
    TestFramework.assert_equal(#result1, 0)

    -- No separator found
    local result2 = utils.split("hello", ",")
    TestFramework.assert_equal(#result2, 1)
    TestFramework.assert_equal(result2[1], "hello")

    -- Nil string
    local result3 = utils.split(nil, ",")
    TestFramework.assert_type(result3, "table")
    TestFramework.assert_equal(#result3, 0)
end)

-- Test weakify_object function
suite:test("weakify_object_basic", function()
    local original = {x = 1, y = 2}
    local weak_obj = utils.weakify_object(original)

    TestFramework.assert_not_nil(weak_obj)
    TestFramework.assert_type(weak_obj, "table")

    -- Should be able to access original properties
    TestFramework.assert_equal(weak_obj.x, 1)
    TestFramework.assert_equal(weak_obj.y, 2)

    -- Should be able to modify original through weak reference
    weak_obj.z = 3
    TestFramework.assert_equal(original.z, 3)
end)

-- Test weakify function
suite:test("weakify_multiple", function()
    local obj1 = {a = 1}
    local obj2 = {b = 2}
    local obj3 = {c = 3}

    local weak1, weak2, weak3 = utils.weakify(obj1, obj2, obj3)

    TestFramework.assert_not_nil(weak1)
    TestFramework.assert_not_nil(weak2)
    TestFramework.assert_not_nil(weak3)

    TestFramework.assert_equal(weak1.a, 1)
    TestFramework.assert_equal(weak2.b, 2)
    TestFramework.assert_equal(weak3.c, 3)
end)

-- Test weakify single object
suite:test("weakify_single", function()
    local original = {value = "test"}
    local weak_obj = utils.weakify(original)

    TestFramework.assert_not_nil(weak_obj)
    TestFramework.assert_equal(weak_obj.value, "test")

    -- Modify through weak reference
    weak_obj.value = "modified"
    TestFramework.assert_equal(original.value, "modified")
end)

-- Test FFI optimization (if available)
suite:test("ffi_optimization_check", function()
    -- Check if running under LuaJIT
    if _G.jit then
        print("Running under LuaJIT - FFI optimizations should be available")

        -- Test that gettime still works (should use FFI version)
        local time1 = utils.gettime()
        TestFramework.assert_type(time1, "number")
        TestFramework.assert_true(time1 > 0)
    else
        print("Running under standard Lua - using pure Lua implementations")

        -- Test fallback version
        local time1 = utils.gettime()
        TestFramework.assert_type(time1, "number")
        TestFramework.assert_true(time1 > 0)
    end
end)

-- Test performance comparison (async timing)
suite:test("async_timing_performance", function()
    local start_time = utils.gettime()

    -- Perform some async operations
    fan.sleep(0.01)  -- 10ms sleep

    local end_time = utils.gettime()
    local duration = end_time - start_time

    -- Should be approximately 10ms (allow some tolerance)
    TestFramework.assert_true(duration >= 0.008)  -- At least 8ms
    TestFramework.assert_true(duration <= 0.020)  -- At most 20ms
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)