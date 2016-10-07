local fan = require "fan"
local connector = require "fan.connector"

local master_mt = {}
master_mt.__index = master_mt

local function new(funcmap, slavecount)
  local fifoname = connector.tmpfifoname()
  local url = "fifo:" .. fifoname

  local master = false
  local slave_pids = {}

  for i=1,slavecount do
    print("forking")
    local pid = assert(fan.fork())
    master = pid > 0

    if not master then
      break
    else
      assert(fan.setpgid(pid, pid))
      table.insert(slave_pids, pid)
    end
  end

  if master then
    local obj = {slaves = {}, slave_pids = slave_pids}
    obj.serv = connector.bind(url)

    obj.serv.onaccept = function(apt)
      print("onaccept", apt)
      obj.slaves[apt] = apt

      apt.onread = function(input)
        print("onread", input:GetString())
      end

      while true do
        local output = stream.new()
        output:AddString(os.date())
        if not apt:send(output:package()) then
          obj.slaves[apt] = nil
          break
        end
        fan.sleep(1)
      end

    end

    return obj
  else
    local pid = fan.getpid()
    assert(fan.setpgid())

    -- for i=1,3 do
    --   assert(fan.close(i - 1))
    -- end
    --
    -- local f0 = fan.open("/dev/null")
    -- local f1 = fan.open("/dev/null")
    -- local f2 = fan.open("/dev/null")

    fan.loop(function()
        while not cli do
          fan.sleep(0.001)
          cli = connector.connect(url)
        end

        while true do
          local input = cli:receive()
          if not input then
            break
          end

          print(pid, "onread", input:available(), input:GetString())

          local output = stream.new()
          output:AddString(os.time())
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
