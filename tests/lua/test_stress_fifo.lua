#!/usr/bin/env lua

-- fifo HIGH-CONCURRENCY stress:
--   1. 10 independent fifo pairs writing/reading concurrently (50 msgs each)
--   2. open/close churn: 30 create→write→read→destroy cycles
--   3. 1MB stream through one pair while 5 other pairs exchange small msgs
--
-- (single process: reader/writer endpoints per pair)

local TestFramework = require('test_framework')
local fan = require "fan"
local fifo = require "fan.fifo"

local suite = TestFramework.create_suite("fifo high-concurrency stress")

local TMP_DIR = os.getenv("TMPDIR") or "/tmp"
local PATH_BASE = TMP_DIR .. "/luafan-fifo-stress-" .. tostring(fan.getpid())

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

-- 1. Ten pairs active at once.
suite:test("ten_pairs_concurrent", function()
    local PAIRS = 10
    local MSGS = 50
    local bodies = {}
    local readers, writers = {}, {}

    for p = 1, PAIRS do
        local path = PATH_BASE .. "-p" .. p
        bodies[p] = { got = "", expected = "" }
        readers[p] = fifo.connect{
            name = path, rwmode = "r",
            onread = function(data) bodies[p].got = bodies[p].got .. tostring(data) end,
        }
        writers[p] = fifo.connect{
            name = path, rwmode = "w",
            onsendready = function() end,
            ondisconnected = function() end,
        }
    end

    -- interleave writes across all pairs
    for m = 1, MSGS do
        for p = 1, PAIRS do
            local msg = string.format("p%02dm%02d|", p, m)
            bodies[p].expected = bodies[p].expected .. msg
            assert(writers[p]:send(msg), "pair " .. p .. " msg " .. m .. " failed")
        end
    end

    assert(wait_until(function()
        for p = 1, PAIRS do
            if #bodies[p].got < #bodies[p].expected then return false end
        end
        return true
    end, 30), "ten-pair delivery incomplete")

    for p = 1, PAIRS do
        assert(bodies[p].got == bodies[p].expected,
            "pair " .. p .. " content mismatch: got " .. #bodies[p].got)
        readers[p]:close()
        writers[p]:close()
        os.remove(PATH_BASE .. "-p" .. p)
    end
end)

-- 2. Churn: 30 create → exchange → destroy cycles.
suite:test("open_close_churn", function()
    local cycles = 30
    for c = 1, cycles do
        local path = PATH_BASE .. "-churn" .. c
        local done = false
        local got = nil

        local reader = fifo.connect{
            name = path, rwmode = "r",
            onread = function(data)
                got = tostring(data)
                done = true
            end,
        }
        local writer = fifo.connect{
            name = path, rwmode = "w",
            onsendready = function() end,
            ondisconnected = function() end,
        }
        assert(writer:send("cycle-" .. c), "churn " .. c .. " send failed")
        assert(wait_until(function() return done end, 10),
            "churn " .. c .. " read incomplete")
        assert(got == "cycle-" .. c, "churn " .. c .. " content wrong")

        reader:close()
        writer:close()
        os.remove(path)
    end
    -- all nodes cleaned up
    for c = 1, cycles do
        local f = io.open(PATH_BASE .. "-churn" .. c, "rb")
        assert(not f, "churn node " .. c .. " leaked")
        if f then f:close() end
    end
end)

-- 3. One big stream while five small pairs stay active.
suite:test("big_stream_with_background_pairs", function()
    local big_path = PATH_BASE .. "-big"
    local big_body = ""

    local big_reader = fifo.connect{
        name = big_path, rwmode = "r",
        onread = function(data) big_body = big_body .. tostring(data) end,
    }
    local big_writer = fifo.connect{
        name = big_path, rwmode = "w",
        onsendready = function() end,
        ondisconnected = function() end,
    }

    -- background pairs
    local bg = {}
    for p = 1, 5 do
        local path = PATH_BASE .. "-bg" .. p
        bg[p] = { path = path, got = "" }
        bg[p].reader = fifo.connect{
            name = path, rwmode = "r",
            onread = function(data) bg[p].got = bg[p].got .. tostring(data) end,
        }
        bg[p].writer = fifo.connect{
            name = path, rwmode = "w",
            onsendready = function() end,
            ondisconnected = function() end,
        }
    end

    local PAYLOAD = string.rep("B", 1024 * 1024) -- 1MB
    -- interleave: big stream slices + background messages
    local sent = 0
    local slice_size = 16 * 1024
    local bg_round = 0
    while sent < #PAYLOAD do
        local slice = string.sub(PAYLOAD, sent + 1, sent + slice_size)
        local n = big_writer:send(slice)
        if n and n > 0 then
            sent = sent + n
            if n < #slice then
                local rest = string.sub(slice, n + 1)
                assert(wait_until(function()
                    local m = big_writer:send(rest)
                    if m and m > 0 then
                        sent = sent + m
                        rest = string.sub(rest, m + 1)
                        return #rest == 0
                    end
                    return false
                end, 10), "big stream drain stalled at " .. sent)
            end
        end
        bg_round = bg_round + 1
        local msg = "bg" .. bg_round .. "|"
        for p = 1, 5 do
            bg[p].expected = (bg[p].expected or "") .. msg
            bg[p].writer:send(msg)
        end
        fan.sleep(0.001)
    end

    assert(wait_until(function() return #big_body >= #PAYLOAD end, 30),
        "big stream incomplete: " .. #big_body)
    assert(big_body == PAYLOAD, "big stream content mismatch")

    for p = 1, 5 do
        assert(wait_until(function() return #(bg[p].got or "") >= #(bg[p].expected or "") end, 10),
            "background pair " .. p .. " incomplete")
        assert(bg[p].got == bg[p].expected, "background pair " .. p .. " content mismatch")
        bg[p].reader:close()
        bg[p].writer:close()
        os.remove(bg[p].path)
    end
    big_reader:close()
    big_writer:close()
    os.remove(big_path)
end)

-- Optional worker pool (LUAN_TEST_WORKERS=N); must precede fan.loop().
TestFramework.init_workers_from_env()

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
