APIs
====

## Goals
Connector is used to create tcp/udp/fifo connection quickly with same interface. It simulate block api call for tcp/fifo, so it's easy for you to write functions without callback hell.

* `cli = connector.connect(url)`

 create a logic connection to fifo/tcp/udp

* `serv = connector.bind(url)`

bind a listener of fifo/tcp/udp

* `filename = connector.tmpfifoname()`

create a temporary fifo file that will not conflict with others.

URL_Format
==========

* `fifo:<file path>`
* `tcp://host:port`
* `udp://host:port`

CLI_APIs
========

* `cli:send(buf)` (fifo/tcp)

yield until buf sent, if `#buf` is too big, it will be divided to parts (fifo `MAX_LINE_SIZE = 8192`).

* `stream = cli:receive(expect_length?)` (fifo/tcp)

yield to wait for expect data ready for read, return the read stream ([fan.stream](stream.md)) on read ready, the default expect_length is 1.

* `cli:close()`(fifo/tcp)

close this connection.

* `cli:send(buf)` (udp with embedded private protocol)

send buf on background, using embedded private protocol to control squence, buf size limit around 65536 * (MTU payload len), support 33.75MB (65536 * (576 - 8 - 20 - 8)) over internet at last.

* `cli.onread = function(buf) end` (udp with embedded private protocol)

input buffer callback, using embedded private protocol to control squence, when all parts of the buffer received, this callback will be invoked.

SERV
====

* `serv.onaccept = function(cli) end`

Samples
=======

* listen on fifo file, and service echo request.

```lua
local fan = require "fan"
local connector = require "fan.connector"
local utils = require "fan.utils"

local fifoname = connector.tmpfifoname()
local url = "fifo:" .. fifoname

local data = string.rep("abc", 1024)

local co = coroutine.create(function()
    serv = connector.bind(url)
    serv.onaccept = function(apt)
        -- apt.simulate_send_block = false
        print("onaccept")
        while true do
            local input = apt:receive()
            if not input then
                break
            end

            local buf = input:GetBytes()
            apt:send(buf)
        end
    end
end)
print(coroutine.resume(co))

fan.loop(function()
    cli = connector.connect(url)
    -- cli.simulate_send_block = false
    print(cli:send(data))

    local count = 0
    local last_count = 0
    local last_time = utils.gettime()

    coroutine.wrap(function()
        while true do
          fan.sleep(2)
          print(string.format("count=%d speed=%1.03f", count, (count - last_count) / (utils.gettime() - last_time)))
          last_time = utils.gettime()
          last_count = count
        end
    end)()

    while true do
        local input = cli:receive()
        if not input then
            break
        end

        local buf = input:GetBytes()
        count = count + 1

        cli:send(data)
     end
end)
```

* listen on tcp port, and service echo request.

```lua
local fan = require "fan"
local connector = require "fan.connector"
local utils = require "fan.utils"

local co = coroutine.create(function()
    serv = connector.bind("tcp://127.0.0.1:10000")
    serv.onaccept = function(apt)
        -- apt.simulate_send_block = false
        print("onaccept")
        while true do
            local input = apt:receive()
            if not input then
                break
            end

            local buf = input:GetBytes()
            apt:send(buf)
        end
    end
end)
coroutine.resume(co)

fan.loop(function()
    cli = connector.connect("tcp://127.0.0.1:10000")
    -- cli.simulate_send_block = false
    cli:send("hi")

    coroutine.wrap(function()
        while true do
            fan.sleep(1)
            cli:send("noise")
        end
        end)()

    local count = 0
    local last_count = 0
    local last_time = utils.gettime()

    coroutine.wrap(function()
        while true do
          fan.sleep(2)
          print(string.format("count=%d speed=%1.03f", count, (count - last_count) / (utils.gettime() - last_time)))
          last_time = utils.gettime()
          last_count = count
        end
    end)()

    while true do
        local input = cli:receive()
        if not input then
            break
        end

        -- print("client read", input)
        input:GetBytes()
        count = count + 1

        cli:send(os.date())
     end
end)
```
