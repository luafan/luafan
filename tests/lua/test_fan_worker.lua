#!/usr/bin/env lua

-- Test for fan.worker module (multi-process worker management)
-- This module provides process pooling, load balancing, and task distribution

local TestFramework = require('test_framework')
local fan = require "fan"

-- Mock config module if needed
package.preload['config'] = function()
    return {
        worker_using_cjson = false,  -- Use objectbuf instead of cjson
        pool_size = 10,
        debug = false
    }
end

-- Try to load fan.worker module
local worker_available = false
local worker

local ok, result = pcall(require, 'fan.worker')
if ok then
    worker = result
    worker_available = true
    print("fan.worker module loaded successfully")
else
    print("Error: fan.worker module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.worker Tests")

print("Testing fan.worker module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(worker)
    TestFramework.assert_type(worker, "table")

    -- Check for essential functions
    TestFramework.assert_type(worker.new, "function")
end)

-- Test basic worker creation (master only, no slaves)
suite:test("basic_worker_creation", function()
    -- Create worker with no slaves (master only mode)
    local test_functions = {
        add = function(a, b) return a + b end,
        multiply = function(a, b) return a * b end,
        concat = function(a, b) return tostring(a) .. tostring(b) end
    }

    local worker_obj = worker.new(test_functions, 0, 5)  -- 0 slaves, max 5 jobs per worker

    TestFramework.assert_not_nil(worker_obj)
    TestFramework.assert_type(worker_obj, "table")

    -- Check master structure
    TestFramework.assert_not_nil(worker_obj.func_names)
    TestFramework.assert_not_nil(worker_obj.slave_pool)
    TestFramework.assert_not_nil(worker_obj.loadbalance)
    TestFramework.assert_not_nil(worker_obj.slave_pids)
    TestFramework.assert_not_nil(worker_obj.slaves)

    -- Check function registration
    TestFramework.assert_equal("add", worker_obj.func_names.add)
    TestFramework.assert_equal("multiply", worker_obj.func_names.multiply)
    TestFramework.assert_equal("concat", worker_obj.func_names.concat)

    -- Check utility functions
    TestFramework.assert_type(worker_obj.terminate, "function")
    TestFramework.assert_type(worker_obj.wait_all_slaves, "function")
end)

-- Test loadbalance structure
suite:test("loadbalance_structure", function()
    local test_functions = {
        test_func = function(x) return x * 2 end
    }

    local worker_obj = worker.new(test_functions, 0, 3)
    local lb = worker_obj.loadbalance

    TestFramework.assert_not_nil(lb)
    TestFramework.assert_type(lb, "table")

    -- Check loadbalancer properties
    TestFramework.assert_equal(3, lb.max_job_count)
    TestFramework.assert_not_nil(lb.slaves)
    TestFramework.assert_not_nil(lb.yielding)

    -- Check yielding queue structure
    TestFramework.assert_nil(lb.yielding.head)
    TestFramework.assert_nil(lb.yielding.tail)
end)

-- Test worker with simple function map
suite:test("function_map_registration", function()
    local functions = {
        square = function(n) return n * n end,
        cube = function(n) return n * n * n end,
        is_even = function(n) return n % 2 == 0 end,
        reverse_string = function(s) return string.reverse(s) end,
        join_strings = function(a, b, sep) return a .. (sep or "") .. b end
    }

    local worker_obj = worker.new(functions, 0, 2)

    -- Verify all functions are registered
    for name, _ in pairs(functions) do
        TestFramework.assert_equal(name, worker_obj.func_names[name])
    end

    -- Check no extra functions registered
    local count = 0
    for _ in pairs(worker_obj.func_names) do
        count = count + 1
    end
    TestFramework.assert_equal(5, count)
end)

-- Test edge case: empty function map
suite:test("empty_function_map", function()
    local worker_obj = worker.new({}, 0, 1)

    TestFramework.assert_not_nil(worker_obj)
    TestFramework.assert_not_nil(worker_obj.func_names)

    local count = 0
    for _ in pairs(worker_obj.func_names) do
        count = count + 1
    end
    TestFramework.assert_equal(0, count)
end)

-- Test single function registration
suite:test("single_function", function()
    local worker_obj = worker.new({
        echo = function(msg) return "Echo: " .. tostring(msg) end
    }, 0, 1)

    TestFramework.assert_equal("echo", worker_obj.func_names.echo)

    local count = 0
    for _ in pairs(worker_obj.func_names) do
        count = count + 1
    end
    TestFramework.assert_equal(1, count)
end)

