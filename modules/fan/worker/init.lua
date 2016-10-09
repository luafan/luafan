local fan = require "fan"
local pool = require "fan.pool"
local objectbuf = require "fan.objectbuf"
local connector = require "fan.connector"
require "compat53"

local function maxn(t)
  local n = 0
  for k,v in pairs(t) do
    if k > n then
      n = k
    end
  end

  return n
end

local master_mt = {}
master_mt.__index = function(obj, k)
  if obj.func_names[k] then
    return function(obj, ...)
      local slave = obj.slave_pool:pop()

      local task_key = string.format("%d", slave.task_index)
      slave.task_index = slave.task_index + 1
      local args = {task_key, k, ...}

      local output = stream.new()
      output:AddString(objectbuf.encode(args))

      if not slave:send(output:package()) then
        print("salve dead.")
        return
      end

      slave.task_map[task_key] = coroutine.running()
      return coroutine.yield()
    end
  end
end

local function new(funcmap, slavecount)
  local fifoname = connector.tmpfifoname()
  local url = "fifo:" .. fifoname

  local master = false
  local slave_pids = {}
  local slave_index
  local master_pid = fan.getpid()

  for i=1,slavecount do
    local pid = assert(fan.fork())
    master = pid > 0

    if not master then
      slave_index = i
      break
    else
      -- assert(fan.setpgid(pid, pid))
      table.insert(slave_pids, pid)
    end
  end

  if master then
    fan.setprogname(string.format("fan: master-%d (%d)", master_pid, slavecount))
    local obj = {slave_pool = pool.new(), slave_pids = slave_pids, func_names = {}}
    for k,v in pairs(funcmap) do
      obj.func_names[k] = k
    end

    setmetatable(obj, master_mt)

    obj.serv = connector.bind(url)

    obj.serv.onaccept = function(apt)
      -- print("onaccept", apt)
      apt.task_map = {}
      apt.task_index = 1
      obj.slave_pool:push(apt)

      apt.onread = function(input)
        while input:available() > 0 do
          local str = input:GetString()
          -- print("onread master", str)
          local args = objectbuf.decode(str)

          obj.slave_pool:push(apt)

          if apt.task_map[args[1]] then
            local running = apt.task_map[args[1]]
            apt.task_map[args[1]] = nil
            assert(coroutine.resume(running, table.unpack(args, 2, maxn(args))))
          end
        end
      end
    end

    return obj
  else
    local pid = fan.getpid()
    fan.setprogname(string.format("fan: slave-%d-%d", master_pid, slave_index))

    -- assert(fan.setpgid())

    -- for i=1,3 do
    -- assert(fan.close(i - 1))
    -- end
    --
    -- local f0 = fan.open("/dev/null")
    -- local f1 = fan.open("/dev/null")
    -- local f2 = fan.open("/dev/null")

    fan.loop(function()
        while not cli do
          fan.sleep(0.1)
          cli = connector.connect(url)
        end

        while true do
          local input = cli:receive()
          if not input then
            break
          end

          local str = input:GetString()
          -- print(pid, "onread slave", str)

          local args = objectbuf.decode(str)

          local task_key = args[1]
          local func = funcmap[args[2]]

          -- print(pid, "process", task_key, args[2], table.unpack(args, 3, maxn(args)))

          local ret = ""
          if func then
            local results = {task_key, func(table.unpack(args, 3, maxn(args))) }
            ret = objectbuf.encode(results)
          end

          local output = stream.new()
          output:AddString(ret)
          cli:send(output:package())
        end
      end)

    cli:close()
    cli = nil
    print(string.format("quiting pid=%d", pid))
    os.exit()
  end
end

return {
  new = new
}
