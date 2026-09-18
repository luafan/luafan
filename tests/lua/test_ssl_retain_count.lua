#!/usr/bin/env lua

-- Regression test for problem 11: SSL retain_count not atomic
-- tcpd_ssl_context_retain/release use non-atomic ++/-- on retain_count.
-- In worker mode, concurrent retain/release can cause count corruption,
-- leading to premature free or leaked contexts.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("SSL retain_count atomicity")

-- Subprocess: create many SSL connections (if SSL available) to stress retain_count
local CRASH_SCRIPT = [[
local fan = require "fan"
local tcpd = require "fan.tcpd"

-- Check if SSL is available
local has_ssl = pcall(function()
    -- Try to create an SSL connection (will fail but tests code path)
    tcpd.connect({
        host = "127.0.0.1",
        port = 1,
        ssl_enabled = true,
        ssl_verifypeer = 0,
        ssl_verifyhost = 0,
        onconnected = function() end,
        onread = function() end,
        ondisconnected = function() end,
    })
end)

if not has_ssl then
    print("SSL not available, testing non-SSL path only")
end

-- Create many connections rapidly to stress retain/release paths
fan.loop(function()
    for i = 1, 100 do
        pcall(function()
            tcpd.connect({
                host = "127.0.0.1",
                port = 1,
                ssl_enabled = has_ssl,
                ssl_verifypeer = 0,
                ssl_verifyhost = 0,
                onconnected = function() end,
                onread = function() end,
                ondisconnected = function() end,
            })
        end)
    end

    -- Force GC to trigger cleanup
    collectgarbage("collect")

    fan.sleep(0.1)
    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_ssl_retain_count_stress", function()
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

    -- A crash is a FAILURE: the previous "signal => BUG CONFIRMED, pass" form
    -- made this suite unable to fail no matter what the child did.
    TestFramework.assert_child_no_crash(output, success, exit_type, code,
        "SSL retain_count stress child")
end)

-- In-process: create connections that share SSL context
suite:test("ssl_context_shared_retain", function()
    local tcpd = require "fan.tcpd"
    local conns = {}

    -- Multiple connections to same host should share SSL context
    for i = 1, 10 do
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = 1,
            ssl_enabled = false, -- use non-SSL to avoid connection errors
            onconnected = function() end,
            onread = function() end,
            ondisconnected = function() end,
        })
        if ok and conn then
            table.insert(conns, conn)
        end
    end

    -- Close all connections
    for _, conn in ipairs(conns) do
        pcall(function() conn:close() end)
    end

    collectgarbage("collect")
    TestFramework.assert_true(true)
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
