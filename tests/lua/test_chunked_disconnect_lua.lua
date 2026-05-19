#!/usr/bin/env lua

-- Test that reply_chunk detects client disconnection (Lua httpd)

local fan = require "fan"
local httpd = require "fan.httpd.httpd"

local test_passed = false
local test_error = nil

fan.loop(function()
    local chunk_error = nil

    local server_info = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply_start(200, "OK")
            resp:reply_chunk("chunk1\n")

            -- Wait for client to disconnect
            fan.sleep(0.5)

            -- This should fail because client disconnected
            local ok, err = pcall(resp.reply_chunk, resp, "chunk2\n")
            if not ok then
                chunk_error = err
            end

            pcall(resp.reply_end, resp)
        end
    })

    if not server_info or not server_info.port or server_info.port == 0 then
        test_error = "server failed to bind or port is 0"
        fan.loopbreak()
        return
    end

    local port = server_info.port

    coroutine.wrap(function()
        local tcp = require "fan.connector.tcp"
        local apt = tcp.connect("127.0.0.1", port)
        apt.conn:send("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
        -- Read response until we have some data
        local data = apt:receive()
        if data and data:available() > 0 then
            data:GetString(data:available())
        end
        fan.sleep(0.1)
        local data2 = apt:receive()
        if data2 and data2:available() > 0 then
            data2:GetString(data2:available())
        end
        -- Close connection while server is still in fan.sleep(0.5)
        apt:close()
    end)()

    -- Wait for the server handler to complete
    local waited = 0
    while not chunk_error and waited < 3 do
        fan.sleep(0.1)
        waited = waited + 0.1
    end

    if chunk_error and tostring(chunk_error):find("connection closed") then
        test_passed = true
    else
        test_error = "expected 'connection closed by peer' error, got: " .. tostring(chunk_error)
    end

    fan.loopbreak()
end)

if test_passed then
    print("PASS: Lua httpd reply_chunk detects client disconnection")
    os.exit(0)
else
    print("FAIL: " .. (test_error or "unknown error"))
    os.exit(1)
end
