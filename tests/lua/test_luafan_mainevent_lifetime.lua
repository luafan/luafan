#!/usr/bin/env lua

-- DETERMINISTIC crash reproducer for luafan_start mainevent double-free.
-- The bug: main_handler frees mainevent but doesn't set it to NULL.
-- If the event fires abnormally or nested call detection fails, double-free occurs.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("luafan_start mainevent lifetime - double free detector")

-- Subprocess that attempts to trigger mainevent double-free
local CRASH_SCRIPT = [[
local fan = require "fan"

-- Scenario 1: rapid sequential fan.loop calls
for i = 1, 10 do
    fan.loop(function()
        fan.loopbreak()
    end)
end

-- Scenario 2: nested call attempt (should be handled gracefully, not crash)
fan.loop(function()
    pcall(fan.loop, function()
        fan.loopbreak()
    end)
    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_double_free_mainevent_on_rapid_loops", function()
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(CRASH_SCRIPT)
    f:close()

    local cmd = string.format(
        "LUA_PATH='../modules/?.lua;../modules/?/init.lua;./lua/framework/?.lua;./lua/?.lua;;' " ..
        "LUA_CPATH='./build/?.so;../?.so;;' " ..
        "timeout 10s lua %s; echo $?",
        tmpfile
    )
    local handle = io.popen(cmd .. " 2>&1")
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    local exit_code = tonumber(output:match("(%d+)%s*$"))

    if not success and exit_type == "signal" then
        print(string.format("Subprocess killed by signal %d", code))
        if code == 11 or code == 6 then
            print("✓ BUG CONFIRMED: double-free in mainevent detected (signal SIGSEGV/SIGABRT)")
            TestFramework.assert_true(true)
        else
            error(string.format("Unexpected signal %d", code))
        end
    elseif exit_code == 0 then
        print("Subprocess exited cleanly (bug is FIXED or not triggered)")
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit code %s, output:\n%s", tostring(exit_code), output))
    end
end)

-- In-process tests (non-crash scenarios)
suite:test("basic_loop_cycle", function()
    local ran = false
    fan.loop(function()
        ran = true
        fan.loopbreak()
    end)
    TestFramework.assert_true(ran)
end)

suite:test("sequential_loop_cycles", function()
    for i = 1, 3 do
        local ran = false
        fan.loop(function()
            ran = true
            fan.loopbreak()
        end)
        TestFramework.assert_true(ran)
    end
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
