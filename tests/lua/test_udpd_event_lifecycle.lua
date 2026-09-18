#!/usr/bin/env lua

-- Regression guard for UDP event lifecycle. The bug: read_ev/write_ev are not
-- atomically protected, so cleanup races with udpd_base_conn_request_send_ready
-- calling event_add on freed events. The child below provokes that race; a crash
-- there is a FAILURE (only a clean exit passes).

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("UDPD event lifecycle - race guard")

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
    return 25000 + (fan.getpid() % 1000) + math.random(1, 500)
end

local CRASH_SCRIPT_TEMPLATE = [[
local fan = require "fan"
local udpd = require "fan.udpd"

fan.loop(function()
    local conns = {}

    -- Create %d UDP sockets
    for i = 1, %d do
        local conn = udpd.new({
            bind_port = 0,
            onread = function(self, data, dest)
                -- May fire after close
            end,
            onsendready = function(self)
                -- May fire after close
            end,
        })
        if conn then
            table.insert(conns, conn)
        end
    end

    -- Hammer send_req/rebind/close in tight loops
    for round = 1, %d do
        for _, conn in ipairs(conns) do
            for k = 1, 50 do
                pcall(function() conn:send_req("x", "127.0.0.1", %d + k) end)
                if k %% 5 == 0 then
                    pcall(function() conn:rebind({bind_port = 0}) end)
                end
            end

            -- Close halfway through
            if round == math.floor(%d / 2) then
                pcall(function() conn:close() end)
                pcall(function() conn:close() end)  -- Double close
            end
        end
        collectgarbage("collect")
    end

    -- Close remaining
    for _, conn in ipairs(conns) do
        pcall(function() conn:close() end)
    end

    fan.sleep(0.3)
    collectgarbage("collect")
    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_udpd_event_add_after_free", function()
    local num_conns = 15
    local hammer_rounds = 3
    local base_port = alloc_port()

    local script = string.format(CRASH_SCRIPT_TEMPLATE,
        num_conns, num_conns, hammer_rounds, base_port, hammer_rounds)

    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(script)
    f:close()

    local cmd = TestFramework.child_lua_command(tmpfile, 15) .. "; echo $?"
    local handle = io.popen(cmd)
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    -- A crash is a FAILURE: the previous "signal => BUG CONFIRMED, pass" form
    -- made this suite unable to fail no matter what the child did. The helper
    -- also classifies the 128+signal exit codes the shell wrapper produces.
    TestFramework.assert_child_no_crash(output, success, exit_type, code,
        "UDP event lifecycle child (read_ev/write_ev UAF)")
end)

-- In-process light test
suite:test("rapid_new_close_gc", function()
    for i = 1, 30 do
        local conn = udpd.new({
            bind_port = 0,
            onread = function() end,
            onsendready = function() end,
        })
        if conn then
            pcall(function() conn:send_req("hello", "127.0.0.1", alloc_port()) end)
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
