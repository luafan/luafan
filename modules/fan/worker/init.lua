local fan = require "fan"
local pool = require "fan.pool"
local config = require "config"
local objectbuf = config.worker_using_cjson and require "cjson" or require "fan.objectbuf"
local connector = require "fan.connector"
local stream = require "fan.stream"
require "compat53"

local function maxn(t)
  local n = 0
  for k, v in pairs(t) do
    if k > n then
      n = k
    end
  end

  return n
end

local loadbalance_mt = {}
loadbalance_mt.__index = loadbalance_mt

function loadbalance_mt.new(max_job_count)
  local obj = {max_job_count = max_job_count, slaves = {}, yielding = {head = nil, tail = nil}}
  setmetatable(obj, loadbalance_mt)
  return obj
end

function loadbalance_mt:_assign(slave)
  if self.yielding.head then
    local co = self.yielding.head.value
    self.yielding.head = self.yielding.head.next
    local st, msg = coroutine.resume(co, slave)
    if not st then
      print(msg)
    end
  end
end

function loadbalance_mt:add(slave)
  table.insert(self.slaves, slave)

  self:_assign(slave)
end

local function compare_slave_jobcount(a, b)
  return a.max_job_count - a.jobcount > b.max_job_count - b.jobcount
end

function loadbalance_mt:findbest()
  table.sort(self.slaves, compare_slave_jobcount)
  local slave = self.slaves[1]
  if not slave or slave.jobcount >= self.max_job_count then
    if not self.yielding.head then
      self.yielding.head = {value = coroutine.running()}
      self.yielding.tail = self.yielding.head
    else
      self.yielding.tail.next = {value = coroutine.running()}
      self.yielding.tail = self.yielding.tail.next
    end
    slave = coroutine.yield()
  end

  slave.jobcount = slave.jobcount + 1
  return slave
end

function loadbalance_mt:telldone(slave)
  slave.jobcount = slave.jobcount - 1
  self:_assign(slave)
end

local master_mt = {}
master_mt.__index = function(obj, k)
  if obj.func_names[k] then
    obj:wait_all_slaves()

    return function(obj, ...)
      local slave = obj.loadbalance:findbest()

      local task_key = string.format("%d", slave.task_index)
      slave.task_index = slave.task_index + 1
      local args = {task_key, k, ...}

      local output = stream.new()
      output:AddString(objectbuf.encode(args))

      if not slave:send(output:package()) then
        print("slave dead.")
        slave.status = "dead"
        return
      end

      -- slave resume maybe faster than master's salve:send resume
      if slave.task_map[task_key] then
        local args = slave.task_map[task_key]
        slave.task_map[task_key] = nil
        return table.unpack(args)
      else
        slave.task_map[task_key] = coroutine.running()
        return coroutine.yield()
      end
    end
  end
end

