#!/usr/bin/env lua

-- Asynchronous fifo (named pipe) behavior tests:
--   reader/writer endpoint pairing in one process,
--   message delivery and ordering,
--   multi-KB payloads through the onread chunker,
--   send_req -> onsendready flow,
--   writer close -> reader ondisconnected,
--   reader close -> writer send failure -> ondisconnected,
--   delete_on_close cleanup.
--
-- FIFO open order matters: the reader end must exist before the writer end
-- (O_WRONLY|O_NONBLOCK fails with ENXIO otherwise), so tests always create
-- the reader first.

local TestFramework = require('test_framework')
local fan = require "fan"
local fifo = require "fan.fifo"

local suite = TestFramework.create_suite("fifo async behavior")

local TMP_DIR = os.getenv("TMPDIR") or "/tmp"
local PATH_BASE = TMP_DIR .. "/luafan-fifo-test-" .. tostring(fan.getpid())

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

local function path_exists(p)
    local f = io.open(p, "rb")
    if f then f:close() return true end
    return false
end

-- 1. Basic roundtrip: writer:send -> reader onread.
suite:test("basic_roundtrip", function()
    local path = PATH_BASE .. "-1"
    local got = nil
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function(data) got = got or tostring(data) end,
    }
    assert(reader, "reader open failed")

    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function() end,
    }
    assert(writer, "writer open failed (reader must exist first)")

    local n = writer:send("hello-fifo")
    assert(n and n > 0, "send returned " .. tostring(n))

    assert(wait_until(function() return got ~= nil end, 10), "reader never got data")
    assert(string.find(got, "hello-fifo", 1, true), "content wrong: " .. tostring(got))
    reader:close()
    writer:close()
    os.remove(path)
end)

-- 2. Ordering: ten sequential writes arrive as a byte stream in FIFO order
-- (a pipe coalesces writes; onread chunks are stream slices, not messages).
suite:test("message_ordering", function()
    local path = PATH_BASE .. "-2"
    local body = ""
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function(data) body = body .. tostring(data) end,
    }
    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function() end,
    }

    local expected = ""
    for i = 1, 10 do
        local msg = "msg-" .. string.format("%02d", i)
        expected = expected .. msg
        assert(writer:send(msg), "send " .. i .. " failed")
    end

    assert(wait_until(function() return #body >= #expected end, 10),
        "stream incomplete: " .. #body .. "/" .. #expected)
    assert(body == expected, "fifo stream content mismatch")
    reader:close()
    writer:close()
    os.remove(path)
end)

-- 3. Payload spanning several pipe chunks is reassembled byte-exact.
suite:test("chunked_payload_reassembly", function()
    local path = PATH_BASE .. "-3"
    local body = ""
    local expected_len = 48 * 1024
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function(data) body = body .. tostring(data) end,
    }
    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function() end,
    }

    local payload = string.rep("0123456789abcdef", 3072) -- 48KB
    -- write in 8KB slices; stop early when the pipe buffer is full and wait
    local sent = 0
    while sent < #payload do
        local slice = string.sub(payload, sent + 1, sent + 8192)
        local n = writer:send(slice)
        if not n or n <= 0 then
            assert(wait_until(function() return false end, 0.05) or true, "")
            fan.sleep(0.01)
            n = writer:send(slice)
        end
        assert(n and n > 0, "slice send failed at " .. sent)
        sent = sent + n
        if n < #slice then
            -- partial write: pipe full; resend the remainder after a beat
            local rest = string.sub(slice, n + 1)
            assert(wait_until(function()
                local m = writer:send(rest)
                if m and m > 0 then
                    sent = sent + m
                    rest = string.sub(rest, m + 1)
                    return #rest == 0
                end
                return false
            end, 10), "pipe drain stalled at " .. sent)
        end
        fan.sleep(0.001)
    end

    assert(wait_until(function() return #body >= expected_len end, 10),
        "payload incomplete: " .. #body)
    assert(body == payload, "reassembled payload mismatch at " .. #body)
    reader:close()
    writer:close()
    os.remove(path)
end)

-- 4. send_req arms the write event; onsendready must fire.
suite:test("send_request_flow", function()
    local path = PATH_BASE .. "-4"
    local ready = 0
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function() end,
    }
    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() ready = ready + 1 end,
        ondisconnected = function() end,
    }

    writer:send("prime")
    writer:send_req()

    assert(wait_until(function() return ready >= 1 end, 10),
        "onsendready never fired after send_req")
    reader:close()
    writer:close()
    os.remove(path)
end)

-- 5. Writer close: reader sees ondisconnected.
suite:test("writer_close_notifies_reader", function()
    local path = PATH_BASE .. "-5"
    local reader_eof = nil
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function() end,
        ondisconnected = function(reason) reader_eof = reason or "eof" end,
    }
    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function() end,
    }

    writer:send("last-words")
    fan.sleep(0.05)
    writer:close()

    assert(wait_until(function() return reader_eof ~= nil end, 10),
        "reader never saw EOF after writer close")
    reader:close()
    os.remove(path)
end)

-- 6. Reader close first: writer send fails and writer sees ondisconnected.
suite:test("reader_close_notifies_writer", function()
    local path = PATH_BASE .. "-6"
    local writer_disconnected = nil
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function() end,
        ondisconnected = function() end,
    }
    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function(reason) writer_disconnected = reason or "eof" end,
    }

    fan.sleep(0.05)
    reader:close()
    fan.sleep(0.1)

    local ok, err = pcall(function() writer:send("into-the-void") end)
    -- send either returns <=0 (EPIPE) or the disconnect callback fires
    assert(wait_until(function() return writer_disconnected ~= nil end, 10),
        "writer never saw disconnect after reader close (send ok=" ..
        tostring(ok) .. " err=" .. tostring(err) .. ")")
    writer:close()
    os.remove(path)
end)

-- 7. delete_on_close removes the fifo node.
suite:test("delete_on_close", function()
    local path = PATH_BASE .. "-7"
    local reader = fifo.connect{
        name = path, rwmode = "r",
        onread = function() end,
        delete_on_close = true,
    }
    local writer = fifo.connect{
        name = path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function() end,
    }
    assert(path_exists(path), "fifo node should exist while open")

    writer:close()
    reader:close()
    fan.sleep(0.05)

    assert(not path_exists(path), "fifo node should be removed after close")
end)

-- 8. Two independent fifo pairs do not cross-deliver.
suite:test("independent_pairs", function()
    local p1, p2 = PATH_BASE .. "-8a", PATH_BASE .. "-8b"
    local got1, got2 = nil, nil
    local r1 = fifo.connect{ name = p1, rwmode = "r",
        onread = function(d) got1 = tostring(d) end }
    local r2 = fifo.connect{ name = p2, rwmode = "r",
        onread = function(d) got2 = tostring(d) end }
    local w1 = fifo.connect{ name = p1, rwmode = "w",
        onsendready = function() end, ondisconnected = function() end }
    local w2 = fifo.connect{ name = p2, rwmode = "w",
        onsendready = function() end, ondisconnected = function() end }

    w1:send("one")
    w2:send("two")

    assert(wait_until(function() return got1 ~= nil and got2 ~= nil end, 10),
        "independent pairs incomplete")
    assert(got1 == "one" and got2 == "two",
        "cross delivery: got1=" .. tostring(got1) .. " got2=" .. tostring(got2))
    r1:close(); r2:close(); w1:close(); w2:close()
    os.remove(p1)
    os.remove(p2)
end)

-- Optional worker pool (LUAN_TEST_WORKERS=N); must precede fan.loop().
TestFramework.init_workers_from_env()

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
