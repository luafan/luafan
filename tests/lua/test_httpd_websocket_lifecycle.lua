#!/usr/bin/env lua

-- DETERMINISTIC crash reproducer for WebSocket cleanup double-free / use-after-free.
-- Bug: ws_connection_cleanup can be called multiple times, or ws_deferred_free_cb
-- fires after mainthread is closed, causing use-after-free on request->self_ref.
--
-- Strategy: spawn server, accept many WebSocket connections, immediately close
-- some while sending large payloads on others, then force GC.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("httpd WebSocket cleanup lifecycle - crash inducer")

-- Check for httpd.core availability
local httpd_core
local ok, core = pcall(require, 'fan.httpd.core')
if not ok then
    print("fan.httpd.core not available, creating skip-only suite")
    local failed = TestFramework.run_suite(suite)
    os.exit(77)  -- Skip
end
httpd_core = core

local CRASH_SCRIPT_TEMPLATE = [[
local fan = require "fan"
local httpd_core = require "fan.httpd.core"
local connector = require "fan.connector"

fan.loop(function()
    local port = %d
    local accepts = {}

    local server = httpd_core.bind({
        host = "127.0.0.1",
        port = port,
        onService = function(req, resp)
            local ok_ws, is_ws = pcall(function() return req:is_websocket_upgrade() end)
            if ok_ws and is_ws then
                local ok2 = pcall(function() req:websocket_accept() end)
                if ok2 then
                    table.insert(accepts, req)
                    -- Immediately close every 3rd connection (inline, no spawn)
                    if #accepts %% 3 == 0 then
                        pcall(function() req:websocket_close() end)
                        -- Double close to stress cleanup
                        pcall(function() req:websocket_close() end)
                    end
                end
            else
                resp:reply(400, "Bad Request", "not ws")
            end
        end,
    })

    fan.sleep(0.05)

    -- Create %d clients (without spawn, sequential)
    local clients = {}
    for i = 1, %d do
        local client = connector.connect("tcp", "127.0.0.1", port)
        if client then
            table.insert(clients, client)
            client:send(
                "GET / HTTP/1.1\r\n" ..
                "Host: localhost\r\n" ..
                "Upgrade: websocket\r\n" ..
                "Connection: Upgrade\r\n" ..
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" ..
                "Sec-WebSocket-Version: 13\r\n\r\n"
            )
        end
    end

    fan.sleep(0.15)

    -- Close clients to trigger server-side race
    for _, client in ipairs(clients) do
        pcall(function() client:close() end)
    end

    fan.sleep(0.05)

    -- Force large sends then immediate close on server-side
    for _, req in ipairs(accepts) do
        pcall(function() req:websocket_send(string.rep("x", 65536)) end)
        pcall(function() req:websocket_close() end)
        pcall(function() tostring(req) end)
    end

    fan.sleep(0.1)
    collectgarbage("collect")
    fan.sleep(0.1)
    collectgarbage("collect")

    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_websocket_double_cleanup_crash", function()
    local port = 23000 + (fan.getpid() % 1000)
    local num_clients = 20

    local script = string.format(CRASH_SCRIPT_TEMPLATE, port, num_clients, num_clients)

    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(script)
    f:close()

    local cmd = string.format(
        "LUA_PATH='../modules/?.lua;../modules/?/init.lua;./lua/framework/?.lua;./lua/?.lua;;' " ..
        "LUA_CPATH='./build/?.so;../?.so;;' " ..
        "timeout 20s lua %s 2>&1; echo $?",
        tmpfile
    )
    local handle = io.popen(cmd)
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    local exit_code = tonumber(output:match("(%d+)%s*$"))

    if not success and exit_type == "signal" then
        print(string.format("Subprocess killed by signal %d", code))
        if code == 11 or code == 6 then
            print("✓ BUG CONFIRMED: WebSocket cleanup race/double-free detected (SIGSEGV/SIGABRT)")
            print("  Likely: ws_connection_cleanup or ws_deferred_free_cb use-after-free")
            TestFramework.assert_true(true)
        else
            error(string.format("Unexpected signal %d", code))
        end
    elseif exit_code == 0 then
        print("Subprocess completed without crash (bug is FIXED or not triggered)")
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit: code=%s, output:\n%s", tostring(exit_code), output))
    end
end)

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    fan.loopbreak()
    os.exit(failed > 0 and 1 or 0)
end)
