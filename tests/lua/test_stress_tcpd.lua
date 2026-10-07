#!/usr/bin/env lua

-- tcpd HIGH-CONCURRENCY stress:
--   1. 100 concurrent echo clients x 10 messages (1000 exchanges)
--   2. connect/echo/close churn: 60 concurrent cycles under load
--   3. parallel downstream push: 20 clients x 64KB
--
-- Watchdogs bound every case; failures report per-client diagnostics.

local TestFramework = require('test_framework')
local fan = require "fan"
local tcpd = require "fan.tcpd"

local suite = TestFramework.create_suite("tcpd high-concurrency stress")

local function wait_until(cond, timeout, step)
    timeout = timeout or 30
    step = step or 0.02
    local deadline = fan.gettime() + timeout
    while not cond() do
        if fan.gettime() > deadline then
            return false
        end
        fan.sleep(step)
    end
    return true
end

local function bind_server(opts)
    local srv = tcpd.bind(opts)
    local info = srv and srv:localinfo()
    return srv, info and info.port
end

-- 1. 100 concurrent echo clients x 10 messages.
suite:test("echo_100_concurrent", function()
    local MSGS = 10
    local CLIENTS = 100
    local received = {}
    local clients = {}

    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function(conn, buf) conn:send(buf) end,
                ondisconnected = function() end,
            }
        end,
    })
    assert(type(port) == "number" and port > 0, "bind failed")

    for i = 1, CLIENTS do
        coroutine.wrap(function()
            local expected = ""
            for m = 1, MSGS do
                expected = expected .. "c" .. i .. "m" .. m .. "|"
            end
            local conn = tcpd.connect({
                host = "127.0.0.1", port = port,
                onread = function(_, buf)
                    received[i] = (received[i] or "") .. tostring(buf)
                end,
                ondisconnected = function() end,
            })
            if not conn then
                received[i] = "CONNECT-FAIL"
                return
            end
            clients[i] = conn
            conn:send(expected)
        end)()
    end

    local function all_done()
        for i = 1, CLIENTS do
            if not received[i] or #received[i] < #(tostring(received[i])) then
                return false
            end
            if #received[i] < 10 then return false end
        end
        return true
    end
    -- completion check: every client saw its full payload (length match per client)
    local function complete()
        for i = 1, CLIENTS do
            local want = 0
            for m = 1, MSGS do
                want = want + #"c" + #tostring(i) + #"m" + #tostring(m) + 1
            end
            if not received[i] or #received[i] ~= want then
                return false
            end
        end
        return true
    end
    if not wait_until(complete, 60) then
        local missing = {}
        for i = 1, CLIENTS do
            if not received[i] then missing[#missing+1] = i .. ":nil"
            else missing[#missing+1] = i .. ":" .. #received[i] end
        end
        error("echo_100_concurrent incomplete: " .. table.concat(missing, " "), 0)
    end
    for i = 1, CLIENTS do
        local want = ""
        for m = 1, MSGS do
            want = want .. "c" .. i .. "m" .. m .. "|"
        end
        assert(received[i] == want, "client " .. i .. " payload mismatch")
        clients[i]:close()
    end
    server:close()
end)

-- 2. Churn: 60 concurrent connect → echo → close cycles.
suite:test("churn_60_concurrent_cycles", function()
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function(conn, buf) conn:send(buf) end,
                ondisconnected = function() end,
            }
        end,
    })
    assert(type(port) == "number" and port > 0, "bind failed")

    local completed = 0
    for i = 1, 60 do
        coroutine.wrap(function()
            local got = false
            local conn = tcpd.connect({
                host = "127.0.0.1", port = port,
                onread = function(_, buf)
                    if tostring(buf) == "churn-" .. i then
                        got = true
                        completed = completed + 1
                    end
                end,
                ondisconnected = function() end,
            })
            if conn then
                conn:send("churn-" .. i)
                -- Wait for THIS client's echo (bounded) before closing. A blind
                -- sleep is not enough: with an event-worker pool the
                -- connect/echo path needs tens of ms under 60-way concurrency
                -- (measured ~50ms for 60 concurrent connects vs ~6ms
                -- single-threaded), so closing on a fixed 20-50ms budget drops
                -- the echo instead of exercising the churn lifecycle.
                local deadline = fan.gettime() + 5
                while not got and fan.gettime() < deadline do
                    fan.sleep(0.005)
                end
                -- keep the staggered lifetimes so the cycles still overlap
                fan.sleep(0.02 + (i % 5) * 0.01)
                conn:close()
            end
        end)()
    end

    assert(wait_until(function() return completed == 60 end, 30),
        "churn completed only " .. completed .. "/60")
    server:close()
end)

-- 3. Parallel downstream push: server sends 64KB to each of 20 clients.
suite:test("parallel_downstream_push", function()
    local CLIENTS = 20
    local PAYLOAD = string.rep("D", 64 * 1024)
    local bound = {}
    local push = {}

    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function() end,
                ondisconnected = function() end,
            }
            bound[#bound + 1] = apt
            if push then
                push(apt)
            end
        end,
    })
    assert(type(port) == "number" and port > 0, "bind failed")

    local received = {}
    local conns = {}
    local want_clients = CLIENTS
    push = function(apt)
        if #bound == want_clients then
            for _, a in ipairs(bound) do
                a:send(PAYLOAD)
            end
        end
    end

    for i = 1, CLIENTS do
        coroutine.wrap(function()
            local conn = tcpd.connect({
                host = "127.0.0.1", port = port,
                onread = function(_, buf)
                    received[i] = (received[i] or "") .. tostring(buf)
                end,
                ondisconnected = function() end,
            })
            conns[i] = conn
            assert(conn, "connect " .. i .. " failed")
        end)()
    end

    local function complete()
        for i = 1, CLIENTS do
            if not received[i] or #received[i] ~= #PAYLOAD then
                return false
            end
        end
        return true
    end
    assert(wait_until(complete, 30), "downstream push incomplete")
    for i = 1, CLIENTS do
        assert(received[i] == PAYLOAD, "client " .. i .. " payload mismatch")
        conns[i]:close()
    end
    server:close()
end)


fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
