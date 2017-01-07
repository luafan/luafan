local udpd = require "fan.udpd"
local utils = require "fan.utils"
local config = require "config"

local string = string
local next = next
local table = table
local pairs = pairs
local math = math
local print = print
local coroutine = coroutine
local tostring = tostring

config.udp_send_total = 0
config.udp_receive_total = 0
config.udp_resend_total = 0

-- impl waiting pool
local MTU = (config.udp_mtu or 576) - 8 - 20
local HEAD_SIZE = 2 + 2 + 2
local BODY_SIZE = MTU - HEAD_SIZE

local TIMEOUT = config.udp_package_timeout or 2
local WAITING_COUNT = config.udp_waiting_count or 10

local function gettime()
  local sec,usec = fan.gettime()
  return sec + usec/1000000.0
end

local apt_mt = {}
apt_mt.__index = apt_mt

function apt_mt:_output_chain_push(head, package)
  if not self._output_chain._head then
    self._output_chain._head = {head, package}
    self._output_chain._tail = self._output_chain._head
  else
    self._output_chain._tail._next = {head, package}
    self._output_chain._tail = self._output_chain._tail._next
  end
end

function apt_mt:_output_chain_pop()
  if self._output_chain._head then
    local head = self._output_chain._head[1]
    local package = self._output_chain._head[2]

    self._output_chain._head = self._output_chain._head._next
    if not self._output_chain._head then
      self._output_chain._tail = nil
    end

    return head, package
  end
end

function apt_mt:send(buf)
  if not self.conn then
    return nil
  end

  local output_index = self._output_index + 1
  if output_index >= 0xfff0 then
    output_index = 1
  end
  self._output_index = output_index

  local package_parts_map = {}

  if #(buf) > BODY_SIZE then
    local package_index = 1
    local index_count_f = #(buf) / BODY_SIZE
    local index_count,fractional = math.modf(index_count_f)
    if fractional == 0 then
      index_count = index_count - 1
    end

    for i=0,index_count do
      local body = string.sub(buf, i * BODY_SIZE + 1, i * BODY_SIZE + BODY_SIZE)
      local head = string.pack("<I2I2I2", output_index, index_count + 1, package_index)
      -- print(string.format("pp: %d %d/%d", output_index, package_index, index_count + 1))
      local package = head .. body
      package_parts_map[head] = package
      package_index = package_index + 1

      self:_output_chain_push(head, package)
      self._output_wait_package_parts_map[head] = package_parts_map
    end
  else
    local head = string.pack("<I2I2I2", output_index, 1, 1)
    local package = head .. buf
    package_parts_map[head] = package

    self:_output_chain_push(head, package)
    self._output_wait_package_parts_map[head] = package_parts_map
  end

  -- print("send_req")
  self.conn:send_req()

  return output_index
end

function apt_mt:_moretosend()
  if #(self._output_ack_package) > 0 then
    return true
  end

  if self._output_chain._head then
    return true
  end
end

function apt_mt:_mark_send_completed(head)
  if self._output_wait_package_parts_map[head] then
    self._output_wait_ack[head] = nil
    self._output_wait_package_parts_map[head] = nil
    self._output_wait_count = self._output_wait_count - 1
    -- print("_output_wait_count", self._output_wait_count)
  end
end

function apt_mt:_onack(buf)
  local output_index,count,package_index = string.unpack("<I2I2I2", buf)
  if config.debug then
    print(string.format("%s:%d\tack: %d %d/%d", self.host, self.port, output_index, package_index, count))
  end

  local map = self._output_wait_package_parts_map[buf]
  self:_mark_send_completed(buf)

  if map then
    map[buf] = nil
    if not next(map) then
      if self.onsent then
        coroutine.wrap(self.onsent)(output_index)
      end
    end
  end
end

