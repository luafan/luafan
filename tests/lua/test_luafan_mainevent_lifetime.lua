#!/usr/bin/env lua

-- Regression guard for luafan_start mainevent double-free.
-- The bug: main_handler frees mainevent but doesn't set it to NULL, so an event
-- that fires abnormally (or a failed nested-call detection) double-frees it.
-- The child below provokes that path; a crash there is a FAILURE (only a clean
-- exit passes).

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("luafan_start mainevent lifetime - double-free guard")

-- Subprocess that attempts to trigger mainevent double-free
local CRASH_SCRIPT = [[
local fan = require "fan"

-- Scenario 1: rapid sequential fan.loop calls
for i = 1, 10 do
    fan.loop(function()
        fan.loopbreak()
    end)
end

-- Scenario 2: nested call attempt (should be handled gracefully, not crash)
fan.loop(function()
    pcall(fan.loop, function()
        fan.loopbreak()
    end)
    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_double_free_mainevent_on_rapid_loops", function()
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(CRASH_SCRIPT)
    f:close()

    local cmd = TestFramework.child_lua_command(tmpfile, 10) .. "; echo $?"
    local handle = io.popen(cmd .. " 2>&1")
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    -- A crash is a FAILURE: the previous "signal => pass" form made this suite
    -- unable to fail no matter what the child did.
    TestFramework.assert_child_no_crash(output, success, exit_type, code,
        "luafan_start mainevent child")
end)

-- In-process tests (non-crash scenarios)
suite:test("basic_loop_cycle", function()
    local ran = false
    fan.loop(function()
        ran = true
        fan.loopbreak()
    end)
    TestFramework.assert_true(ran)
end)

suite:test("sequential_loop_cycles", function()
    for i = 1, 3 do
        print(string.format("sequential_loop_cycles: starting cycle %d", i))
        local ran = false
        fan.loop(function()
            ran = true
            fan.loopbreak()
        end)
        print(string.format("sequential_loop_cycles: completed cycle %d (ran=%s)", i, tostring(ran)))
        TestFramework.assert_true(ran, "loop cycle " .. i .. " did not run")
    end
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
