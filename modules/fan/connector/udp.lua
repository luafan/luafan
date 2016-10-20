local udpd = require "fan.udpd"
local config = require "config"
-- impl waiting pool
local MTU = 576 - 8 - 20
local HEAD_SIZE = 2 + 2 + 2
local BODY_SIZE = MTU - HEAD_SIZE

local TIMEOUT = config.udp_package_timeout or 2
local WAITING_COUNT = 10 * 1024 * 1024 / MTU -- 10MB

local function gettime()
  local sec,usec = fan.gettime()
  return sec + usec/1000000.0
end

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:send(buf, ...)
  if not self.conn then
    return nil
  end

  self._output_index = self._output_index + 1
  local output_index = self._output_index

  local package_index_map = {}
  self._output_wait_index_map[output_index] = package_index_map

  local dest = {...}

  if #(buf) > BODY_SIZE then
    local package_index = 1
    local index_count = math.floor(#(buf) / BODY_SIZE)
    for i=0,index_count do
      local package = string.sub(buf, i * BODY_SIZE + 1, i * BODY_SIZE + BODY_SIZE)
      local head = string.pack("<I2I2I2", output_index, index_count + 1, package_index)
      package_index_map[package_index] = true
      package_index = package_index + 1

      self._output_queue[head] = package
      self._output_dest[head] = dest
    end
  else
    local head = string.pack("<I2I2I2", output_index, 1, 1)
    self._output_queue[head] = buf
    self._output_dest[head] = dest
    package_index_map[1] = true
  end

  self.conn:send_req()

  return output_index
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

function apt_mt:_mark_send_completed(head)
  if self._output_wait_ack[head] then
    self._output_wait_ack[head] = nil
    self._output_ack_dest[head] = nil
    self._output_wait_package[head] = nil
    self._output_wait_count = self._output_wait_count - 1
    -- print("_output_wait_count", self._output_wait_count)
  end
end

function apt_mt:_onread(buf, ...)
  -- print("read", self.dest, #(buf))
  if #(buf) == HEAD_SIZE then
    local output_index,count,package_index = string.unpack("<I2I2I2", buf)
    self:_mark_send_completed(buf)

    local package_index_map = self._output_wait_index_map[output_index]
    if package_index_map then
      package_index_map[package_index] = nil
      if not next(package_index_map) then
        self._output_wait_index_map[output_index] = nil

        if self.onsent then
          coroutine.wrap(self.onsent)(output_index)
        end
      end
    end

    return
  else
    local head = string.sub(buf, 1, HEAD_SIZE)
    table.insert(self._output_ack_package, head)
    self._output_ack_dest[head] = {...}
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
      coroutine.wrap(self.onread)(table.concat(incoming_items), ...)
    end

    -- mark true, so we can drop dup packages.
    self._incoming_map[output_index] = true
  end
end

function apt_mt:_send(buf, dest)
  -- print("send", self.dest, #(buf), dest)
  if type(dest) == "string" then
    print(debug.traceback())
  end
  if dest and #(dest) > 0 then
    self.conn:send(buf, table.unpack(dest))
  else
    self.conn:send(buf, self.dest)
  end

  self.conn:send_req()
end

function apt_mt:_onsendready()
  if #(self._output_ack_package) > 0 then
    local package = table.remove(self._output_ack_package)
    -- print("send ack")
    self:_send(package, self._output_ack_dest[package])
    self._output_ack_dest[package] = nil
    return true
  end

  for k,v in pairs(self._output_wait_ack) do
    if gettime() - v >= TIMEOUT then
      if self.ontimeout then
        local dest = self._output_dest[k]
        local host, port
        if dest and #(dest) > 0 then
          host,port = table.unpack(dest)
        elseif self.dest then
          host = self.dest:getHost()
          port = self.dest:getPort()
        end

        local resend = self.ontimeout(self._output_wait_package[k], host, port)
        if resend then
          self:_send(self._output_wait_package[k], self._output_dest[k])
          self._output_wait_ack[k] = gettime()
        else
          self:_mark_send_completed(k)
        end
      else
        self:_send(self._output_wait_package[k], self._output_dest[k])
        self._output_wait_ack[k] = gettime()
      end

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
    local package = k .. v
    self:_send(package, self._output_dest[k])
    self._output_wait_package[k] = package
    self._output_wait_ack[k] = gettime()
    self._output_wait_count = self._output_wait_count + 1
    -- print("_output_wait_count", self._output_wait_count)

    self._output_queue[k] = nil
    return true
  end

  return false
end

-- cleanup packages(ack not include) related with host,port
function apt_mt:cleanup(host, port)
  if (not host or not port) and self.dest then
    host = self.dest:getHost()
    port = self.dest:getPort()
  end

  if host and port then
    for k,v in pairs(self._output_dest) do
      if self._output_wait_ack[k] and v[1] == host and v[2] == port then
        self._output_dest[v] = nil
        self._output_wait_package[v] = nil
        self._output_wait_ack[v] = nil
        self._output_wait_count = self._output_wait_count - 1
      end
    end
  end

end

function apt_mt:close()
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
    _output_dest = {},
    _output_wait_ack = {},
    _output_wait_package = {},
    _output_wait_index_map = {},
    _output_wait_count = 0,
    -- _output_wait_timeout_count_map = {},
    _output_ack_package = {},
    _output_ack_dest = {},
    _incoming_map = {},
  }
  setmetatable(t, apt_mt)

  t.conn = udpd.new{
    host = host,
    port = port,
    onread = function(buf, from)
      -- print("onread", #(buf))
      t:_onread(buf, from:getHost(), from:getPort())
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
          _output_dest = {},
          _output_wait_ack = {},
          _output_wait_package = {},
          _output_wait_index_map = {},
          _output_wait_count = 0,
          -- _output_wait_timeout_count_map = {},
          _output_ack_package = {},
          _output_ack_dest = {},
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
