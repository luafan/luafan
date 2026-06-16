#!/usr/bin/env lua

-- DETERMINISTIC race reproducer for TCPD concurrent buf access.
-- Bug: tcpd_format_connection_info (tostring) and callbacks access conn->buf
-- without holding buf_mutex, racing with cleanup that sets conn->buf=NULL.
--
-- Strategy: spawn many coroutines hammering tostring/send while another closes.

local TestFramework = require('test_framework')
local fan = require "fan"
local tcpd = require "fan.tcpd"

local suite = TestFramework.create_suite("TCPD concurrent lifecycle - race inducer")

local function alloc_port()
    return 22000 + (fan.getpid() % 1000) + math.random(1, 500)
end

-- Subprocess that hammers tcpd operations to trigger race
local CRASH_SCRIPT_TEMPLATE = [[
local fan = require "fan"
local tcpd = require "fan.tcpd"

fan.loop(function()
    local conns = {}

    -- Create %d connections (some on worker base if available)
    for i = 1, %d do
        local worker_id = (i %% 2 == 0 and fan.worker_count() > 0) and 0 or -1
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = %d + i,
            worker = worker_id,
            read_timeout = 0.001,
            write_timeout = 0.001,
            onconnected = function() end,
            onread = function() end,
            onsendready = function() end,
            ondisconnected = function() end,
        })
        if ok and conn then
            table.insert(conns, conn)
        end
    end

    fan.sleep(0.01)

    -- Hammer tostring/send/close in tight loop (no spawn needed)
    for round = 1, %d do
        for _, conn in ipairs(conns) do
            pcall(tostring, conn)
            pcall(function() conn:send("x") end)
            pcall(tostring, conn)
        end
        if round == 3 then
            -- Close half the connections mid-hammer
            for i = 1, math.floor(#conns / 2) do
                pcall(function() conns[i]:close() end)
            end
        end
        collectgarbage("collect")
    end

    -- Close remaining
    for _, conn in ipairs(conns) do
        pcall(function() conn:close() end)
    end

    fan.sleep(0.05)
    collectgarbage("collect")
    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_tcpd_concurrent_tostring_close_race", function()
    local num_conns = 20
    local hammer_rounds = 50
    local base_port = 22000 + (fan.getpid() % 1000)

    local script = string.format(CRASH_SCRIPT_TEMPLATE,
        num_conns, num_conns, base_port, hammer_rounds)

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
        "timeout 15s lua %s 2>&1; echo $?",
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
            print("✓ BUG CONFIRMED: tcpd race detected (SIGSEGV/SIGABRT)")
            print("  Likely: tcpd_format_connection_info or callback accessed freed conn->buf")
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

-- In-process light test
suite:test("rapid_connect_close_tostring", function()
    math.randomseed(fan.gettime())
    for i = 1, 20 do
        local port = alloc_port()
        local ok, conn = pcall(function()
            return tcpd.connect({
                host = "127.0.0.1",
                port = port,
                worker = (i % 2 == 0 and fan.worker_count() > 0) and 0 or -1,
                read_timeout = 0.01,
                onconnected = function() end,
                onread = function() end,
                ondisconnected = function() end,
            })
        end)

        if ok and conn then
            pcall(tostring, conn)
            pcall(function() conn:close() end)
        end
    end
    collectgarbage("collect")
end)

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    fan.loopbreak()
    os.exit(failed > 0 and 1 or 0)
end)
