local tcpd = require "fan.tcpd"
local stream = require "fan.stream"

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
  if self.disconnected or not self.conn then
    return nil
  end

  self.conn:send(buf)

  return #(buf)
end

function apt_mt:receive(expect)
  if self.disconnected then
    return nil
  end

  expect = expect or 1

  if self._readstream:available() >= expect then
    return self._readstream
  else
    self.receiving_expect = expect
    self.receiving = coroutine.running()
    return coroutine.yield()
  end
end

function apt_mt:_onread(input)
  if self.receiving and (not input or input:available() >= self.receiving_expect) then
    local receiving = self.receiving
    self.receiving = nil
    self.receiving_expect = 0

    local st,msg = coroutine.resume(receiving, input)
    if not st then
      print(msg)
    end
    return true
  end
end

function apt_mt:_ondisconnected(msg)
  print("ondisconnected", msg)
  self:close()
end

function apt_mt:close()
  self.disconnected = true

  self:_onread(nil)
  if self.conn then
    self.conn:close()
    self.conn = nil
  end
end

local function connect(host, port, path)
  local t = {_readstream = stream.new()}
  t.conn = tcpd.connect{
    host = host,
    port = port,
    onread = function(buf)
      t._readstream:prepare_add()
      t._readstream:AddBytes(buf)
      t._readstream:prepare_get()

      t:_onread(t._readstream)
    end,
    ondisconnected = function(msg)
      print("client ondisconnected", msg)
      t:close()
    end
  }

  setmetatable(t, apt_mt)
  return t
end

local function bind(host, port, path)
  local obj = {onaccept = nil}
  obj.serv = tcpd.bind{
    host = host,
    port = port,
    onaccept = function(apt)
      local t = {conn = apt, _readstream = stream.new()}
      setmetatable(t, apt_mt)

      apt:bind{
        onread = function(buf)
          t._readstream:prepare_add()
          t._readstream:AddBytes(buf)
          t._readstream:prepare_get()

          if not t:_onread(t._readstream) and t.onread then
            t.onread(t._readstream)
          end
        end,
        ondisconnected = function(msg)
          print("client ondisconnected", msg)
          t:close()
        end
      }

      if obj.onaccept then
        obj.onaccept(t)
      end
    end
  }

  return obj
end

return {
  connect = connect,
  bind = bind
}
