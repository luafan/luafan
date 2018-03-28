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

send buf on background, using embedded private protocol to control squence, buf size limit around 65536 * (MTU payload len), support 33.75MB (65536 * (576 - 8 - 20 - 8)) over internet at least.

* `cli.onread = function(cli, buf) end` (udp with embedded private protocol)

input buffer callback, using embedded private protocol to control squence, when all parts of the buffer received, this callback will be invoked.

* `cli.onsent = function(cli, package) end` (udp with embedded private protocol)

output buffer sent callback, when all parts of the buffer have been sent, this callback will be invoked.

* `cli.ontimeout = function(cli, package) end` (udp with embedded private protocol)

output buffer timeout callback, if callback return false, the package will be dropped, otherwise, the package will be resend.

SERV
====

* `serv.onaccept = function(cli) end`

Samples
=======

## benchmark server

```
Linux XXXXXX 4.4.0-70-generic #91-Ubuntu SMP Wed Mar 22 12:47:43 UTC 2017 x86_64 x86_64 x86_64 GNU/Linux

Intel(R) Core(TM) i7-2640M CPU @ 2.80GHz

MemTotal:        3922816 kB
```

* listen on fifo file, and service echo request.

  * benchmark result: 59k/s avg.

```
count=118723 speed=59361.918
count=237557 speed=59417.623
count=356271 speed=59357.149
count=475312 speed=59521.302
count=593905 speed=59297.002
count=711984 speed=59040.119
count=830336 speed=59176.649
count=949218 speed=59441.624
count=1067916 speed=59349.566
```

  * code

```lua
local fan = require "fan"
local connector = require "fan.connector"
local utils = require "fan.utils"

local fifoname = connector.tmpfifoname()
local url = "fifo:" .. fifoname

local data = string.rep("abc", 1) -- change 1024 to 1 to test ping/pong performance.

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

  * benchmark result, 41k/s avg.

```
count=75811 speed=37907.827
count=163627 speed=43908.398
count=253310 speed=44841.976
count=339962 speed=43325.525
count=419818 speed=39929.276
count=509269 speed=44725.767
count=593292 speed=42011.710
count=678407 speed=42557.688
count=761588 speed=41590.897
count=850561 speed=44486.792
```

  * code

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

    local data = os.date()

    while true do
        local input = cli:receive()
        if not input then
            break
        end

        -- print("client read", input)
        input:GetBytes()
        count = count + 1

        cli:send(data)
     end
end)
```

* connector.udp io benchmark

  * iftop result: 190 MB/s

```
                373Mb         745Mb          1.09Gb        1.46Gb
  └─────────────┴─────────────┴──────────────┴─────────────┴──────────────
  localhost              => localhost              1.54Gb  1.53Gb  1.51Gb
                         <=                           0b      0b      0b

  ────────────────────────────────────────────────────────────────────────
  TX:             cum:   26.4GB   peak:   rates:   1.54Gb  1.53Gb  1.51Gb
  RX:                       0B               0b       0b      0b      0b
  TOTAL:                 26.4GB           1.54Gb   1.54Gb  1.53Gb  1.51Gb
```

  * code

```lua
local fan = require "fan"
local config = require "config"

-- override config settings.
config.udp_mtu = 8192
config.udp_waiting_count = 10
config.udp_package_timeout = 3

local utils = require "fan.utils"
local connector = require "fan.connector"

if fan.fork() > 0 then
  local longstr = string.rep("abc", 102400)
  print(#(longstr))

  fan.loop(function()
      fan.sleep(1)
      cli = connector.connect("udp://127.0.0.1:10000")

      local count = 0
      local last_count = 0
      local last_time = utils.gettime()

      -- coroutine.wrap(function()
      --     while true do
      --       fan.sleep(2)
      --       print(string.format("count=%d speed=%1.03f", count, (count - last_count) / (utils.gettime() - last_time)))
      --       last_time = utils.gettime()
      --       last_count = count
      --     end
      -- end)()

      cli.onread = function(apt, body)
        count = count + 1
        -- print("cli onread", #(body))
        -- assert(body == longstr)
        -- local start = utils.gettime()
        cli:send(longstr)
        -- print(utils.gettime() - start)
      end

      cli:send(longstr)
    end)
else
  local co = coroutine.create(function()
      serv = connector.bind("udp://127.0.0.1:10000")
      serv.onaccept = function(apt)
        print("onaccept")
        apt.onread = function(apt, body)
          -- print("apt onread", #(body))
          apt:send(body)
        end
      end
    end)
  assert(coroutine.resume(co))

  fan.loop()

end
```

* connector.tcp io benchmark

* iftop result: 338 MB/s

```
              369Mb         738Mb          1.08Gb        1.44Gb   1.80Gb
└─────────────┴─────────────┴──────────────┴─────────────┴──────────────
localhost              => localhost              2.71Gb  2.63Gb  2.64Gb
                       <=                           0b      0b      0b

────────────────────────────────────────────────────────────────────────
TX:             cum:   25.4GB   peak:   rates:   2.71Gb  2.63Gb  2.64Gb
RX:                       0B               0b       0b      0b      0b
TOTAL:                 25.4GB           2.71Gb   2.71Gb  2.63Gb  2.64Gb
```

* code

```lua
local fan = require "fan"
local stream = require "fan.stream"
local connector = require "fan.connector"

local data = string.rep([[coroutine.create]], 1000)

local co = coroutine.create(function()
    serv = connector.bind("tcp://127.0.0.1:10000")
    serv.onaccept = function(apt)
      print("onaccept")
      local last_expect = 1

      while true do
        local input = apt:receive(last_expect)
        if not input then
          break
        end
        -- print("serv read", input:available())

        local str,expect = input:GetString()
        if str then
          last_expect = 1

          assert(str == data)

          local d = stream.new()
          d:AddString(str)
          apt:send(d:package())
        else
          last_expect = expect
        end
      end
    end
  end)
coroutine.resume(co)

fan.loop(function()
    cli = connector.connect("tcp://127.0.0.1:10000")
    local d = stream.new()
    d:AddString(data)
    cli:send(d:package())

    local last_expect = 1

    while true do
      local input = cli:receive(last_expect)
      if not input then
        break
      end
      -- print("cli read", input:available())

      local str,expect = input:GetString()
      if str then
        last_expect = 1

        assert(str == data)

        local d = stream.new()
        d:AddString(str)
        cli:send(d:package())
      else
        last_expect = expect
      end
    end
  end)
```
