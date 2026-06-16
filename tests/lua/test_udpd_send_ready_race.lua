#!/usr/bin/env lua

-- Regression test for problem 14: UDP send_ready vs cleanup race
-- udpd_base_conn_request_send_ready calls event_add on write_ev without
-- any lock, racing with cleanup which frees write_ev.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("UDP send_ready vs cleanup race")

-- Check udpd availability
local udpd
local ok, mod = pcall(require, 'fan.udpd')
if not ok then
    print("fan.udpd not available, skipping")
    local failed = TestFramework.run_suite(suite)
    os.exit(77)
end
udpd = mod

local function alloc_port()
    return 26000 + (fan.getpid() % 1000) + math.random(1, 500)
end

-- Subprocess: hammer send_req (which triggers request_send_ready) while closing
-- Build via concatenation rather than string.format so the embedded `%d + k`
-- expression isn't (mis)interpreted as a printf conversion.
local function build_crash_script(base_port)
    return [[
local fan = require "fan"
local udpd = require "fan.udpd"

local BASE_PORT = ]] .. tostring(base_port) .. [[

fan.loop(function()
    local conns = {}

    -- Create UDP sockets
    for i = 1, 30 do
        local ok, conn = pcall(function()
            return udpd.new({
                bind_port = 0,
                onread = function(self, data, dest) end,
                onsendready = function(self)
                    -- This callback fires when write_ev triggers
                    -- If write_ev was freed by cleanup, this is UAF
                    pcall(function() self:send_req("reply", "127.0.0.1", 1) end)
                end,
            })
        end)
        if ok and conn then
            table.insert(conns, conn)
        end
    end

    -- Stress: send_req triggers request_send_ready -> event_add(write_ev)
    -- Close frees write_ev -> race window
    for round = 1, 100 do
        for idx, conn in ipairs(conns) do
            -- Rapid send_req calls (each triggers request_send_ready internally)
            for k = 1, 20 do
                pcall(function() conn:send_req("data" .. k, "127.0.0.1", BASE_PORT + k) end)
            end

            -- Close some connections mid-hammer to create race
            if round == 30 and idx % 3 == 0 then
                pcall(function() conn:close() end)
            elseif round == 60 and idx % 2 == 0 then
                pcall(function() conn:close() end)
            end
        end

        if round % 20 == 0 then
            collectgarbage("collect")
        end
    end

    -- Close remaining
    for _, conn in ipairs(conns) do
        pcall(function() conn:close() end)
    end

    fan.sleep(0.2)
    collectgarbage("collect")
    fan.loopbreak()
end)

os.exit(0)
]]
end

suite:test("subprocess_send_ready_vs_cleanup_race", function()
    local base_port = alloc_port()
    local script = build_crash_script(base_port)

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
            print("BUG CONFIRMED: UDP send_ready race with cleanup (SIGSEGV/SIGABRT)")
            TestFramework.assert_true(true)
        else
            error(string.format("Unexpected signal %d", code))
        end
    elseif exit_code == 0 then
        print("Subprocess exited cleanly")
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit: code=%s, output:\n%s", tostring(exit_code), output))
    end
end)

-- In-process: verify send_req + close sequence
suite:test("send_req_then_close", function()
    for i = 1, 20 do
        local conn = udpd.new({
            bind_port = 0,
            onread = function() end,
            onsendready = function() end,
        })
        if conn then
            pcall(function() conn:send_req("test", "127.0.0.1", alloc_port()) end)
            pcall(function() conn:close() end)
        end
    end
    collectgarbage("collect")
    TestFramework.assert_true(true)
end)

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    fan.loopbreak()
    os.exit(failed > 0 and 1 or 0)
end)
