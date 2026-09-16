#!/usr/bin/env lua

-- udpd HIGH-CONCURRENCY stress:
--   1. 200-packet burst, exact count and order on loopback
--   2. 5 concurrent socket pairs x 60 packets (no cross-delivery)
--   3. high-rate paced bursts: 5 rounds x 50 packets
--   4. 10 sockets bound concurrently on distinct ports, all live

local TestFramework = require('test_framework')
local fan = require "fan"
local udpd = require "fan.udpd"

local suite = TestFramework.create_suite("udpd high-concurrency stress")

local PORT_BASE = 31000 + (fan.getpid() % 500) * 20

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

local function open_socket(port, onread)
    local sock = udpd.new{
        bind_host = "127.0.0.1",
        bind_port = port,
        onread = function(conn, buf, from)
            if onread then onread(tostring(buf), from) end
        end,
        onsendready = function() end,
    }
    assert(sock, "udpd.new failed on " .. tostring(port))
    return sock
end

-- 1. 200-packet burst: exact count and order.
suite:test("burst_200_ordered", function()
    local received = {}
    local b = open_socket(PORT_BASE + 1, function(buf) table.insert(received, buf) end)
    local a = open_socket(PORT_BASE + 2, nil)
    local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 1)

    for i = 1, 200 do
        local ok = a:send(string.format("b%04d", i), dest)
        assert(ok, "burst send " .. i .. " failed")
    end

    assert(wait_until(function() return #received == 200 end, 30),
        "burst incomplete: " .. #received .. "/200")
    for i = 1, 200 do
        assert(received[i] == string.format("b%04d", i),
            "burst order broken at " .. i .. ": " .. tostring(received[i]))
    end
    a:close()
    b:close()
end)

-- 2. Five concurrent pairs, 60 packets each; per-pair integrity.
suite:test("five_pairs_concurrent", function()
    local PAIRS = 5
    local PER = 60
    local rx = {}
    local socks = {}

    for p = 1, PAIRS do
        local rport = PORT_BASE + 10 + p * 2
        local wport = rport + 1
        local r = open_socket(rport, function(buf)
            rx[p] = (rx[p] or "") .. buf
        end)
        local w = open_socket(wport, nil)
        socks[p] = { r = r, w = w, dest = udpd.make_dest("127.0.0.1", rport) }
    end

    for p = 1, PAIRS do
        for i = 1, PER do
            local msg = string.format("p%dx%03d|", p, i)
            assert(socks[p].w:send(msg, socks[p].dest), "pair " .. p .. " send " .. i)
        end
    end

    local function complete()
        for p = 1, PAIRS do
            local want = PER * #"p1x001|"
            if not rx[p] or #rx[p] < want then return false end
        end
        return true
    end
    assert(wait_until(complete, 30), "five-pair delivery incomplete")

    for p = 1, PAIRS do
        local want = ""
        for i = 1, PER do
            want = want .. string.format("p%dx%03d|", p, i)
        end
        assert(rx[p] == want, "pair " .. p .. " content mismatch")
        socks[p].r:close()
        socks[p].w:close()
    end
end)

-- 3. High-rate paced bursts: 5 rounds x 50 packets with 1ms gaps.
suite:test("paced_burst_rounds", function()
    local received = 0
    local b = open_socket(PORT_BASE + 40, function() received = received + 1 end)
    local a = open_socket(PORT_BASE + 41, nil)
    local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 40)

    local total = 0
    for round = 1, 5 do
        for i = 1, 50 do
            assert(a:send(string.format("r%dc%02d", round, i), dest), "paced send failed")
            total = total + 1
        end
        fan.sleep(0.001)
    end

    assert(wait_until(function() return received == total end, 30),
        "paced delivery incomplete: " .. received .. "/" .. total)
    a:close()
    b:close()
end)

-- 4. Ten sockets bound and live simultaneously.
suite:test("ten_sockets_live", function()
    local socks = {}
    local hit = {}
    for i = 1, 10 do
        local port = PORT_BASE + 60 + i
        socks[i] = open_socket(port, function(buf)
            if tostring(buf) == "s" .. i then hit[i] = true end
        end)
    end

    for i = 1, 10 do
        local s = socks[i]
        local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 60 + i)
        -- self-send is legal for UDP loopback
        assert(s:send("s" .. i, dest), "self send " .. i .. " failed")
    end

    assert(wait_until(function()
        for i = 1, 10 do if not hit[i] then return false end end
        return true
    end, 30), "ten-socket delivery incomplete")
    for i = 1, 10 do socks[i]:close() end
end)

-- Optional worker pool (LUAN_TEST_WORKERS=N); must precede fan.loop().
TestFramework.init_workers_from_env()

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
