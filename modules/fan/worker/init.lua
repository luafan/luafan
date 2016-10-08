local fan = require "fan"
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
      local args = {k, ...}
      local slave = next(obj.slaves) -- TODO slave pool impl
      local output = stream.new()
      output:AddString(objectbuf.encode(args))

      if not slave:send(output:package()) then
        obj.slaves[slave] = nil
        return
      end

      slave.master_running = coroutine.running()
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
    local obj = {slaves = {}, slave_pids = slave_pids, func_names = {}}
    for k,v in pairs(funcmap) do
      obj.func_names[k] = k
    end

    setmetatable(obj, master_mt)

    obj.serv = connector.bind(url)

    obj.serv.onaccept = function(apt)
      print("onaccept", apt)
      obj.slaves[apt] = apt

      apt.onread = function(input)
        local str = input:GetString()
        -- print("onread master", str)
        local args = objectbuf.decode(str)

        if apt.master_running then
          local master_running = apt.master_running
          apt.master_running = nil
          coroutine.resume(master_running, table.unpack(args, 1, maxn(args)))
        end
      end
    end

    return obj
  else
    local pid = fan.getpid()
    fan.setprogname(string.format("fan: slave-%d-%d", master_pid, slave_index))

    -- assert(fan.setpgid())

    -- for i=1,3 do
    --   assert(fan.close(i - 1))
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

          local func = funcmap[args[1]]
          local ret = ""
          if func then
            local results = { func(table.unpack(args, 2, maxn(args))) }
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
