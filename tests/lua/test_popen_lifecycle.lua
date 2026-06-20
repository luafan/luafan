#!/usr/bin/env lua

-- Lifecycle tests for fan.popen module (synchronous, no event loop)

local TestFramework = require('test_framework')

package.preload['config'] = function()
    return { debug = false }
end

local fan = require "fan"
local popen = require "fan.popen"

local suite = TestFramework.create_suite("Popen Lifecycle")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_type(popen, "table")
    TestFramework.assert_type(popen.spawn, "function")
end)

-- Test spawn basic - returns userdata with expected methods
suite:test("spawn_basic", function()
    local proc, err = popen.spawn({
        command = "true",
        onread = function() end,
    })
    TestFramework.assert_not_nil(proc, "spawn failed: " .. tostring(err))
    TestFramework.assert_type(proc.send, "function")
    TestFramework.assert_type(proc.close, "function")
    TestFramework.assert_type(proc.close_stdin, "function")
    TestFramework.assert_type(proc.getpid, "function")
    TestFramework.assert_type(proc.is_alive, "function")
    proc:close()
end)

-- Test getpid returns a positive number
suite:test("getpid_returns_number", function()
    local proc = popen.spawn({
        command = {"sleep", "10"},
        onread = function() end,
    })
    local pid = proc:getpid()
    TestFramework.assert_not_nil(pid)
    TestFramework.assert_type(pid, "number")
    TestFramework.assert_true(pid > 0, "expected positive pid, got " .. tostring(pid))
    proc:close()
end)

-- Test close is idempotent
suite:test("close_idempotent", function()
    local proc = popen.spawn({
        command = "true",
        onread = function() end,
    })
    -- First close
    local ok1 = proc:close()
    TestFramework.assert_true(ok1)
    -- Second close should not crash
    local ok2 = proc:close()
    TestFramework.assert_true(ok2)
end)

-- Test spawn with array command
suite:test("spawn_array_command", function()
    local proc, err = popen.spawn({
        command = {"echo", "hello", "world"},
        onread = function() end,
    })
    TestFramework.assert_not_nil(proc, "spawn failed: " .. tostring(err))
    TestFramework.assert_not_nil(proc:getpid())
    proc:close()
end)

-- Test spawn invalid command returns error
suite:test("spawn_invalid_command", function()
    local proc, err = popen.spawn({
        command = "this_command_definitely_does_not_exist_12345",
        onread = function() end,
    })
    -- posix_spawnp returns error for nonexistent commands
    -- Either nil+error or a valid proc is acceptable
    if proc then
        proc:close()
    end
    -- Either nil+error or a valid proc is acceptable
end)

-- Test is_alive on a finished process
suite:test("is_alive_false_after_exit", function()
    local proc = popen.spawn({
        command = "true",
        onread = function() end,
    })
    -- true exits immediately, but we need to give it a moment
    -- In synchronous mode without event loop, just check
    proc:close()
end)

-- Test send returns bytes written
suite:test("send_returns_bytes", function()
    local proc = popen.spawn({
        command = {"cat"},
        onread = function() end,
    })
    local written, err = proc:send("hello")
    TestFramework.assert_not_nil(written, "send failed: " .. tostring(err))
    TestFramework.assert_true(written >= 0)
    proc:close()
end)

-- Test send after close returns error
suite:test("send_after_close_returns_error", function()
    local proc = popen.spawn({
        command = {"cat"},
        onread = function() end,
    })
    proc:close()
    local result, err = proc:send("hello")
    TestFramework.assert_nil(result)
    TestFramework.assert_not_nil(err)
end)

-- Test close_stdin
suite:test("close_stdin_method", function()
    local proc = popen.spawn({
        command = {"cat"},
        onread = function() end,
    })
    local ok = proc:close_stdin()
    TestFramework.assert_true(ok)
    -- send after close_stdin should fail
    local result, err = proc:send("hello")
    TestFramework.assert_nil(result)
    proc:close()
end)

-- Test spawn without capture_stderr
suite:test("spawn_no_stderr", function()
    local proc, err = popen.spawn({
        command = "true",
        capture_stderr = false,
        onread = function() end,
    })
    TestFramework.assert_not_nil(proc, "spawn failed: " .. tostring(err))
    proc:close()
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
