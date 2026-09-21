#!/usr/bin/env lua

-- Targeted reproducer: deferred read/write callbacks overlap peer EOF cleanup.

local fan = require("fan")
local tcpd = require("fan.tcpd")

local WORKERS = 3
local CLIENTS = 120
local LOOPS = 80

if fan.workers_init then
    local ok, ret = pcall(fan.workers_init, WORKERS)
    if not ok or (ret ~= nil and ret ~= 0) then
        os.exit(77)
    end
end

local payload = string.rep("z", 32768)
local server = tcpd.bind({
    host = "127.0.0.1",
    port = 0,
    worker = 1,
    onaccept = function(_, accept)
        if not accept then return end
        accept:bind({
            onread = function(conn)
                for _ = 1, 3 do
                    pcall(function() conn:send(payload) end)
                end
                pcall(function() conn:close() end)
            end,
            ondisconnected = function() end,
        })
        for _ = 1, 3 do
            pcall(function() accept:send(payload) end)
        end
        pcall(function() accept:close() end)
    end,
})

assert(server, "bind failed")
local info = server:localinfo()
local port = info and info.port
assert(port, "localinfo failed")

fan.loop(function()
    local conns = {}
    for i = 1, CLIENTS do
        local ok, conn = pcall(tcpd.connect, {
            host = "127.0.0.1",
            port = port,
            worker = i % WORKERS,
            read_timeout = 0.001,
            write_timeout = 0.001,
            onconnected = function(self)
                for _ = 1, LOOPS do
                    pcall(function() self:send(payload) end)
                end
            end,
            onread = function(self)
                for _ = 1, 4 do
                    pcall(function() self:send("r") end)
                    pcall(function() self:close() end)
                end
            end,
            onsendready = function(self)
                pcall(function() self:send("w") end)
                pcall(function() self:close() end)
            end,
            ondisconnected = function(self)
                pcall(function() self:close() end)
                pcall(function() self:send("d") end)
            end,
        })
        if ok and conn then conns[i] = conn end
    end

    for round = 1, 50 do
        for i = 1, CLIENTS do
            local conn = conns[i]
            if conn then
                pcall(function() conn:send("x") end)
                if round % 3 == 0 then pcall(function() conn:close() end) end
            end
        end
        collectgarbage("collect")
        fan.sleep(0.001)
    end
    conns = nil
    collectgarbage("collect")
    fan.sleep(0.5)
    pcall(function() server:close() end)
    fan.loopbreak()
end)

os.exit(0)
