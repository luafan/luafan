local udpd = require "fan.udpd"
local utils = require "fan.utils"
local config = require "config"

config.udp_send_total = 0
config.udp_receive_total = 0
config.udp_resend_total = 0

-- impl waiting pool
local MTU = 576 - 8 - 20
local HEAD_SIZE = 2 + 2 + 2
local BODY_SIZE = MTU - HEAD_SIZE

local TIMEOUT = config.udp_package_timeout or 2
local WAITING_COUNT = 10

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

  if self._output_wait_count >= WAITING_COUNT then
    table.insert(self._output_wait_thread, coroutine.running())
    coroutine.yield()
  end

  local output_index = self._output_index + 1
  if output_index > 65535 then
    output_index = 1
  end
  self._output_index = output_index

  local package_index_map = {}
  self._output_wait_index_map[output_index] = package_index_map

  local dest = {...}

  if #(buf) > BODY_SIZE then
    local package_index = 1
    local index_count = math.floor(#(buf) / BODY_SIZE)
    for i=0,index_count do
      local body = string.sub(buf, i * BODY_SIZE + 1, i * BODY_SIZE + BODY_SIZE)
      local head = string.pack("<I2I2I2", output_index, index_count + 1, package_index)
      -- print(string.format("pp: %d %d/%d", output_index, package_index, index_count + 1))
      package_index_map[package_index] = true
      package_index = package_index + 1

      self._output_queue[head] = head .. body
      self._output_dest[head] = dest
    end
  else
    local head = string.pack("<I2I2I2", output_index, 1, 1)
    self._output_queue[head] = head .. buf
    self._output_dest[head] = dest
    package_index_map[1] = true
  end

  -- print("send_req")
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

    if #(self._output_wait_thread) > 0 then
      coroutine.resume(table.remove(self._output_wait_thread, 1))
    end
    -- print("_output_wait_count", self._output_wait_count)
  end
end

function apt_mt:_onread(buf, host, port)
  -- print("read", self.dest, #(buf))
  if #(buf) == HEAD_SIZE then
    local output_index,count,package_index = string.unpack("<I2I2I2", buf)
    if config.debug then
      print(string.format("%s:%d\tack: %d %d/%d", host, port, output_index, package_index, count))
    end
    self:_mark_send_completed(buf)
    self.conn:send_req()

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
  elseif #(buf) > HEAD_SIZE then
    local head = string.sub(buf, 1, HEAD_SIZE)
    table.insert(self._output_ack_package, head)
    self._output_ack_dest[head] = {host, port}
    -- print("send_req")
    self.conn:send_req()
    -- if config.debug then
    -- print("sending ack", #(head), #(self._output_ack_package))
    -- end

    local body = string.sub(buf, HEAD_SIZE + 1)
    local output_index,count,package_index = string.unpack("<I2I2I2", head)
    if config.debug then
      print(string.format("%s:%d\trecv: %d %d/%d (%d)", host, port, output_index, package_index, count, #(body)))
    end

    local key = string.format("%s:%d", host, port)
    local incoming = self._incoming_map[key]

    if not incoming then
      incoming = {}
      self._incoming_map[key] = incoming
    end

    local incoming_object = incoming[output_index]
    if not incoming_object then
      incoming_object = {items = {}, count = count, start = utils.gettime()}
      incoming[output_index] = incoming_object
    elseif incoming_object.done then
      return
    end

    incoming_object.items[package_index] = body

    for i=1,count do
      if not incoming_object.items[i] then
        return
      end
    end

    -- print(self.dest, string.format("fail rate: %d/%d", self._output_wait_timeout_count_map[output_index] or 0, count))
    -- self._output_wait_timeout_count_map[output_index] = nil

    -- mark true, so we can drop dup packages.
    incoming_object.done = true
    incoming_object.donetime = utils.gettime()

    if self.onread then
      coroutine.wrap(self.onread)(table.concat(incoming_object.items), host, port)
    end
  else
    print("receive buf size too small", #(buf), fan.data2hex(buf))
  end
end

function apt_mt:_send(buf, dest)
  -- print("send", #(buf))
  if config.debug then
    local output_index,count,package_index = string.unpack("<I2I2I2", buf)
    print(string.format("send: %d %d/%d", output_index, package_index, count))
  end

  if dest and #(dest) > 0 then
    self.conn:send(buf, table.unpack(dest))
  else
    self.conn:send(buf, self.dest)
  end

  config.udp_send_total = config.udp_send_total + 1

  -- print("send_req")
  self.conn:send_req()
end

function apt_mt:_check_timeout()
  for key,incoming in pairs(self._incoming_map) do
    for index,incoming_object in pairs(incoming) do
      if incoming_object.done and utils.gettime() - incoming_object.donetime > 10 then
        incoming[index] = nil
        -- elseif utils.gettime() - incoming_object.start > 120 then
        -- incoming[index] = nil
      end
    end
    if not next(incoming) then
      self._incoming_map[key] = nil
    end
  end
  local has_timeout = false
  for k,v in pairs(self._output_wait_ack) do
    if gettime() - v >= TIMEOUT then
      has_timeout = true
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
          config.udp_resend_total = config.udp_resend_total + 1
          self._output_queue[k] = self._output_wait_package[k]
          self._output_wait_ack[k] = gettime()
        else
          self:_mark_send_completed(k)
        end
      else
        config.udp_resend_total = config.udp_resend_total + 1
        self._output_queue[k] = self._output_wait_package[k]
        self._output_wait_ack[k] = gettime()
      end
    end
  end

  if has_timeout then
    self.conn:send_req()
  end

  return has_timeout
end

function apt_mt:_onsendready()
  if #(self._output_ack_package) > 0 then
    local package = table.remove(self._output_ack_package)
    -- print("send ack")
    self:_send(package, self._output_ack_dest[package])
    self._output_ack_dest[package] = nil
    return true
  end

  for k,v in pairs(self._output_queue) do
    self:_send(v, self._output_dest[k])
    self._output_wait_package[k] = v
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

    local key = string.format("%s:%d", host, port)

    self._incoming_map[key] = nil

    if self._parent then
      self._parent.clientmap[key] = nil
    end
  end

end

function apt_mt:close()
  self.stop = true

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
    _output_wait_thread = {},
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
      config.udp_receive_total = config.udp_receive_total + 1
      t:_onread(buf, from:getHost(), from:getPort())
    end,
    onsendready = function()
      if not t:_onsendready() then
        -- fan.sleep(1)
        -- print("send_req", t.conn, "conn.onsendready")
        -- t.conn:send_req()
      end
    end
  }

  t.stop = false

  coroutine.wrap(function()
      while not t.stop do
        t:_check_timeout()
        fan.sleep(0.5)
      end
      end)()

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
      end,
      onread = function(buf, from)
        config.udp_receive_total = config.udp_receive_total + 1
        local host = from:getHost()
        local port = from:getPort()
        local client_key = string.format("%s:%d", host, port)
        local apt = obj.clientmap[client_key]
        if not apt then
          apt = {
            host = host,
            port = port,
            dest = from,
            conn = obj.serv,
            _parent = obj,
            _output_index = 0,
            _output_queue = {},
            _output_dest = {},
            _output_wait_ack = {},
            _output_wait_package = {},
            _output_wait_thread = {},
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

        apt:_onread(buf, host, port)
      end
    }

    obj.stop = false

    coroutine.wrap(function()
        while not obj.stop do
          for clientkey,apt in pairs(obj.clientmap) do
            if apt:_check_timeout() then
              break
            end
          end

          fan.sleep(0.5)
        end
        end)()

      obj.close = function()
        obj.stop = true

        for clientkey,apt in pairs(obj.clientmap) do
          apt:close()
        end

        obj.serv:close()
        obj.serv = nil
      end
      -- print("serv", obj.serv)

      return obj
    end

    return {
      connect = connect,
      bind = bind,
    }
