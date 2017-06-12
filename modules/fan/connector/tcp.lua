local tcpd = require "fan.tcpd"
local stream = require "fan.stream"
local config = require "config"

local TCP_PAUSE_READ_WRITE_ON_CALLBACK = config.tcp_pause_read_write_on_callback

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
  if self.disconnected or not self.conn then
    return nil
  end

  if self.send_running then
    table.insert(self._sender_queue, (coroutine.running()))
    coroutine.yield()
  end

  if self.simulate_send_block then
    self.send_running = coroutine.running()
    self.conn:send(buf)
    coroutine.yield()
  else
    self.conn:send(buf)
  end

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

function apt_mt:_onsendready()
  if self.send_running then
    local send_running = self.send_running
    self.send_running = nil
    if #(self._sender_queue) > 0 then
      local running = table.remove(self._sender_queue, 1)
      assert(coroutine.resume(running))
    end
    assert(coroutine.resume(send_running, true))
  else
    return
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

local function connect(host, port, path, args)
  local t = {_readstream = stream.new(), _sender_queue = {}, simulate_send_block = true}
  local params = {
    host = host,
    port = port,
    onread = function(buf)
      t._readstream:prepare_add()
      t._readstream:AddBytes(buf)
      t._readstream:prepare_get()

      if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
        t.conn:pause_read()
      end
      
      t:_onread(t._readstream)

      if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
        t.conn:resume_read()
      end
    end,
    onsendready = function()
      t:_onsendready()
    end,
    ondisconnected = function(msg)
      print("client ondisconnected", msg)
      t:close()
    end
  }

  if args and type(args) == "table" then
    for k,v in pairs(args) do
      params[k] = v
    end
  end

  t.conn = tcpd.connect(params)

  setmetatable(t, apt_mt)
  return t
end

local function bind(host, port, path, args)
  local obj = {onaccept = nil}

  local params = {
    host = host,
    port = port,
    onaccept = function(apt)
      local t = {conn = apt, _readstream = stream.new(), _sender_queue = {}, simulate_send_block = true}
      setmetatable(t, apt_mt)

      apt:bind{
        onread = function(buf)
          t._readstream:prepare_add()
          t._readstream:AddBytes(buf)
          t._readstream:prepare_get()

          if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
            t.conn:pause_read()
          end

          if not t:_onread(t._readstream) and t.onread then
            local status,msg = pcall(t.onread, t._readstream)
            if not status then
              print(msg)
            end
          end
          if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
            t.conn:resume_read()
          end
        end,
        onsendready = function()
          t:_onsendready()
        end,
        ondisconnected = function(msg)
          if config.debug then
            print("client ondisconnected", msg)
          end
          t:close()
        end
      }

      if obj.onaccept then
        obj.onaccept(t)
      end
    end
  }
  
  if args and type(args) == "table" then
    for k,v in pairs(args) do
      params[k] = v
    end
  end

  obj.serv = tcpd.bind(params)

  return obj
end

return {
  connect = connect,
  bind = bind
}
