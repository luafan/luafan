local udpd = require "fan.udpd"
-- impl waiting pool
local MTU = 576 - 8 - 20
local HEAD_SIZE = 2 + 2 + 2
local BODY_SIZE = MTU - HEAD_SIZE

local TIMEOUT = 2
local WAITING_COUNT = 10 * 1024 -- about 5mb

local function gettime()
  local sec,usec = fan.gettime()
  return sec + usec/1000000.0
end

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf)
  if not self.conn then
    return nil
  end

  self._output_index = self._output_index + 1

  if #(buf) > BODY_SIZE then
    local package_index = 1
    local count_index = math.floor(#(buf) / BODY_SIZE)
    for i=0,count_index do
      local package = string.sub(buf, i * BODY_SIZE + 1, i * BODY_SIZE + BODY_SIZE)
      local head = string.pack("<I2I2I2", self._output_index, count_index + 1, package_index)
      package_index = package_index + 1

      self._output_queue[head] = package
    end
  else
    local head = string.pack("<I2I2I2", self._output_index, 1, 1)
    self._output_queue[head] = buf
  end

  self.conn:send_req()
end

function apt_mt:_moretosend()
  if #(self._output_ack_package) > 0 then
    return true
  end

  for k,v in pairs(self._output_queue) do
    return true
  end

  for k,v in pairs(self._output_wait_ack) do
    return true
  end
end

function apt_mt:_onread(buf)
  -- print("read", self.dest, #(buf))
  if #(buf) == HEAD_SIZE then
    if self._output_wait_ack[buf] then
      self._output_wait_ack[buf] = nil
      self._output_wait_package[buf] = nil
      self._output_wait_count = self._output_wait_count - 1
      -- print("_output_wait_count", self._output_wait_count)
    end
    return
  else
    local head = string.sub(buf, 1, HEAD_SIZE)
    table.insert(self._output_ack_package, head)
    self.conn:send_req()
    -- print("sending ack", #(head), #(self._output_ack_package))

    local body = string.sub(buf, HEAD_SIZE + 1)
    local output_index,count,package_index = string.unpack("<I2I2I2", head)
    -- print(string.format("idx: %d %d/%d", output_index, package_index, count))

    local incoming_items = self._incoming_map[output_index]
    if incoming_items == true then
      return
    elseif not incoming_items then
      incoming_items = {}
      self._incoming_map[output_index] = incoming_items
    end

    incoming_items[package_index] = body

    for i=1,count do
      if not incoming_items[i] then
        return
      end
    end

    -- print(self.dest, string.format("fail rate: %d/%d", self._output_wait_timeout_count_map[output_index] or 0, count))
    -- self._output_wait_timeout_count_map[output_index] = nil

    if self.onread then
      coroutine.wrap(self.onread)(table.concat(incoming_items))
    end

    -- mark true, so we can drop dup packages.
    self._incoming_map[output_index] = true
  end
end

function apt_mt:_send(buf)
  -- print("send", self.dest, #(buf))
  self.conn:send(buf, self.dest)
  self.conn:send_req()
end

function apt_mt:_onsendready()
  if #(self._output_ack_package) > 0 then
    local package = table.remove(self._output_ack_package)
    -- print("send ack")
    self:_send(package)
    return true
  end

  for k,v in pairs(self._output_wait_ack) do
    if gettime() - v >= TIMEOUT then
      self:_send(self._output_wait_package[k])
      self._output_wait_ack[k] = gettime()

      -- local output_index = string.unpack("<I2", k)
      -- local timeout_count = self._output_wait_timeout_count_map[output_index] or 0
      -- self._output_wait_timeout_count_map[output_index] = timeout_count + 1
      return true
    end
  end

  if self._output_wait_count >= WAITING_COUNT then
    return false
  end

  for k,v in pairs(self._output_queue) do
    local package = string.pack(string.format("<c%dc%d", #(k), #(v)), k, v)
    self:_send(package)
    self._output_wait_package[k] = package
    self._output_wait_ack[k] = gettime()
    self._output_wait_count = self._output_wait_count + 1
    -- print("_output_wait_count", self._output_wait_count)

    self._output_queue[k] = nil
    return true
  end

  return false
end

function apt_mt:close()
  self:_onread(nil)
  if self.conn then
    self.conn:close()
    self.conn = nil
  end

  if self.send_running then
    local send_running = self.send_running
    self.send_running = nil
    coroutine.resume(send_running)
  end
end

local function connect(host, port, path)
  local t = {
    _output_index = 0,
    _output_queue = {},
    _output_wait_ack = {},
    _output_wait_package = {},
    _output_wait_count = 0,
    -- _output_wait_timeout_count_map = {},
    _output_ack_package = {},
    _incoming_map = {},
  }
  setmetatable(t, apt_mt)

  t.conn = udpd.new{
    host = host,
    port = port,
    onread = function(buf)
      -- print("onread", #(buf))
      t:_onread(buf)
    end,
    onsendready = function()
      if not t:_onsendready() then
        fan.sleep(1)
        t.conn:send_req()
      end
    end
  }

  return t
end

local function bind(host, port, path)
  local obj = {clientmap = {}}

  obj.serv = udpd.new{
    bind_port = port,
    onsendready = function()
      for k,apt in pairs(obj.clientmap) do
        if apt:_moretosend() and apt:_onsendready() then
          return
        end
      end

      fan.sleep(1)
      obj.serv:send_req()
    end,
    onread = function(buf, from)
      local client_key = tostring(from)
      local apt = obj.clientmap[client_key]
      if not apt then
        apt = {
          dest = from,
          conn = obj.serv,
          _output_index = 0,
          _output_queue = {},
          _output_wait_ack = {},
          _output_wait_package = {},
          _output_wait_count = 0,
          -- _output_wait_timeout_count_map = {},
          _output_ack_package = {},
          _incoming_map = {},
        }
        setmetatable(apt, apt_mt)
        obj.clientmap[client_key] = apt

        if obj.onaccept then
          coroutine.wrap(obj.onaccept)(apt)
        end
      end

      apt:_onread(buf)
    end
  }

  -- print("serv", obj.serv)

  return obj
end

return {
  connect = connect,
  bind = bind,
}
