#!/usr/bin/env lua

-- Regression test for problem 12: tcpd_base_conn_cleanup using closed mainthread
-- If Lua state is closed before __gc runs, mainthread pointer is invalid.
-- tcpd_base_conn_cleanup uses mainthread to CLEAR_REF and tcpd_ssl_context_release.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("tcpd_base_conn_cleanup with closed mainthread")

-- Subprocess: create connections, then exit Lua abruptly
local CRASH_SCRIPT = [[
local fan = require "fan"
local tcpd = require "fan.tcpd"

fan.loop(function()
    local conns = {}

    -- Create connections
    for i = 1, 20 do
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = 1,
            onconnected = function() end,
            onread = function() end,
            ondisconnected = function() end,
        })
        if ok and conn then
            table.insert(conns, conn)
        end
    end

    -- Don't close connections explicitly
    -- Let Lua __gc handle cleanup when lua_close is called
    -- This tests whether mainthread is still valid during __gc

    fan.loopbreak()
end)

-- At this point, fan.loop returned, event_mgr_loop() called full_cleanup()
-- which freed the event base. Now Lua will run __gc on userdata.
-- If mainthread is invalid at this point, crash occurs.
os.exit(0)
]]

suite:test("subprocess_cleanup_with_closed_mainthread", function()
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(CRASH_SCRIPT)
    f:close()

    local cmd = TestFramework.child_lua_command(tmpfile, 15) .. "; echo $?"
    local handle = io.popen(cmd .. " 2>&1")
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    -- A crash is a FAILURE: the previous "signal => pass" form made this suite
    -- unable to fail no matter what the child did.
    TestFramework.assert_child_no_crash(output, success, exit_type, code,
        "tcpd_base_conn_cleanup child (closed mainthread)")
end)

-- In-process: create and GC connections without explicit close
suite:test("gc_cleanup_without_explicit_close", function()
    local tcpd = require "fan.tcpd"

    for i = 1, 10 do
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = 1,
            onconnected = function() end,
            onread = function() end,
            ondisconnected = function() end,
        })
    end

    -- Force GC while event loop is still alive
    collectgarbage("collect")
    TestFramework.assert_true(true)
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
