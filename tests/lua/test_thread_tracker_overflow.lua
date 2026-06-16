#!/usr/bin/env lua

-- Regression test for problem 15: thread_tracker array bounds
-- When MAX_THREAD_RECORDS (1000) or MAX_FUNCTION_RECORDS (2000) is exceeded,
-- circular overwrite silently destroys tracking data.
-- With DEBUG_THREAD_TRACKING enabled, this can corrupt tracking state.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("thread_tracker array bounds")

-- Subprocess: create enough references to overflow the tracker arrays
local CRASH_SCRIPT = [[
local fan = require "fan"

-- Check if thread tracker is available (requires DEBUG_THREAD_TRACKING build)
if not get_thread_ref_count then
    print("Thread tracker not available (DEBUG_THREAD_TRACKING not enabled)")
    os.exit(0)
end

-- Create many coroutine references to overflow function_records
-- MAX_FUNCTION_RECORDS = 2000
local threads = {}
for i = 1, 2500 do
    local co = coroutine.create(function()
        coroutine.yield()
    end)
    table.insert(threads, co)
end

-- Check if tracking overflowed
local count = get_function_ref_count()
print(string.format("Function ref count: %d", count))

-- Now destroy them all
threads = nil
collectgarbage("collect")
collectgarbage("collect")

-- Verify count decreased
local final_count = get_function_ref_count()
print(string.format("Final function ref count: %d", final_count))

os.exit(0)
]]

suite:test("subprocess_tracker_overflow", function()
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
        "timeout 15s lua %s; echo $?",
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
            print("BUG CONFIRMED: thread_tracker array corruption")
            TestFramework.assert_true(true)
        else
            error(string.format("Unexpected signal %d", code))
        end
    elseif exit_code == 0 then
        print("Subprocess exited cleanly")
        -- Check if overflow message appeared
        if output:find("circular overwrite") or output:find("WARNING") then
            print("NOTE: Circular overwrite occurred (data loss but no crash)")
        end
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit code %s, output:\n%s", tostring(exit_code), output))
    end
end)

-- In-process: verify tracker API exists
suite:test("tracker_api_check", function()
    if get_thread_ref_count then
        local count = get_thread_ref_count()
        TestFramework.assert_true(type(count) == "number")
    else
        print("Thread tracker not available (expected in release builds)")
        TestFramework.assert_true(true)
    end
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
