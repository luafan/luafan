APIs
====

* `worker.new(funcmap, slavecount, max_job_count, url)`
	* `funcmap` defines tasks to be run on slave process.


	* `slavecount` defines number of slave process to start, each slave process will bind to one cpu core with affinity(if support).


	* `max_job_count` defines number of task per slave can run.

	* `url` defines how does slave connect to master, can be fifo url or tcp url, if not set, default fifo tunnel is created.

Samples
=======

* master/slave process on same host with fifo tunnel.

```lua
local fan = require "fan"
local utils = require "fan.utils"
local worker = require "fan.worker"
local md5 = require "md5"

local commander = worker.new({
    ["test"] = function(x, y)
      local d = md5.new()
      d:update(y)
      return y, d:digest()
    end
  }, tonumber(arg[1] or 3), tonumber(arg[2] or 1)) --  "tcp://127.0.0.1:10000"

fan.loop(function()
    commander.wait_all_slaves()

    local count = 0
    local last_count = 0
    local last_time = utils.gettime()


    for i=1,20 do
      local co = coroutine.create(function()
          local data = string.rep("abc", 1024)
          while true do
            local status, x, y = commander:test(1000, data)
            if status then
              assert(x == data)
              assert(y == "3437d873715e86d96e373929b63a3b28")
              count = count + 1
            else
              print(x)
            end
            count = count + 1
          end
        end)
      local st,msg = coroutine.resume(co)
      if not st then
        print(msg)
      else
        print("started")
      end
    end

    while true do
      fan.sleep(2)
      print(string.format("count=%d speed=%1.03f", count, (count - last_count) / (utils.gettime() - last_time)))
      last_time = utils.gettime()
      last_count = count
      for i,v in ipairs(commander.slaves) do
        print(i, v.task_index, v.status, v.jobcount)
      end
    end
  end)

print("quit master")

```

* master & slave on different host.

slave:

```lua
local fan = require "fan"
local utils = require "fan.utils"
local worker = require "fan.worker"
local md5 = require "md5"

local commander = worker.new({
    ["test"] = function(x, y)
      local d = md5.new()
      d:update(y)
      return y, d:digest()
    end
  }, tonumber(arg[1] or 1), tonumber(arg[2] or 10), "tcp://127.0.0.1:10000")

fan.loop(function()
  while true do
    fan.sleep(60)
  end
end)
print("quit")
```

master:

```lua
local fan = require "fan"
local utils = require "fan.utils"
local worker = require "fan.worker"

local commander = worker.new({
    ["test"] = function(x, y)
      error("you should not see this as slave count is 0")
    end
  }, 0, tonumber(arg[1] or 10), "tcp://127.0.0.1:10000")

fan.loop(function()
    local count = 0
    local last_count = 0
    local last_time = utils.gettime()

    for i=1,20 do
      local co = coroutine.create(function()
          local data = string.rep("abc", 1024)
          while true do
            local status, x, y = commander:test(1000, data)
            if status then
              assert(x == data)
              assert(y == "3437d873715e86d96e373929b63a3b28")
              count = count + 1
            else
              print(x)
            end
          end
        end)
      local st,msg = coroutine.resume(co)
      if not st then
        print(msg)
      else
        print("started")
      end
    end

    while true do
      fan.sleep(2)
      print(string.format("count=%d speed=%1.03f", count, (count - last_count) / (utils.gettime() - last_time)))
      last_time = utils.gettime()
      last_count = count
      for i,v in ipairs(commander.slaves) do
        print(i, v.task_index, v.status, v.jobcount)
      end
    end
  end)

print("quit master")
```