function apt_mt:_onread(buf)
  -- print("read", self.dest, #(buf))
  if #(buf) == HEAD_SIZE then
    -- single ack.
    self:_onack(buf)
    self.conn:send_req()
    return
  elseif #(buf) > HEAD_SIZE then
    local head = string.sub(buf, 1, HEAD_SIZE)
    local body = string.sub(buf, HEAD_SIZE + 1)
    local output_index,count,package_index = string.unpack("<I2I2I2", head)

    if output_index == 0xffff and #(body)%6 == 0 then
      -- multi-ack
      local offset = 1
      while offset < #(body) do
        local line = string.sub(body, offset, offset + 5)
        self:_onack(line)
        offset = offset + 6
      end
    else
      table.insert(self._output_ack_package, head)
      self.conn:send_req()
      -- if config.debug then
      -- print("sending ack", #(head), #(self._output_ack_package))
      -- end

      if config.debug then
        print(string.format("%s:%d\trecv: [%d] %d/%d (body=%d)", self.host, self.port, output_index, package_index, count, #(body)))
      end

      local incoming_object = self._incoming_map[output_index]
      if not incoming_object then
        incoming_object = {items = {}, count = count, start = utils.gettime()}
        self._incoming_map[output_index] = incoming_object
      elseif incoming_object.done then
        print("ignore done", output_index)
        return
      end

      incoming_object.items[package_index] = body

      for i=1,count do
        if not incoming_object.items[i] then
          return
        end
      end

      -- mark true, so we can drop dup packages.
      incoming_object.done = true
      incoming_object.donetime = utils.gettime()

      if self.onread then
        if count == 1 then
          coroutine.wrap(self.onread)(incoming_object.items[1])
        else
          coroutine.wrap(self.onread)(table.concat(incoming_object.items))
        end
      end

      incoming_object.items = nil
    end
  else
    print("receive buf size too small", #(buf), fan.data2hex(buf))
  end
end

function apt_mt:_send(buf)
  -- print("send", #(buf))
  if config.debug then
    local output_index,count,package_index = string.unpack("<I2I2I2", buf)
    if dest and #(dest) > 0 then
      print(string.format("send: %s:%d\t[%d]\t%d/%d", dest[1], dest[2], output_index, package_index, count))
    else
      print(string.format("send: [%d]\t%d/%d", output_index, package_index, count))
    end
  end

  if self.dest then
    self.conn:send(buf, self.dest)
  else
    -- print("send direct", self.host, self.port)
    self.conn:send(buf, self.host, self.port)
  end

  config.udp_send_total = config.udp_send_total + 1

  -- print("send_req")
  self.conn:send_req()
end

function apt_mt:_check_timeout()
  for index,incoming_object in pairs(self._incoming_map) do
    if incoming_object.done and utils.gettime() - incoming_object.donetime > 10 then
      self._incoming_map[index] = nil
      -- elseif utils.gettime() - incoming_object.start > 120 then
      -- incoming[index] = nil
    end
  end
  local has_timeout = false
  for k,map in pairs(self._output_wait_package_parts_map) do
    local last_output_time = self._output_wait_ack[k]

    if last_output_time and gettime() - last_output_time >= TIMEOUT then
      has_timeout = true
      local resend = true
      if self.ontimeout then
        resend = self.ontimeout(map[k])
      end

      if resend then
        self._output_wait_count = self._output_wait_count - 1
        config.udp_resend_total = config.udp_resend_total + 1
        self:_output_chain_push(k, map[k])
        self._output_wait_ack[k] = nil
      else
        for k,v in pairs(map) do
          map[k] = nil
          self:_mark_send_completed(k)
        end
      end
    end
  end

  if has_timeout then
    self.conn:send_req()
  end

  return has_timeout
end

local max_ack_count = math.floor(BODY_SIZE / 6)
local multi_ack_head = string.pack("<I2I2I2", 0xffff, 1, 1)

function apt_mt:_onsendready()
  if #(self._output_ack_package) > max_ack_count then
    local tmp = {}
    local package = table.concat(self._output_ack_package, "", 1, max_ack_count)
    print("multi ack sub:", #package)
    table.move(self._output_ack_package, max_ack_count + 1, #(self._output_ack_package), 1, tmp)
    self._output_ack_package = tmp
    self:_send(multi_ack_head .. package)
    return true
  elseif #(self._output_ack_package) > 1 then
    local package = table.concat(self._output_ack_package)
    self._output_ack_package = {}
    self:_send(multi_ack_head .. package)
    return true
  elseif #(self._output_ack_package) > 0 then
    local package = table.remove(self._output_ack_package)
    self:_send(package)
    return true
  end

  if self._output_wait_count >= WAITING_COUNT then
    -- print("waiting ...")
    return false
  end

  local head,package = self:_output_chain_pop()
  if head and package then
    if self._output_wait_package_parts_map[head] then
      self._output_wait_ack[head] = gettime()
      self._output_wait_count = self._output_wait_count + 1

      -- print("_output_wait_count", self._output_wait_count)

      self:_send(package)
      return true
    end
  end

  return false
end

-- cleanup packages(ack not include) related with host,port
function apt_mt:cleanup()
  if self._parent then
    self._parent.clientmap[self._client_key] = nil
    self._parent = nil
  end

  for k,v in pairs(self) do
    self[k] = nil
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
  port = tonumber(port)

  local t = {
    host = host,
    port = port,
    _output_index = 0,
    _output_chain = {_head = nil, _tail = nil},
    _output_wait_ack = {},
    _output_wait_package_parts_map = {},
    _output_wait_count = 0,
    _output_ack_package = {},
    _incoming_map = {},
  }
  setmetatable(t, apt_mt)

  t.conn = udpd.new{
    host = host,
    port = port,
    onread = function(buf, from)
      -- print("onread", #(buf), from, host, port, type(from:getPort()), type(port))
      config.udp_receive_total = config.udp_receive_total + 1
      if from:getHost() == host and from:getPort() == port then
        t:_onread(buf)
      end
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

    obj.getapt = function(host, port, from, client_key)
      local apt = obj.clientmap[client_key]
      if not apt then
        if not host or not port then
          host = from:getHost()
          port = from:getPort()
        end

        apt = {
          host = host,
          port = port,
          dest = from,
          conn = obj.serv,
          _parent = obj,
          _output_index = 0,
          _output_chain = {_head = nil, _tail = nil},
          _output_wait_ack = {},
          _output_wait_package_parts_map = {},
          _output_wait_count = 0,
          _output_ack_package = {},
          _incoming_map = {},
          _client_key = client_key,
        }
        setmetatable(apt, apt_mt)
        obj.clientmap[client_key] = apt

        if obj.onaccept then
          coroutine.wrap(obj.onaccept)(apt)
        end
      elseif not apt.dest then
        apt.dest = from
      end

      return apt
    end
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
        local apt = obj.getapt(nil, nil, from, tostring(from))

        apt:_onread(buf)
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
