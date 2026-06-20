#!/usr/bin/env lua

-- Integration tests for fan.popen module (async, uses fan.loop)

local TestFramework = require('test_framework')

package.preload['config'] = function()
    return { debug = false }
end

local fan = require "fan"
_G.fan = fan
local popen = require "fan.popen"

local suite = TestFramework.create_suite("Popen Integration")

-- Test: echo roundtrip with cat
suite:test("echo_roundtrip", TestFramework.async_test(function()
    local received = nil

    local proc = popen.spawn({
        command = {"cat"},
        onread = function(data)
            received = data
        end,
        ondisconnected = function() end,
    })
    TestFramework.assert_not_nil(proc)

    proc:send("hello world\n")
    fan.sleep(0.5)

    TestFramework.assert_not_nil(received)
    TestFramework.assert_match(received, "hello world")
    proc:close()
end))

-- Test: stderr capture
suite:test("stderr_capture", TestFramework.async_test(function()
    local stdout_data = ""
    local stderr_data = ""

    local proc = popen.spawn({
        command = {"bash", "-c", "echo out; echo err >&2"},
        onread = function(data)
            stdout_data = stdout_data .. data
        end,
        onstderr = function(data)
            stderr_data = stderr_data .. data
        end,
        ondisconnected = function() end,
    })
    TestFramework.assert_not_nil(proc)

    fan.sleep(0.5)

    TestFramework.assert_match(stdout_data, "out")
    TestFramework.assert_match(stderr_data, "err")
    proc:close()
end))

-- Test: exit detection with exit code 0
suite:test("exit_detection_zero", TestFramework.async_test(function()
    local exit_code = nil

    local proc = popen.spawn({
        command = {"true"},
        onread = function() end,
        ondisconnected = function(msg, code)
            exit_code = code
        end,
    })
    TestFramework.assert_not_nil(proc)

    fan.sleep(0.5)

    TestFramework.assert_not_nil(exit_code)
    TestFramework.assert_equal(exit_code, 0)
end))

-- Test: exit with non-zero code
suite:test("exit_nonzero", TestFramework.async_test(function()
    local exit_code = nil

    local proc = popen.spawn({
        command = {"bash", "-c", "exit 42"},
        onread = function() end,
        ondisconnected = function(msg, code)
            exit_code = code
        end,
    })
    TestFramework.assert_not_nil(proc)

    fan.sleep(0.5)

    TestFramework.assert_not_nil(exit_code)
    TestFramework.assert_equal(exit_code, 42)
end))

-- Test: kill child process
suite:test("kill_child", TestFramework.async_test(function()
    local proc = popen.spawn({
        command = {"sleep", "100"},
        onread = function() end,
    })
    TestFramework.assert_not_nil(proc)
    TestFramework.assert_true(proc:is_alive())

    proc:close()
    -- After close, child should be dead
    TestFramework.assert_false(proc:is_alive())
end))

-- Test: large output
suite:test("large_output", TestFramework.async_test(function()
    local total_received = 0
    local expected = 100 * 1024  -- 100KB

    local proc = popen.spawn({
        command = {"bash", "-c", "dd if=/dev/zero bs=1024 count=100 2>/dev/null"},
        onread = function(data)
            total_received = total_received + #data
        end,
        ondisconnected = function() end,
    })
    TestFramework.assert_not_nil(proc)

    fan.sleep(2)

    TestFramework.assert_true(total_received >= expected,
        string.format("expected >= %d bytes, got %d", expected, total_received))
    proc:close()
end))

-- Test: MCP protocol simulation (JSON-RPC over stdio via cat echo)
suite:test("mcp_protocol_simulation", TestFramework.async_test(function()
    local received = ""

    local proc = popen.spawn({
        command = {"cat"},
        onread = function(data)
            received = received .. data
        end,
        ondisconnected = function() end,
    })
    TestFramework.assert_not_nil(proc)

    -- Send a JSON-RPC initialize message
    local msg = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0.1"}}}\n'
    proc:send(msg)

    fan.sleep(0.5)

    -- cat echoes back, so received should contain our message
    TestFramework.assert_match(received, '"jsonrpc"')
    TestFramework.assert_match(received, '"initialize"')
    TestFramework.assert_match(received, '"2024%-11%-05"')
    proc:close()
end))

-- Test: stdin close triggers EOF for child
suite:test("stdin_write_close", TestFramework.async_test(function()
    local received = ""
    local disconnected = false

    local proc = popen.spawn({
        command = {"cat"},
        onread = function(data)
            received = received .. data
        end,
        ondisconnected = function(msg, code)
            disconnected = true
        end,
    })
    TestFramework.assert_not_nil(proc)

    proc:send("test data\n")
    proc:close_stdin()

    fan.sleep(0.5)

    TestFramework.assert_match(received, "test data")
    TestFramework.assert_true(disconnected, "expected disconnect after stdin close")
end))

-- Test: concurrent processes
suite:test("concurrent_processes", TestFramework.async_test(function()
    local results = {}

    local procs = {}
    for i = 1, 3 do
        local idx = i
        local proc = popen.spawn({
            command = {"bash", "-c", "echo proc" .. idx},
            onread = function(data)
                results[idx] = data
            end,
            ondisconnected = function() end,
        })
        TestFramework.assert_not_nil(proc, "spawn " .. idx .. " failed")
        procs[idx] = proc
    end

    fan.sleep(0.5)

    for i = 1, 3 do
        TestFramework.assert_not_nil(results[i], "no data from proc " .. i)
        TestFramework.assert_match(results[i], "proc" .. i)
        procs[i]:close()
    end
end))

-- Test: connector wrapper (fan.connector.popen)
suite:test("connector_roundtrip", TestFramework.async_test(function()
    local connector = require "fan.connector.popen"

    local proc, err = connector.spawn({
        command = {"cat"},
    })
    TestFramework.assert_not_nil(proc, "connector spawn failed: " .. tostring(err))

    proc:send("connector test\n")

    fan.sleep(0.5)

    local data = proc:receive(1)
    TestFramework.assert_not_nil(data, "no data received from connector")
    TestFramework.assert_true(data:available() > 0, "received stream is empty")
    proc:close()
end))

-- Test: multiple sends before read
suite:test("multiple_sends", TestFramework.async_test(function()
    local received = ""

    local proc = popen.spawn({
        command = {"cat"},
        onread = function(data)
            received = received .. data
        end,
        ondisconnected = function() end,
    })
    TestFramework.assert_not_nil(proc)

    proc:send("line1\n")
    proc:send("line2\n")
    proc:send("line3\n")

    fan.sleep(0.5)

    TestFramework.assert_match(received, "line1")
    TestFramework.assert_match(received, "line2")
    TestFramework.assert_match(received, "line3")
    proc:close()
end))

-- Test: process that produces no output
suite:test("no_output_process", TestFramework.async_test(function()
    local read_called = false
    local disconnected = false

    local proc = popen.spawn({
        command = {"true"},
        onread = function(data)
            read_called = true
        end,
        ondisconnected = function(msg, code)
            disconnected = true
        end,
    })
    TestFramework.assert_not_nil(proc)

    fan.sleep(0.5)

    TestFramework.assert_false(read_called, "onread should not be called for no output")
    TestFramework.assert_true(disconnected, "expected disconnect notification")
end))

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
