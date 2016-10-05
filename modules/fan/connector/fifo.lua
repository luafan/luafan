local fan = require "fan"
local fifo = require "fan.fifo"
local stream = require "fan.stream"

local function connect(host, port, path)
  local obj = {fifo_read = nil, fifo_write = nil, readstream = stream.new()}

  local fifo_read_name = os.tmpname()
  os.remove(fifo_read_name)

  print("waiting for write pipe", fifo_read_name)

  obj.fifo_read = fifo.connect{
    name = fifo_read_name,
    rwmode = "r",
    onread = function(buf)
      obj.readstream:prepare_add()
      obj.readstream:AddBytes(buf)
      obj.readstream:prepare_get()
      if not obj.fifo_write then
        local fifo_name = obj.readstream:GetString()
        print("connect for write", fifo_name)
        obj.fifo_write = fifo.connect{
          name = fifo_name,
          rwmode = "w",
          onsendready = function()
            if obj.onsendready then
              obj.onsendready()
            end
          end,
          ondisconnected = function(msg)
            print("ondisconnected", msg)
          end
        }
      else
        if obj.onread then
          obj.onread(obj.readstream)
        end
      end
    end,
    ondisconnected = function(msg)
      print("ondisconnected", msg)
    end
  }

  local conn = fifo.connect{
    name = path,
    rwmode = "w"
  }

  if not conn then
    return nil, "unable to connect"
  end

  local output = stream.new()
  output:AddString(fifo_read_name)
  conn:send(output:package())
  return obj
end

local function bind(host, port, path)
  local obj = {conn = nil, onaccept = nil, readstream = stream.new()}
  obj.conn = fifo.connect{
    name = path,
    rwmode = "r",
    onread = function(buf)
      obj.readstream:prepare_add()
      obj.readstream:AddBytes(buf)
      obj.readstream:prepare_get()
      local fifo_name = obj.readstream:GetString()
      local apt = {fifo_read = nil, fifo_write = nil, readstream = stream.new()}
      apt.fifo_write = fifo.connect{
        name = fifo_name,
        rwmode = "w",
        onsendready = function()
          if apt.onsendready then
            apt.onsendready()
          end
        end,
        ondisconnected = function(msg)
          apt.disconnected = true
          print("ondisconnected", msg)
        end
      }

      local fifo_read_name = os.tmpname()
      os.remove(fifo_read_name)

      print("waiting for write pipe", fifo_read_name)

      apt.fifo_read = fifo.connect{
        name = fifo_read_name,
        rwmode = "r",
        onread = function(buf)
          apt.readstream:prepare_add()
          apt.readstream:AddBytes(buf)
          apt.readstream:prepare_get()
          if apt.onread then
            apt.onread(apt.readstream)
          end
        end,
        ondisconnected = function(msg)
          apt.disconnected = true
          print("ondisconnected", msg)
        end
      }

      local output = stream.new()
      output:AddString(fifo_read_name)
      apt.fifo_write:send(output:package())

      if obj.onaccept then
        obj.onaccept(apt)
      end
    end
  }

  return obj
end

return {
  connect = connect,
  bind = bind
}
