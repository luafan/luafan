local fan = require "fan"
local fifo = require "fan.fifo"
local stream = require "fan.stream"

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
  if self.disconnected or not self._fifo_write then
    return nil
  end

  self._fifo_write:send(buf)

  return #(buf)
end

function apt_mt:receive()
  if self.disconnected then
    return nil
  end

  self.receiving = coroutine.running()
  return coroutine.yield()
end

function apt_mt:_onread(...)
  if self.receiving then
    local receiving = self.receiving
    self.receiving = nil

    coroutine.resume(receiving, ...)
    return true
  end
end

function apt_mt:_ondisconnected(msg)
  print("ondisconnected", msg)
  self.disconnected = true

  self:_onread(nil)
  self._fifo_write:close()
  self._fifo_write = nil
  os.remove(self._fifo_write_name)

  self._fifo_read:close()
  self._fifo_read = nil
  os.remove(self._fifo_read_name)
end

local function connect(host, port, path)
  local obj = {_fifo_read = nil, _fifo_write = nil, _readstream = stream.new()}
  setmetatable(obj, apt_mt)

  obj._fifo_read_name = os.tmpname()
  os.remove(obj._fifo_read_name)

  -- print("waiting for write pipe", fifo_read_name)

  obj._fifo_read = fifo.connect{
    name = obj._fifo_read_name,
    rwmode = "r",
    onread = function(buf)
      obj._readstream:prepare_add()
      obj._readstream:AddBytes(buf)
      obj._readstream:prepare_get()
      if not obj._fifo_write then
        obj._fifo_write_name = obj._readstream:GetString()
        -- print("connect for write", fifo_name)
        obj._fifo_write = fifo.connect{
          name = obj._fifo_write_name,
          rwmode = "w",
          ondisconnected = function(msg)
            obj:_ondisconnected(msg)
          end
        }
      else
        if not obj:_onread(obj._readstream) and obj.onread then
          obj.onread(obj._readstream)
        end
      end
    end,
    -- ondisconnected = function(msg)
    --   print("ondisconnected", msg)
    -- end
  }

  local conn, err = fifo.connect{
    name = path,
    rwmode = "w"
  }

  if not conn then
    return nil, err
  end

  local output = stream.new()
  output:AddString(obj._fifo_read_name)
  conn:send(output:package())
  return obj
end

local function bind(host, port, path)
  local obj = {conn = nil, onaccept = nil, _readstream = stream.new()}
  obj.conn = fifo.connect{
    name = path,
    rwmode = "r",
    onread = function(buf)
      obj._readstream:prepare_add()
      obj._readstream:AddBytes(buf)
      obj._readstream:prepare_get()
      local apt = {_fifo_read = nil, _fifo_write = nil, _readstream = stream.new(), receiving = nil}
      setmetatable(apt, apt_mt)

      apt._fifo_write_name = obj._readstream:GetString()

      apt._fifo_write = fifo.connect{
        name = apt._fifo_write_name,
        rwmode = "w",
        ondisconnected = function(msg)
          apt:_ondisconnected(msg)
        end
      }

      apt._fifo_read_name = os.tmpname()
      os.remove(apt._fifo_read_name)

      -- print("waiting for write pipe", fifo_read_name)

      apt._fifo_read = fifo.connect{
        name = apt._fifo_read_name,
        rwmode = "r",
        onread = function(buf)
          apt._readstream:prepare_add()
          apt._readstream:AddBytes(buf)
          apt._readstream:prepare_get()

          if not apt:_onread(apt._readstream) and apt.onread then
            apt.onread(apt._readstream)
          end
        end,
        -- ondisconnected = function(msg)
        --   apt.disconnected = true
        --   print("ondisconnected", msg)
        -- end
      }

      local output = stream.new()
      output:AddString(apt._fifo_read_name)
      apt:send(output:package())

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