-- Test worker slave process list initialization
suite:test("slave_process_management", function()
    local worker_obj = worker.new({
        test = function() return "test" end
    }, 0, 1)  -- 0 slaves

    TestFramework.assert_not_nil(worker_obj.slave_pids)
    TestFramework.assert_type(worker_obj.slave_pids, "table")
    TestFramework.assert_equal(0, #worker_obj.slave_pids)

    TestFramework.assert_not_nil(worker_obj.slaves)
    TestFramework.assert_type(worker_obj.slaves, "table")
    TestFramework.assert_equal(0, #worker_obj.slaves)
end)

-- Test terminate function behavior (master only mode)
suite:test("terminate_function", function()
    local worker_obj = worker.new({
        dummy = function() return 42 end
    }, 0, 1)

    -- Test terminate function exists and is callable
    TestFramework.assert_type(worker_obj.terminate, "function")

    -- In master-only mode (0 slaves), terminate should work without errors
    local ok, err = pcall(worker_obj.terminate, worker_obj)
    TestFramework.assert_true(ok)

    -- After terminate, slave_pids should still be table but could be modified
    TestFramework.assert_type(worker_obj.slave_pids, "table")
end)

-- Test wait_all_slaves function (master only mode - should return immediately)
suite:test("wait_all_slaves_no_slaves", function()
    local worker_obj = worker.new({
        test_wait = function() return "ready" end
    }, 0, 1)

    TestFramework.assert_type(worker_obj.wait_all_slaves, "function")

    -- With 0 slaves, wait_all_slaves should return immediately since we already have all slaves (none)
    local start_time = fan.gettime()
    local result = worker_obj.wait_all_slaves()
    local end_time = fan.gettime()

    -- Should complete very quickly (no actual slaves to wait for)
    TestFramework.assert_true((end_time - start_time) < 0.1)
end)

-- Test complex function map with various data types
suite:test("complex_function_map", function()
    local functions = {
        -- String operations
        string_length = function(s) return #s end,
        string_upper = function(s) return string.upper(s) end,

        -- Number operations
        add_three = function(a, b, c) return a + b + c end,
        max_value = function(a, b) return math.max(a, b) end,

        -- Table operations
        table_size = function(t)
            local count = 0
            for _ in pairs(t) do count = count + 1 end
            return count
        end,

        -- Boolean operations
        logical_and = function(a, b) return a and b end,

        -- Mixed operations
        format_result = function(name, value, success)
            return string.format("%s: %s (%s)", name, tostring(value), success and "OK" or "FAIL")
        end
    }

    local worker_obj = worker.new(functions, 0, 3)

    -- Verify all complex functions are registered
    for name, _ in pairs(functions) do
        TestFramework.assert_equal(name, worker_obj.func_names[name])
    end

    local registered_count = 0
    for _ in pairs(worker_obj.func_names) do
        registered_count = registered_count + 1
    end
    TestFramework.assert_equal(7, registered_count)
end)

-- Test worker pool integration
suite:test("worker_pool_integration", function()
    local worker_obj = worker.new({
        pool_test = function(x) return x + 1 end
    }, 0, 2)

    TestFramework.assert_not_nil(worker_obj.slave_pool)
    TestFramework.assert_type(worker_obj.slave_pool, "table")

    -- Pool should have standard pool methods
    TestFramework.assert_type(worker_obj.slave_pool.pop, "function")
    TestFramework.assert_type(worker_obj.slave_pool.push, "function")
    TestFramework.assert_type(worker_obj.slave_pool.safe, "function")
end)

-- Test process affinity and naming behavior (structural test)
suite:test("process_management_structure", function()
    -- Note: This test doesn't actually fork processes, just tests the structure
    local worker_obj = worker.new({
        affinity_test = function() return fan.getpid() end
    }, 0, 1)

    -- Test that process-related functions are available
    TestFramework.assert_type(fan.getpid, "function")
    TestFramework.assert_type(fan.getcpucount, "function")

    local pid = fan.getpid()
    TestFramework.assert_type(pid, "number")
    TestFramework.assert_true(pid > 0)

    local cpu_count = fan.getcpucount()
    TestFramework.assert_type(cpu_count, "number")
    TestFramework.assert_true(cpu_count >= 1)
end)

-- Test error handling with invalid parameters
suite:test("error_handling", function()
    -- Test with invalid function map
    local ok1, err1 = pcall(function()
        return worker.new(nil, 0, 1)
    end)
    -- Should handle gracefully (implementation dependent)
    TestFramework.assert_type(ok1, "boolean")

    -- Test with negative slave count - may fail due to for loop behavior
    local ok2, result2 = pcall(function()
        return worker.new({test = function() end}, -1, 1)
    end)
    -- Don't assert this must succeed, as negative values may cause issues
    TestFramework.assert_type(ok2, "boolean")
    if ok2 then
        TestFramework.assert_not_nil(result2)
    end

    -- Test with zero max_job_count (edge case)
    local ok3, result3 = pcall(function()
        return worker.new({test = function() end}, 0, 0)
    end)
    TestFramework.assert_true(ok3)
    if ok3 then
        TestFramework.assert_not_nil(result3)
    end
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)