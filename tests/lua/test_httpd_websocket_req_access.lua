#!/usr/bin/env lua

-- Regression test for problem 10: WebSocket API accessing request->req after accept
-- After websocket_accept, some API paths still depend on request->req
-- which may have been freed.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("WebSocket API request->req access after accept")

local SERVER_PORT = 22112

-- Subprocess: WebSocket server + clients doing rapid accept/send/close
local CRASH_SCRIPT = [[
local fan = require "fan"
local httpd = require "fan.httpd"

local PORT = ]] .. SERVER_PORT .. [[

fan.loop(function()
    local server = httpd.bind({
        host = "0.0.0.0",
        port = PORT,
        onrequest = function(req)
            local ok, is_ws = pcall(function() return req:is_websocket_upgrade() end)
            if ok and is_ws then
                -- Accept the WebSocket
                pcall(function()
                    return req:websocket_accept({
                        onmessage = function(ws_req, data)
                            -- Echo back
                            pcall(function() ws_req:websocket_send(data) end)
                        end,
                        onclose = function(ws_req)
                            -- Try to use req after close — this is the risky pattern
                            pcall(function() ws_req:websocket_state() end)
                            pcall(function() ws_req:websocket_send("late data") end)
                        end,
                    })
                end)
            else
                pcall(function() req:respond(200, {}, "ok") end)
            end
        end,
    })

    if not server then
        print("Failed to bind server")
        os.exit(1)
    end

    -- Run for a short time inside the loop coroutine
    fan.sleep(0.5)
    fan.loopbreak()
end)

os.exit(0)
]]

suite:test("subprocess_websocket_req_after_accept", function()
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(CRASH_SCRIPT)
    f:close()

    local cmd = TestFramework.child_lua_command(tmpfile, 15) .. "; echo $?"
    local handle = io.popen(cmd .. " 2>&1")
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    -- A crash is a FAILURE: the previous "signal => pass" form made this suite
    -- unable to fail no matter what the child did.
    TestFramework.assert_child_no_crash(output, success, exit_type, code,
        "WebSocket req->req access child")
end)

-- In-process: verify WebSocket module loads
suite:test("websocket_module_loads", function()
    local ok, httpd = pcall(require, "fan.httpd")
    TestFramework.assert_true(ok)
end)

local failed = TestFramework.run_suite(suite)
os.exit(failed > 0 and 1 or 0)
