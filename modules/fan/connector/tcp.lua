local tcpd = require "fan.tcpd"
local stream = require "fan.stream"
local config = require "config"
local utils = require "fan.utils"

local TCP_PAUSE_READ_WRITE_ON_CALLBACK = config.tcp_pause_read_write_on_callback == nil and true or config.tcp_pause_read_write_on_callback

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
  if self.disconnected or not self.conn or not buf or #(buf) == 0 then
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

    local input = coroutine.yield()
    self.receiving_expect = 0
    self.receiving = nil

    return input
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
    local st, msg = coroutine.resume(self.receiving, input)
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
    if self.connection_map then
      self.connection_map[self.conn] = nil
    end
    self.conn:close()
    self.conn = nil
  end
end

local function connect(host, port, path, args)
  local verbose = args and args.verbose == 1 or false
  local running = coroutine.running()

  local t = {_readstream = stream.new(), _sender_queue = {}, simulate_send_block = true}
  local weak_t = t -- utils.weakify_object(t)
  local params = {
    host = host,
    port = port,
    onconnected = function()
      coroutine.resume(running)
    end,
    onread = function(buf)
      collectgarbage()
      
      local t = weak_t

      if verbose then
        print("[CONNECTOR][READ]", buf)
      end

      t._readstream:prepare_add()
      t._readstream:AddBytes(buf)
      t._readstream:prepare_get()

      if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
        if t.conn then
          t.conn:pause_read()
        end
      end

      t:_onread(t._readstream)

      if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
        if t.conn then
          t.conn:resume_read()
        end
      end
    end,
    onsendready = function()
      local t = weak_t
      t:_onsendready()
    end,
    ondisconnected = function(msg)
      if config.debug then
        print("client ondisconnected", msg)
      end
      local t = weak_t
      t.disconnected_message = msg

      if running then
        coroutine.resume(running)
      end

      t:close()
    end
  }

  if args and type(args) == "table" then
    for k, v in pairs(args) do
      params[k] = v
    end
  end

  t.conn = tcpd.connect(params)

  setmetatable(t, apt_mt)

  coroutine.yield()
  running = nil

  return t
end

local function bind(host, port, path, args)
  local connection_map = {}
  local obj = {onaccept = nil, connection_map = connection_map}

  local weak_connection_map = utils.weakify_object(connection_map)
  local params = {
    host = host,
    port = port,
    onaccept = function(apt)
      local t = {
        connection_map = weak_connection_map,
        conn = apt,
        _readstream = stream.new(),
        _sender_queue = {},
        simulate_send_block = true
      }
      t._pack = {t}
      setmetatable(t, apt_mt)

      weak_connection_map[apt] = t

      local weak_obj = utils.weakify_object(t._pack)

      apt:bind {
        onread = function(buf)
          local t = weak_obj[1]
          t._readstream:prepare_add()
          t._readstream:AddBytes(buf)
          t._readstream:prepare_get()

          if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
            t.conn:pause_read()
          end

          if not t:_onread(t._readstream) and t.onread then
            local status, msg = pcall(t.onread, t._readstream)
            if not status then
              print(msg)
            end
          end
          if TCP_PAUSE_READ_WRITE_ON_CALLBACK then
            t.conn:resume_read()
          end
        end,
        onsendready = function()
          local t = weak_obj[1]
          t:_onsendready()
        end,
        ondisconnected = function(msg)
          if config.debug then
            print("client ondisconnected", msg)
          end
          local t = weak_obj[1]
          t:close()
        end
      }

      if obj.onaccept then
        obj.onaccept(t)
      end
    end
  }

  if args and type(args) == "table" then
    for k, v in pairs(args) do
      params[k] = v
    end
  end

  obj.serv = tcpd.bind(params)

  obj.rebind = function(obj)
    obj.serv:rebind()
  end

  return obj
end

return {
  connect = connect,
  bind = bind
}
