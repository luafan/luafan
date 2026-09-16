#!/usr/bin/env lua

-- Asynchronous udpd behavior tests:
--   paired send/receive between two sockets,
--   burst packet ordering/count on loopback,
--   bidirectional round trip,
--   make_dest + from-address visibility,
--   packet size validation,
--   close stops callbacks cleanly.

local TestFramework = require('test_framework')
local fan = require "fan"
local udpd = require "fan.udpd"

local suite = TestFramework.create_suite("udpd async behavior")

local PORT_BASE = 27000 + (fan.getpid() % 2000) * 2

local function wait_until(cond, timeout, step)
    timeout = timeout or 10
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

-- Open a bound udpd socket with onread capture.
local function open_socket(port, onread)
    local sock = udpd.new{
        bind_host = "127.0.0.1",
        bind_port = port,
        onread = function(conn, buf, from)
            if onread then onread(tostring(buf), from) end
        end,
        onsendready = function() end,
    }
    assert(sock, "udpd.new failed on port " .. tostring(port))
    return sock
end

-- 1. Paired send/receive: A -> B, packet content and sender address intact.
suite:test("pair_send_receive", function()
    local got = nil
    local got_from_host, got_from_port = nil, nil
    local b = open_socket(PORT_BASE + 1, function(buf, from)
        if not got then
            got = buf
            got_from_host = from:getHost()
            got_from_port = from:getPort()
        end
    end)
    local a = open_socket(PORT_BASE + 2, nil)

    local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 1)
    a:send("hello-udp", dest)

    assert(wait_until(function() return got ~= nil end, 10), "packet never arrived")
    assert(got == "hello-udp", "packet content wrong: " .. tostring(got))
    assert(got_from_host == "127.0.0.1", "from host wrong: " .. tostring(got_from_host))
    assert(got_from_port == PORT_BASE + 2, "from port wrong: " .. tostring(got_from_port))
    a:close()
    b:close()
end)

-- 2. Burst: 20 packets arrive complete and in order on loopback.
suite:test("burst_packets_order", function()
    local received = {}
    local b = open_socket(PORT_BASE + 3, function(buf)
        table.insert(received, buf)
    end)
    local a = open_socket(PORT_BASE + 4, nil)
    local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 3)

    local total = 0
    for i = 1, 20 do
        local msg = "pkt-" .. string.format("%03d", i)
        local ok = a:send(msg, dest)
        assert(ok, "send " .. i .. " failed")
        total = total + 1
    end
    assert(total == 20)

    assert(wait_until(function() return #received == 20 end, 15),
        "burst incomplete: " .. #received)
    for i = 1, 20 do
        assert(received[i] == "pkt-" .. string.format("%03d", i),
            "burst packet " .. i .. " wrong: " .. tostring(received[i]))
    end
    a:close()
    b:close()
end)

-- 3. Bidirectional round trip A -> B -> A.
suite:test("bidirectional_roundtrip", function()
    local a_got, b_got = nil, nil
    local a = open_socket(PORT_BASE + 5, function(buf) a_got = a_got or buf end)
    local b = open_socket(PORT_BASE + 6, function(buf) b_got = b_got or buf end)

    local dest_b = udpd.make_dest("127.0.0.1", PORT_BASE + 6)
    local dest_a = udpd.make_dest("127.0.0.1", PORT_BASE + 5)

    a:send("ping", dest_b)
    assert(wait_until(function() return b_got ~= nil end, 10), "B never got ping")
    b:send("pong", dest_a)
    assert(wait_until(function() return a_got ~= nil end, 10), "A never got pong")
    assert(b_got == "ping" and a_got == "pong", "roundtrip content wrong")
    a:close()
    b:close()
end)

-- 4. Interleaved send/onsendready: send_request arms the write event and the
-- callback fires even when nothing is queued yet.
suite:test("send_request_ready_notification", function()
    local ready_fires = 0
    local sock = udpd.new{
        host = "127.0.0.1",
        port = PORT_BASE + 7,
        onread = function() end,
        onsendready = function() ready_fires = ready_fires + 1 end,
    }
    sock:send_req()
    assert(wait_until(function() return ready_fires >= 1 end, 10),
        "onsendready never fired after send_request")
    sock:close()
end)

-- 5. Packet size validation: oversize datagram is rejected by send.
suite:test("packet_size_validation", function()
    local b = open_socket(PORT_BASE + 8, function() end)
    local a = open_socket(PORT_BASE + 9, nil)
    local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 8)

    -- a payload beyond any legal UDP datagram (~70KB > 64KiB limit)
    local oversized = string.rep("z", 70 * 1024)
    local ok, err = a:send(oversized, dest)
    assert(not ok, "oversize send should fail, got " .. tostring(ok))
    assert(err and #err > 0, "oversize send should return a reason")
    a:close()
    b:close()
end)

-- 6. close() stops callbacks: nothing arrives after the receiver closed and
-- the sender does not crash.
suite:test("close_stops_callbacks", function()
    local count = 0
    local b = open_socket(PORT_BASE + 10, function() count = count + 1 end)
    local a = open_socket(PORT_BASE + 11, nil)
    local dest = udpd.make_dest("127.0.0.1", PORT_BASE + 10)

    a:send("before-close", dest)
    assert(wait_until(function() return count == 1 end, 10), "pre-close packet lost")

    b:close()
    fan.sleep(0.1)
    a:send("after-close", dest)  -- ICMP may or may not surface; must not crash
    fan.sleep(0.2)
    assert(count == 1, "callbacks fired after close: " .. count)
    a:close()
end)

-- 7. Multiple destinations from one socket.
suite:test("multiple_destinations", function()
    local got_b, got_c = nil, nil
    local b = open_socket(PORT_BASE + 12, function(buf) got_b = buf end)
    local c = open_socket(PORT_BASE + 13, function(buf) got_c = buf end)
    local a = open_socket(PORT_BASE + 14, nil)

    local dest_b = udpd.make_dest("127.0.0.1", PORT_BASE + 12)
    local dest_c = udpd.make_dest("127.0.0.1", PORT_BASE + 13)
    a:send("for-b", dest_b)
    a:send("for-c", dest_c)

    assert(wait_until(function() return got_b ~= nil and got_c ~= nil end, 10),
        "multi-destination delivery incomplete")
    assert(got_b == "for-b" and got_c == "for-c", "multi-destination content wrong")
    a:close()
    b:close()
    c:close()
end)

-- Optional worker pool (LUAN_TEST_WORKERS=N); must precede fan.loop().
TestFramework.init_workers_from_env()

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
