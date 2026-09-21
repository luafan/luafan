#!/usr/bin/env lua

-- Targeted reproducer for the TCPD bufferevent/evbuffer drain race.
-- The peer closes immediately while a worker client floods send/close/GC.

local fan = require("fan")
local tcpd = require("fan.tcpd")

local WORKERS = 2
local ROUNDS = 200
local CLIENTS = 80

if fan.workers_init then
    local ok, ret = pcall(fan.workers_init, WORKERS)
    if not ok or (ret ~= nil and ret ~= 0) then
        io.stderr:write("workers_init failed\n")
        os.exit(77)
    end
end

local server = tcpd.bind({
    host = "127.0.0.1",
    port = 0,
    worker = 1,
    onaccept = function(_, accept)
        if accept then
            accept:bind({
                onread = function(conn)
                    pcall(function() conn:close() end)
                end,
                ondisconnected = function() end,
            })
            pcall(function() accept:close() end)
        end
    end,
})

if not server then
    io.stderr:write("bind failed\n")
    os.exit(1)
end

local info = server:localinfo()
local port = info and info.port
if not port then
    io.stderr:write("localinfo failed\n")
    os.exit(1)
end

fan.loop(function()
    local live = {}
    for i = 1, CLIENTS do
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = port,
            worker = 0,
            read_timeout = 0.001,
            write_timeout = 0.001,
            onconnected = function(self)
                for _ = 1, ROUNDS do
                    pcall(function() self:send(string.rep("x", 4096)) end)
                end
                pcall(function() self:close() end)
            end,
            onread = function() end,
            onsendready = function() end,
            ondisconnected = function() end,
        })
        if ok and conn then
            live[i] = conn
        end
    end

    for round = 1, 20 do
        for i = 1, CLIENTS do
            local conn = live[i]
            if conn then
                pcall(function() conn:send(string.rep("y", 2048)) end)
                if round % 2 == 0 then
                    pcall(function() conn:close() end)
                end
            end
        end
        collectgarbage("collect")
        fan.sleep(0.002)
    end

    live = nil
    collectgarbage("collect")
    fan.sleep(0.2)
    pcall(function() server:close() end)
    fan.loopbreak()
end)

os.exit(0)
