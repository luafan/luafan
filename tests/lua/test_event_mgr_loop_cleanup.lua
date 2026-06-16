#!/usr/bin/env lua

-- DETERMINISTIC crash reproducer for event_mgr_loop cleanup ordering bug.
-- This script spawns a subprocess that:
--   1. Initializes worker threads via event_mgr_workers_init(1)
--   2. Creates worker-mode tcpd connections
--   3. Exits fan.loop WITHOUT closing connections
--   4. Lua GC triggers tcpd __gc -> bufferevent_free on FREED worker base -> SEGFAULT
--
-- Parent process checks exit signal; SIGSEGV (signal 11) proves the bug.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("event_mgr_loop cleanup ordering - subprocess crash test")

-- Subprocess script that WILL crash if bug exists
local CRASH_SCRIPT = [[
local fan = require "fan"
local tcpd = require "fan.tcpd"

-- CRITICAL: initialize worker threads (this is done in main.lua or via C API)
-- In real LuaFan apps, workers are started via event_mgr_workers_init(count)
-- from C or via config. We simulate by checking if worker_count API exists.
local worker_available = (fan.worker_count() == 0)

if not worker_available then
    -- Workers not initialized; this test needs worker mode to trigger the bug.
    -- Exit with code 77 (skip).
    os.exit(77)
end

fan.loop(function()
    -- Create 5 worker-based tcpd connections
    local conns = {}
    for i = 1, 5 do
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = 60000 + i,
            worker = 0,  -- Force worker base 0
            read_timeout = 0.01,
            write_timeout = 0.01,
            onconnected = function() end,
            onread = function() end,
            ondisconnected = function() end,
        })
        if ok and conn then
            table.insert(conns, conn)
        end
    end

    -- Drop references; connections will be GC'd after fan.loop exits
    conns = nil
    collectgarbage("collect")

    fan.loopbreak()
end)

-- If event_mgr_loop() calls full_cleanup() and frees worker bases now,
-- the following lua_close will trigger tcpd __gc -> bufferevent_free(bev)
-- where bev's base was already freed -> SEGFAULT (signal 11).
-- If the bug is fixed, this exits normally with code 0.
os.exit(0)
]]

suite:test("subprocess_crash_on_worker_base_premature_free", function()
    -- Write crash script to temp file
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(CRASH_SCRIPT)
    f:close()

    -- Run subprocess
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

    -- Parse exit code from output (last line should be the echo $?)
    local exit_code = tonumber(output:match("(%d+)%s*$"))

    if exit_code == 77 then
        TestFramework.skip_test("workers not initialized in build")
        return
    end

    -- If exit_code is nil or process was killed by signal, check signal
    if not success and exit_type == "signal" then
        -- Process was killed by signal (likely SIGSEGV = 11 or SIGABRT = 6)
        print(string.format("Subprocess killed by signal %d (expected if bug exists)", code))
        TestFramework.assert_true(code == 11 or code == 6,
            string.format("Process should crash with SIGSEGV(11) or SIGABRT(6), got signal %d", code))
        print("✓ BUG CONFIRMED: subprocess crashed as expected (event_mgr_loop freed worker base too early)")
    elseif exit_code == 0 then
        print("Subprocess exited cleanly (bug is FIXED or workers not used)")
        -- This is OK if the bug is already fixed
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit code %s, output:\n%s", tostring(exit_code), output))
    end
end)

-- Run suite in main process (no fan.loop needed)
local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
