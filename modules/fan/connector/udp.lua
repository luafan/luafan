local fan = require "fan"
local udpd = require "fan.udpd"
local utils = require "fan.utils"
local config = require "config"

local utils = require "fan.utils"

local string = string
local next = next
local table = table
local pairs = pairs
local math = math
local print = print
local coroutine = coroutine
local tostring = tostring

local gettime = utils.gettime

config.udp_send_total = 0
config.udp_receive_total = 0
config.udp_resend_total = 0

-- impl waiting pool
local MTU = (config.udp_mtu or 576) - 8 - 20
local HEAD_SIZE = 4 + 2 + 2
local BODY_SIZE = MTU - HEAD_SIZE
local MAX_PAYLOAD_SIZE = BODY_SIZE * 65535

local TIMEOUT = config.udp_package_timeout or 2
local WAITING_COUNT = config.udp_waiting_count or 100
local MULTI_ACK = config.multi_ack

local CHECK_TIMEOUT_DURATION = config.udp_check_timeout_duration or 0.5

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

  -- print("apt_mt:send", #(buf), MAX_PAYLOAD_SIZE)
  if #(buf) > MAX_PAYLOAD_SIZE then
    print(":send body size overlimit", #buf)
    return nil
  end

  local output_index = self._output_index + 1
  if output_index >= 0x0ffffff0 then -- preserve first 4 bit.
    output_index = 1
    if config.debug then
      print("reset output_index = 1")
    end
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
      local head = string.pack("<I4I2I2", output_index, index_count + 1, package_index)
      -- print(string.format("pp: %d %d/%d", output_index, package_index, index_count + 1))
      local package = head .. body
      package_parts_map[head] = package
      package_index = package_index + 1

      self:_output_chain_push(head, package)
      self._output_wait_package_parts_map[head] = package_parts_map
    end
  else
    local head = string.pack("<I4I2I2", output_index, 1, 1)
    local package = head .. buf
    package_parts_map[head] = package

    self:_output_chain_push(head, package)
    self._output_wait_package_parts_map[head] = package_parts_map
  end

  -- print("send_req")
  self:send_req()

  return output_index
end

function apt_mt:send_req()
  local obj = self._parent or self

  if not obj._pending_for_send then
    obj._pending_for_send = true
    self.conn:send_req()
  end
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
    self._output_wait_package_parts_map[head] = nil
    if self._output_wait_ack[head] then
      self._output_wait_count = self._output_wait_count - 1
      self._output_wait_ack[head] = nil
    end
    -- print("_output_wait_count", self._output_wait_count)
  end
end

function apt_mt:_onack(buf)
  if config.debug then
    local output_index,count,package_index = string.unpack("<I4I2I2", buf)
    print(self, string.format("recv<ack> %s:%d\t[%d] %d/%d", self.host, self.port, output_index, package_index, count))
  end

  local last_output_time = self._output_wait_ack[buf]
  if last_output_time then
    self.latency = gettime() - last_output_time
  end

  local map = self._output_wait_package_parts_map[buf]
  self:_mark_send_completed(buf)

  if map then
    map[buf] = nil
    if self.onsent then
      if not next(map) then
        local output_index = string.unpack("<I4", buf)
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
    self:send_req()
    return
  elseif #(buf) > HEAD_SIZE then
    local head = string.sub(buf, 1, HEAD_SIZE)
    local body = string.sub(buf, HEAD_SIZE + 1)
    local output_index,count,package_index = string.unpack("<I4I2I2", head)

    if output_index == 0x0fffffff and #(body)%HEAD_SIZE == 0 then
      -- multi-ack
      local offset = 1
      while offset < #(body) do
        local line = string.sub(body, offset, offset + HEAD_SIZE - 1)
        self:_onack(line)
        offset = offset + HEAD_SIZE
      end
    else
      table.insert(self._output_ack_package, head)
      if config.debug then
        print(self, "sending ack", #(self._output_ack_package), self._pending_for_send)
      end
      self:send_req()

      if config.debug then
        print(self, string.format("recv<data> %s:%d\t[%d] %d/%d (body=%d)", self.host, self.port, output_index, package_index, count, #(body)))
      end

      local incoming_object = self._incoming_map[output_index]
      if not incoming_object then
        incoming_object = {items = {}, count = count, start = gettime()}
        self._incoming_map[output_index] = incoming_object
      elseif incoming_object.done then
        if config.debug then
          print("ignore done", output_index)
        end
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
      incoming_object.donetime = gettime()

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

function apt_mt:_send(buf, kind)
  -- print("send", #(buf))
  if config.debug then
    local output_index,count,package_index = string.unpack("<I4I2I2", buf)
    print(self, string.format("send<%s> %s\t[%d] %d/%d", kind, self.dest, output_index, package_index, count))
  end

  self.conn:send(buf, self.dest)

  config.udp_send_total = config.udp_send_total + 1

  self:send_req()
  -- print("send_req")
end

function apt_mt:_check_timeout()
  for index,incoming_object in pairs(self._incoming_map) do
    if incoming_object.done and gettime() - incoming_object.donetime > 60 then
      self._incoming_map[index] = nil
    end
  end
  local has_timeout = false
  for k,map in pairs(self._output_wait_package_parts_map) do
    local last_output_time = self._output_wait_ack[k]

    if last_output_time then
      local timeout = math.max(self.latency and self.latency * 3 or TIMEOUT, 0.1)
      if gettime() - last_output_time >= timeout then
        has_timeout = true
        local resend = true
        if self.ontimeout then
          resend = self.ontimeout(map[k])
        end

        if resend and self._output_wait_ack[k] then
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
  end

  if has_timeout then
    self:send_req()
  elseif not self._pending_for_send then
    -- temp fix for packages pending for send with no more send_req
    if self:_moretosend() then
      -- print("send_req")
      self:send_req()
    end
  end

  return has_timeout
end

local max_ack_count = math.floor(BODY_SIZE / HEAD_SIZE)
local multi_ack_head = string.pack("<I4I2I2", 0x0fffffff, 1, 1)

function apt_mt:_onsendready()
  if MULTI_ACK then
    if #(self._output_ack_package) > max_ack_count then
      local tmp = {}
      local package = table.concat(self._output_ack_package, nil, 1, max_ack_count)
      -- print("multi ack sub:", #package)
      table.move(self._output_ack_package, max_ack_count + 1, #(self._output_ack_package), 1, tmp)
      self._output_ack_package = tmp
      self:_send(multi_ack_head .. package, "mack")
      return true
    elseif #(self._output_ack_package) > 1 then
      -- print("multi_ack_head", #(self._output_ack_package))
      local package = table.concat(self._output_ack_package)
      self._output_ack_package = {}
      self:_send(multi_ack_head .. package, "mack")
      return true
    end
  elseif #(self._output_ack_package) > 0 then
    local package = table.remove(self._output_ack_package)
    self:_send(package, "ack")
    return true
  end

  if self._output_wait_count >= WAITING_COUNT then
    -- print("waiting ...")
    return false
  end

  local head,package = self:_output_chain_pop()
  if head and package then
    if self._output_wait_package_parts_map[head] then
      if not self._output_wait_ack[head] then
        self._output_wait_count = self._output_wait_count + 1
      end
      self._output_wait_ack[head] = gettime()
      -- print("_output_wait_count", self._output_wait_count)

      self:_send(package, "data")
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
    dest = udpd.make_dest(host, port),
    _output_index = 0,
    _output_chain = {_head = nil, _tail = nil},
    _output_wait_ack = {},
    _output_wait_package_parts_map = {},
    _output_wait_count = 0,
    _output_ack_package = {},
    _incoming_map = {},
  }
  setmetatable(t, apt_mt)

  host = t.dest:getHost()
  port = t.dest:getPort()

  local weak_t = utils.weakify(t)

  t.conn = udpd.new{
    host = host,
    port = port,
    onread = function(buf, from)
      -- print("onread", #(buf), from, host, port, type(from:getPort()), type(port))
      config.udp_receive_total = config.udp_receive_total + 1
      
      -- connect() protection, only accept connected host/port.
      if from:getHost() == host and from:getPort() == port then
        weak_t:_onread(buf)
      end
    end,
    onsendready = function()
      weak_t._pending_for_send = nil

      if not weak_t:_onsendready() then
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
        fan.sleep(CHECK_TIMEOUT_DURATION)
      end
      end)()

    return t
  end

  local function bind(host, port, path)
    local obj = {clientmap = {}}
    local weak_obj = utils.weakify(obj)

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
          dest = from or udpd.make_dest(host, port),
          conn = obj.serv,
          _parent = weak_obj,
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
        weak_obj._pending_for_send = nil

        -- TODO: schedule, otherwise some client with heavy traffic may block others.
        for k,apt in pairs(weak_obj.clientmap) do
          if apt:_moretosend() and apt:_onsendready() then
            return
          end
        end
      end,
      onread = function(buf, from)
        config.udp_receive_total = config.udp_receive_total + 1
        local apt = weak_obj.getapt(nil, nil, from, tostring(from))

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

          fan.sleep(CHECK_TIMEOUT_DURATION)
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
