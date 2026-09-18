#!/usr/bin/env lua

-- Regression test for problem 9: DNS base release order
-- cleanup_dnsbase() may free dnsbase with pending requests,
-- causing use-after-free in DNS callbacks.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("evdns cleanup order - DNS base with pending requests")

-- Subprocess: create DNS queries then exit loop abruptly
local CRASH_SCRIPT = [[
local fan = require "fan"
local evdns = require "fan.evdns"

fan.loop(function()
    -- Create custom DNS base
    local dns, err = pcall(function()
        return evdns.create({"8.8.8.8", "1.1.1.1"})
    end)

    -- Start DNS lookups that may still be pending when loop exits
    -- Use many lookups to maximize chance of pending requests
    for i = 1, 50 do
        pcall(function()
            local tcpd = require "fan.tcpd"
            tcpd.connect({
                host = "example-" .. i .. ".nonexistent.invalid",
                port = 80,
                evdns = dns,
                onconnected = function() end,
                onread = function() end,
                ondisconnected = function() end,
            })
        end)
    end

    -- Exit immediately while DNS queries are in-flight
    fan.loopbreak()
end)

-- Loop exited, dnsbase freed by cleanup_dnsbase()
-- If there were pending requests, callbacks may fire on freed memory
os.exit(0)
]]

suite:test("subprocess_dns_base_free_with_pending_requests", function()
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
        "evdns cleanup-order child (DNS base with pending requests)")
end)

-- In-process: create and destroy DNS bases rapidly
suite:test("rapid_dns_base_create_destroy", function()
    for i = 1, 20 do
        local ok, dns = pcall(function()
            return require("fan.evdns").create({"8.8.8.8"})
        end)
        -- DNS base created, will be GC'd at end of scope
    end
    collectgarbage("collect")
    TestFramework.assert_true(true)
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
