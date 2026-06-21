#!/usr/bin/env lua
-- fan.popen basic examples
-- Run: lua examples/popen_basic.lua

package.preload['config'] = function() return { debug = false } end

local fan = require "fan"
local popen = require "fan.popen"
_G.fan = fan

-- Example 1: Simple command, capture output
local function example_echo()
    print("=== Example 1: echo ===")
    fan.loop(function()
        popen.spawn({
            command = {"echo", "hello from subprocess"},
            onread = function(data)
                print("stdout:", data:gsub("\n$", ""))
            end,
            ondisconnected = function(msg, code)
                print("exit code:", code)
                fan.loopbreak()
            end,
        })
    end)
end

-- Example 2: Bidirectional with cat (echo server)
local function example_cat()
    print("\n=== Example 2: cat roundtrip ===")
    fan.loop(function()
        local proc = popen.spawn({
            command = {"cat"},
            onread = function(data)
                print("echoed:", data:gsub("\n$", ""))
            end,
            ondisconnected = function()
                fan.loopbreak()
            end,
        })

        proc:send("line one\n")
        proc:send("line two\n")
        proc:send("line three\n")

        fan.sleep(0.5)
        proc:close_stdin()  -- triggers EOF, cat exits
    end)
end

-- Example 3: stderr capture
local function example_stderr()
    print("\n=== Example 3: stderr ===")
    fan.loop(function()
        popen.spawn({
            command = {"bash", "-c", "echo normal; echo error >&2"},
            onread = function(data)
                print("stdout:", data:gsub("\n$", ""))
            end,
            onstderr = function(data)
                print("stderr:", data:gsub("\n$", ""))
            end,
            ondisconnected = function(msg, code)
                print("exit code:", code)
                fan.loopbreak()
            end,
        })
    end)
end

-- Example 4: Non-zero exit code
local function example_exit_code()
    print("\n=== Example 4: exit code ===")
    fan.loop(function()
        popen.spawn({
            command = {"bash", "-c", "exit 42"},
            onread = function() end,
            ondisconnected = function(msg, code)
                print("exit code:", code)
                assert(code == 42)
                fan.loopbreak()
            end,
        })
    end)
end

-- Example 5: Multiple concurrent processes
local function example_concurrent()
    print("\n=== Example 5: concurrent ===")
    fan.loop(function()
        local done = 0
        for i = 1, 3 do
            local idx = i
            popen.spawn({
                command = {"bash", "-c", string.format("echo proc-%d; sleep %d; echo done-%d", idx, idx, idx)},
                onread = function(data)
                    print(string.format("[proc-%d] %s", idx, data:gsub("\n$", "")))
                end,
                ondisconnected = function(msg, code)
                    done = done + 1
                    if done == 3 then
                        fan.loopbreak()
                    end
                end,
            })
        end
    end)
end

-- Example 6: Using the connector wrapper (coroutine-based send/receive)
local function example_connector()
    print("\n=== Example 6: connector send/receive ===")
    local connector = require "fan.connector.popen"

    fan.loop(function()
        local proc = connector.spawn({
            command = {"cat"},
        })

        proc:send("hello from connector\n")

        fan.sleep(0.3)

        local data = proc:receive(1)
        if data then
            local str = data:GetBytes(data:available())
            print("received:", str:gsub("\n$", ""))
        end

        proc:close()
        fan.loopbreak()
    end)
end

-- Run all examples sequentially
example_echo()
example_cat()
example_stderr()
example_exit_code()
example_concurrent()
example_connector()

print("\n=== All examples completed ===")
