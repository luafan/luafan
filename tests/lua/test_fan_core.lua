#!/usr/bin/env lua

-- Test for fan core module (C extension)

local TestFramework = require('test_framework')

-- Try to load fan core module
local fan_available = false
local fan

local ok, result = pcall(require, 'fan')
if ok then
    fan = result
    fan_available = true
    print("fan core module loaded successfully")
else
    print("Error: fan core module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan core Tests")

-- Test module loading and basic structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(fan)
    TestFramework.assert_type(fan, "table")

    -- Check for essential functions
    TestFramework.assert_type(fan.gettime, "function")
    TestFramework.assert_type(fan.sleep, "function")
    TestFramework.assert_type(fan.loop, "function")
    TestFramework.assert_type(fan.loopbreak, "function")
    TestFramework.assert_type(fan.getpid, "function")
end)

-- Test time functions
suite:test("time_functions", function()
    -- Test gettime - returns multiple values (sec, usec)
    local sec, usec = fan.gettime()
    TestFramework.assert_type(sec, "number")
    TestFramework.assert_type(usec, "number")
    TestFramework.assert_true(sec > 0)
    TestFramework.assert_true(usec >= 0)
    TestFramework.assert_true(usec < 1000000)

    -- Test time progression
    local sec2, usec2 = fan.gettime()
    TestFramework.assert_true(sec2 >= sec)
    if sec2 == sec then
        TestFramework.assert_true(usec2 >= usec)
    end
end)

-- Test process ID functions
suite:test("process_functions", function()
    -- Test getpid
    local pid = fan.getpid()
    TestFramework.assert_type(pid, "number")
    TestFramework.assert_true(pid > 0)

    -- Test getcpucount
    local cpu_count = fan.getcpucount()
    TestFramework.assert_type(cpu_count, "number")
    TestFramework.assert_true(cpu_count > 0)
    TestFramework.assert_true(cpu_count <= 256) -- reasonable upper bound

    -- Test getdtablesize
    local dtable_size = fan.getdtablesize()
    TestFramework.assert_type(dtable_size, "number")
    TestFramework.assert_true(dtable_size > 0)
end)

-- Test hex conversion utilities
suite:test("hex_conversion", function()
    -- Test data2hex
    local test_data = "Hello World"
    local hex_result = fan.data2hex(test_data)
    TestFramework.assert_type(hex_result, "string")
    TestFramework.assert_true(#hex_result > 0)
    TestFramework.assert_true(#hex_result == #test_data * 2) -- each byte -> 2 hex chars

    -- Test hex2data (reverse conversion)
    local data_result = fan.hex2data(hex_result)
    TestFramework.assert_type(data_result, "string")
    TestFramework.assert_equal(test_data, data_result)

    -- Test with empty string
    TestFramework.assert_equal("", fan.data2hex(""))
    TestFramework.assert_equal("", fan.hex2data(""))

    -- Test known conversions (note: uppercase hex)
    TestFramework.assert_equal("48656C6C6F", fan.data2hex("Hello"))
    TestFramework.assert_equal("Hello", fan.hex2data("48656C6C6F"))
end)

-- Test network interface functions
suite:test("network_interfaces", function()
    -- Test getinterfaces
    local interfaces = fan.getinterfaces()
    TestFramework.assert_type(interfaces, "table")

    -- Should have at least loopback interface
    TestFramework.assert_true(#interfaces > 0)

    -- Check structure of first interface
    if #interfaces > 0 then
        local iface = interfaces[1]
        TestFramework.assert_type(iface, "table")
        TestFramework.assert_type(iface.name, "string")
        TestFramework.assert_true(#iface.name > 0)
        -- addr field may not always be present, so just check if it exists
        if iface.addr then
            TestFramework.assert_type(iface.addr, "string")
        end
    end
end)

-- Test file descriptor functions
suite:test("file_descriptor_functions", function()
    -- Test gettop (Lua stack top)
    local top = fan.gettop()
    TestFramework.assert_type(top, "number")
    TestFramework.assert_true(top >= 0)
end)

-- Test sleep function (already in event loop from main runner)
suite:test("sleep_function", function()
    TestFramework.assert_type(fan.sleep, "function")

    -- Test sleep - we're already in event loop from main runner
    local start_time = {fan.gettime()}

    fan.sleep(0.01) -- 10ms sleep

    local end_time = {fan.gettime()}

    -- Verify timing
    TestFramework.assert_not_nil(start_time)
    TestFramework.assert_not_nil(end_time)

    -- Calculate elapsed time
    local elapsed = (end_time[1] + end_time[2] / 1000000) -
                   (start_time[1] + start_time[2] / 1000000)

    -- Should have slept at least the requested time (with some tolerance)
    TestFramework.assert_true(elapsed >= 0.005) -- allow 5ms tolerance
end)

-- Test program name functions
suite:test("program_name", function()
    -- Test setprogname (should not crash)
    TestFramework.assert_type(fan.setprogname, "function")

    -- This should work without error
    local ok = pcall(fan.setprogname, "luafan_test")
    TestFramework.assert_true(ok)
end)

-- Test event loop functions (basic structure test only)
suite:test("event_loop_structure", function()
    TestFramework.assert_type(fan.loop, "function")
    TestFramework.assert_type(fan.loopbreak, "function")

    -- Note: We don't actually start the event loop in tests
    -- as it would block the test execution
end)

-- Test process group functions
suite:test("process_group_functions", function()
    local pid = fan.getpid()

    -- Test getpgid
    local pgid = fan.getpgid(pid)
    TestFramework.assert_type(pgid, "number")
    TestFramework.assert_true(pgid > 0)

    -- Test with current process (0)
    local current_pgid = fan.getpgid(0)
    TestFramework.assert_type(current_pgid, "number")
    TestFramework.assert_equal(pgid, current_pgid)
end)

-- Test error handling
suite:test("error_handling", function()
    -- fan.hex2data is quite permissive and doesn't error on invalid input
    -- it just returns what it can decode

    -- Test invalid hex data - should succeed but return partial/garbled data
    local result1 = fan.hex2data("ABC") -- odd length - truncates to "AB"
    TestFramework.assert_type(result1, "string")

    local result2 = fan.hex2data("XYZ") -- invalid hex chars
    TestFramework.assert_type(result2, "string")

    -- Test nil/empty inputs work correctly
    TestFramework.assert_equal("", fan.hex2data(""))
    TestFramework.assert_equal("", fan.data2hex(""))
end)

-- Test CPU affinity functions (if available)
suite:test("cpu_affinity", function()
    TestFramework.assert_type(fan.getaffinity, "function")
    TestFramework.assert_type(fan.setaffinity, "function")

    -- Test getaffinity for current process - returns bitmask as number
    local affinity = fan.getaffinity()
    TestFramework.assert_type(affinity, "number")
    TestFramework.assert_true(affinity > 0)

    -- Test setaffinity with same value (should work)
    local ok = pcall(fan.setaffinity, affinity)
    TestFramework.assert_true(ok)
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)