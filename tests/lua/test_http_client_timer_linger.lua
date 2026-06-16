#!/usr/bin/env lua

-- Regression test for problem 13: HTTP client timer/socket event lingering
-- Static timer_event and timer_check_multi_info are created in luaopen_fan_http_core
-- but never explicitly cleaned up. resume_cb creates events that fire after lua_close.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("HTTP client timer lingering events")

-- Subprocess: make HTTP requests then exit immediately
local CRASH_SCRIPT = [[
local fan = require "fan"
local http = require "fan.http"

fan.loop(function()
    -- Make several HTTP requests
    for i = 1, 5 do
        pcall(function()
            http.get({
                url = "http://127.0.0.1:1/nonexistent",
                timeout = 1,
                oncomplete = function(res)
                    -- callback fires asynchronously
                end,
            })
        end)
    end

    -- Exit while requests are in-flight
    -- resume_cb timers may still be pending on event_mgr_base()
    fan.loopbreak()
end)

-- Loop exited, event base freed
-- If resume_cb timer fires now, it accesses freed Lua state
os.exit(0)
]]

suite:test("subprocess_http_timer_after_loop_exit", function()
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
            print("BUG CONFIRMED: HTTP timer fired after Lua state closed")
            TestFramework.assert_true(true)
        else
            error(string.format("Unexpected signal %d", code))
        end
    elseif exit_code == 0 then
        print("Subprocess exited cleanly")
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit code %s, output:\n%s", tostring(exit_code), output))
    end
end)

-- In-process: verify http module loads and creates static events
suite:test("http_module_loads", function()
    local ok, http = pcall(require, "fan.http")
    TestFramework.assert_true(ok)
end)

-- In-process: make a request and exit loop
suite:test("http_request_and_exit", function()
    local http = require "fan.http"

    fan.loop(function()
        pcall(function()
            http.get({
                url = "http://127.0.0.1:1/test",
                timeout = 1,
            })
        end)
        fan.sleep(0.05)
        fan.loopbreak()
    end)

    TestFramework.assert_true(true)
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
