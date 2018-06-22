local fan = require "fan"
local fifo = require "fan.fifo"
local stream = require "fan.stream"

local MAX_LINE_SIZE = 8192

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
  if self.disconnected or not self._fifo_write then
    return nil
  end

  if self.send_running then
    table.insert(self._sender_queue, (coroutine.running()))
    coroutine.yield()
  end

  -- print("write", #(buf))
  if #(buf) > MAX_LINE_SIZE then
    local count = math.floor(#(buf) / MAX_LINE_SIZE)
    for i = 0, count do
      table.insert(self._output_queue, string.sub(buf, i * MAX_LINE_SIZE + 1, i * MAX_LINE_SIZE + MAX_LINE_SIZE))
    end
  else
    table.insert(self._output_queue, buf)
  end

  if self.simulate_send_block then
    self.send_running = coroutine.running()
    self._fifo_write:send_req()
    return coroutine.yield()
  else
    self._fifo_write:send_req()
  end
end

function apt_mt:_onsendready()
  if #(self._output_queue) > 0 then
    local buf = self._output_queue[1]
    -- print(self, "send", #(buf))
    local sent = self._fifo_write:send(buf)
    if sent > 0 then
      if sent == 0 then
        -- do nothing.
      elseif sent < #(buf) then
        self._output_queue[1] = buf:sub(sent + 1)
      else
        table.remove(self._output_queue, 1)
      end
    end
  elseif self.send_running then
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

  if self._fifo_write then
    self._fifo_write:send_req()
  end
end

-- function apt_mt:_check_read_timeout()
-- fan.sleep(2)

-- if self.receiving then
-- print("read timeout")
-- local receiving = self.receiving
-- self.receiving = nil
-- local st,msg = coroutine.resume(receiving)
-- if not st then
-- print(msg)
-- end
-- end
-- end

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

    local st, msg = coroutine.resume(receiving, input)
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
  if self._fifo_write then
    self._fifo_write:close()
    self._fifo_write = nil
  end
  if self._fifo_read then
    self._fifo_read:close()
    self._fifo_read = nil
  end

  if self.send_running then
    local send_running = self.send_running
    self.send_running = nil
    assert(coroutine.resume(send_running))
  end
end

local function connect(host, port, path)
  local conn, err =
    fifo.connect {
    name = path,
    rwmode = "w"
  }

  if not conn then
    return nil, err
  end

  local obj = {
    _conn = conn,
    _fifo_read = nil,
    _fifo_write = nil,
    _readstream = stream.new(),
    _output_queue = {},
    _sender_queue = {},
    simulate_send_block = true
  }
  setmetatable(obj, apt_mt)

  obj._fifo_read_name = os.tmpname()
  os.remove(obj._fifo_read_name)

  -- print("waiting for write pipe", fifo_read_name)

  local running = coroutine.running()

  obj._fifo_read =
    fifo.connect {
    name = obj._fifo_read_name,
    delete_on_close = true,
    rwmode = "r",
    onread = function(buf)
      obj._readstream:prepare_add()
      obj._readstream:AddBytes(buf)
      obj._readstream:prepare_get()
      if not obj._fifo_write then
        obj._fifo_write_name = obj._readstream:GetString()
        -- print("connect for write", fifo_name)
        obj._fifo_write =
          fifo.connect {
          name = obj._fifo_write_name,
          delete_on_close = true,
          rwmode = "w",
          onsendready = function()
            obj:_onsendready()
          end,
          ondisconnected = function(msg)
            obj:_ondisconnected(msg)
          end
        }
        assert(coroutine.resume(running))
      else
        if not obj:_onread(obj._readstream) and obj.onread then
          obj.onread(obj._readstream)
        end
      end
    end
    -- ondisconnected = function(msg)
    -- print("ondisconnected", msg)
    -- end
  }

  local output = stream.new()
  output:AddString(obj._fifo_read_name)
  conn:send(output:package())

  coroutine.yield()

  return obj
end

local function bind(host, port, path)
  local obj = {conn = nil, onaccept = nil, _readstream = stream.new()}
  obj.conn =
    fifo.connect {
    name = path,
    delete_on_close = true,
    rwmode = "r",
    onread = function(buf)
      obj._readstream:prepare_add()
      obj._readstream:AddBytes(buf)
      obj._readstream:prepare_get()

      while obj._readstream:available() > 0 do
        local apt = {
          _fifo_read = nil,
          _fifo_write = nil,
          _readstream = stream.new(),
          _output_queue = {},
          _sender_queue = {},
          receiving = nil,
          simulate_send_block = true
        }
        setmetatable(apt, apt_mt)

        apt._fifo_write_name = obj._readstream:GetString()

        apt._fifo_write =
          fifo.connect {
          name = apt._fifo_write_name,
          delete_on_close = true,
          rwmode = "w",
          onsendready = function()
            apt:_onsendready()
          end,
          ondisconnected = function(msg)
            apt:_ondisconnected(msg)
          end
        }

        apt._fifo_read_name = os.tmpname()
        os.remove(apt._fifo_read_name)

        -- print("waiting for write pipe", fifo_read_name)

        apt._fifo_read =
          fifo.connect {
          name = apt._fifo_read_name,
          delete_on_close = true,
          rwmode = "r",
          onread = function(buf)
            apt._readstream:prepare_add()
            apt._readstream:AddBytes(buf)
            apt._readstream:prepare_get()

            if not apt:_onread(apt._readstream) and apt.onread then
              apt.onread(apt._readstream)
            end
          end,
          ondisconnected = function(msg)
            apt:_ondisconnected(msg)
          end
        }

        local output = stream.new()
        output:AddString(apt._fifo_read_name)
        apt:send(output:package())

        if obj.onaccept then
          local st, msg = coroutine.resume(coroutine.create(obj.onaccept), apt)
          if not st then
            print(msg)
          end
        end
      end
    end
  }

  obj.close = function(self)
    self.conn:close()
    self.conn = nil
  end

  return obj
end

return {
  connect = connect,
  bind = bind
}