local function new(funcmap, slavecount, max_job_count, url)
  local samehost = false
  if not url then
    local fifoname = connector.tmpfifoname()
    url = "fifo:" .. fifoname
    samehost = true
  end

  local master = slavecount == 0
  local slave_pids = {}
  local slave_index
  local master_pid = fan.getpid()

  local cpu_count = fan.getcpucount()
  local cpu_masks = {}
  if cpu_count > 1 then
    for i = 1, cpu_count - 1 do
      table.insert(cpu_masks, 2 ^ (i - 1))
    end
  end

  for i = 1, slavecount do
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
    if not samehost and slavecount > 0 then
      error("master/slave within one script not allowed.")
      return
    end
    if cpu_count > 1 then
      fan.setaffinity(2 ^ (cpu_count - 1))
    end
    fan.setprogname(
      string.format("fan: master-%d U(%X/%d)", master_pid, slavecount, fan.getaffinity(), fan.getcpucount())
    )
    local obj = {
      slave_pool = pool.new(),
      loadbalance = loadbalance_mt.new(max_job_count),
      slave_pids = slave_pids,
      slaves = {},
      func_names = {}
    }
    for k, v in pairs(funcmap) do
      obj.func_names[k] = k
    end

    setmetatable(obj, master_mt)

    obj.terminate = function(self)
      for k, v in pairs(self.slave_pids) do
        fan.kill(v)
        self.slave_pids[k] = nil
      end
    end

    obj.wait_all_slaves = function()
      if #(obj.slaves) == #(slave_pids) then
        return
      end

      obj.serv = connector.bind(url)

      obj.serv.onaccept = function(apt)
        -- print("onaccept", apt)
        apt.task_map = {}
        apt.task_index = 1
        apt.jobcount = 0
        apt.status = "running"
        apt.max_job_count = max_job_count

        table.insert(obj.slaves, apt)
        obj.loadbalance:add(apt)

        if #(obj.slaves) == #(slave_pids) then
          if obj._wait_all_slaves_running then
            local running = obj._wait_all_slaves_running
            obj._wait_all_slaves_running = nil
            coroutine.resume(running, obj)
          end
        end

        local last_expect = 1

        while true do
          local input = apt:receive(last_expect)
          if not input then
            break
          end

          local str, expect = input:GetString()
          if str then
            last_expect = 1
            local args = objectbuf.decode(str)

            if apt.task_map[args[1]] then
              local running = apt.task_map[args[1]]
              apt.task_map[args[1]] = nil
              local st, msg = coroutine.resume(running, true, table.unpack(args, 2, maxn(args)))
              if not st then
                print(msg)
              end
            else
              apt.task_map[args[1]] = {true, table.unpack(args, 2, maxn(args))}
            end

            obj.loadbalance:telldone(apt)
          else
            -- print(pid, "not enough, expect", expect)
            last_expect = expect
          end
        end

        for task_key, co in pairs(apt.task_map) do
          if coroutine.status(co) == "suspended" then
            apt.status = "dead"
            assert(coroutine.resume(co, false, "slave dead."))
          end
        end
      end

      obj._wait_all_slaves_running = coroutine.running()
      return coroutine.yield()
    end

    return obj
  else
    if cpu_count > 1 then
      local st = fan.setaffinity(cpu_masks[(slave_index - 1) % #(cpu_masks) + 1])
      if not st then
        local mask = 0
        for i = 1, cpu_count - 1 do
          mask = mask + 2 ^ (i - 1)
        end
        fan.setaffinity(mask)
      end
    end

    local pid = fan.getpid()
    fan.setprogname(
      string.format("fan: slave-%d-%d U(%X/%d)", master_pid, slave_index, fan.getaffinity(), fan.getcpucount())
    )

    -- assert(fan.setpgid())

    -- for i=1,3 do
    -- assert(fan.close(i - 1))
    -- end
    --
    -- local f0 = fan.open("/dev/null")
    -- local f1 = fan.open("/dev/null")
    -- local f2 = fan.open("/dev/null")

    fan.loop(
      function()
        while true do
          while not cli do
            fan.sleep(0.1)
            cli = connector.connect(url)
          end

          local last_expect = 1

          while true do
            local input = cli:receive(last_expect)
            if not input then
              break
            end
            -- print(pid, "receive", input:available())

            local str, expect = input:GetString()
            if str then
              last_expect = 1
              -- print(pid, "onread slave", str)

              local args = objectbuf.decode(str)

              local task_key = args[1]
              local func = funcmap[args[2]]

              -- print(pid, "process", task_key, args[2], table.unpack(args, 3, maxn(args)))

              local ret = ""
              if func then
                local st, msg =
                  pcall(
                  function()
                    local results = {task_key, func(table.unpack(args, 3, maxn(args)))}
                    return objectbuf.encode(results)
                  end
                )
                if not st then
                  print(msg)
                else
                  ret = msg
                end
              end

              local output = stream.new()
              output:AddString(ret)
              cli:send(output:package())
            else
              -- print(pid, "not enough, expect", expect)
              last_expect = expect
            end
          end

          if cli then
            cli:close()
            cli = nil
          end

          fan.sleep(1)
        end
      end
    )

    if cli then
      cli:close()
      cli = nil
    end
    print(string.format("quiting pid=%d", pid))
    os.exit()
  end
end

return {
  new = new
}
