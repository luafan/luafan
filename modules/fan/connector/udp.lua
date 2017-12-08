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

local UDP_WINDOW_SIZE = config.udp_window_size or 10

math.randomseed(utils.gettime())

-- preserve first 4 bit.
local MAX_OUTPUT_INDEX = 0x0ffffff0
local MULTI_ACK_OUTPUT_INDEX = 0x0fffffff
local WINDOW_CTRL = 0x0ffffffe

local CHECK_TIMEOUT_DURATION = config.udp_check_timeout_duration or 0.5

local max_ack_count = math.floor(BODY_SIZE / HEAD_SIZE)
local multi_ack_head = string.pack("<I4I2I2", MULTI_ACK_OUTPUT_INDEX, 1, 1)
local window_ctrl_head_set = string.pack("<I4I2I2", WINDOW_CTRL, 1, 1)
local window_ctrl_head_req = string.pack("<I4I2I2", WINDOW_CTRL, 1, 2)

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

  local output_index = self._output_index

  self._output_index = output_index + 1
  
  if self._output_index >= MAX_OUTPUT_INDEX then
    self._output_index = 1
    if config.debug then
      print("reset output_index = 1")
    end   
  end
  
  local package_parts_map = {}

  if #(buf) > BODY_SIZE then
    local package_index = 1
    local index_count_f = #(buf) / BODY_SIZE
    local index_count,fractional = math.modf(index_count_f)
    if fractional == 0 then
      index_count = index_count - 1
    end

    for i=0,index_count do
      local head = string.pack("<I4I2I2", output_index, index_count + 1, package_index)
      -- print(string.format("pp: %d %d/%d", output_index, package_index, index_count + 1))
      local package = {
        head = head,
        buf = buf,
        output_index = output_index,
        body_begin = i * BODY_SIZE + 1,
        body_end = i * BODY_SIZE + BODY_SIZE
      }
      package_parts_map[head] = package
      package_index = package_index + 1

      self:_output_chain_push(head, package)
      self._output_wait_package_parts_map[head] = package_parts_map
    end
  else
    local head = string.pack("<I4I2I2", output_index, 1, 1)
    local package = {
      head = head,
      body = buf,
      output_index = output_index
    }
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
    local package = map[buf]
    map[buf] = nil

    if not next(map) then
      self._send_window_holes[package.output_index] = true
      
      while self._send_window_holes[self._send_window] do
        self._send_window_holes[self._send_window] = nil
  
        self._send_window = self._send_window + 1
        if self._send_window == MAX_OUTPUT_INDEX then
          self._send_window = 1
        end
      end

      if self.onsent then
        coroutine.wrap(self.onsent)(package.output_index)
      end
    end
  end
end

function apt_mt:_onread(buf)
  self.incoming_bytes_total = self.incoming_bytes_total + #(buf)
  
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

    if output_index == MULTI_ACK_OUTPUT_INDEX and #(body)%HEAD_SIZE == 0 then
      -- multi-ack
      local offset = 1
      while offset < #(body) do
        local line = string.sub(body, offset, offset + HEAD_SIZE - 1)
        self:_onack(line)
        offset = offset + HEAD_SIZE
      end
    elseif output_index == WINDOW_CTRL then
      if count == 1 and package_index == 1 then
        local window = string.unpack("<I4", body)
        if window < MAX_OUTPUT_INDEX then
          if config.debug then
            print(self, "set window", window)
          end
          self._recv_window = window
        else
          if config.debug then
            print(self, "window size over limit", window, MAX_OUTPUT_INDEX)
          end
        end  
      elseif count == 1 and package_index == 2 then
        if config.debug then
          print(self, "will resend window")
        end

        self._window_sent = nil
      end
    elseif output_index < MAX_OUTPUT_INDEX then
      if not self._recv_window then
        if config.debug then
          print(self, "window not set")
        end

        self:_send(window_ctrl_head_req .. string.pack("<I4", self._send_window), "wind")        
        return
      end
      if output_index - self._recv_window > UDP_WINDOW_SIZE
      and output_index + MAX_OUTPUT_INDEX - self._recv_window > UDP_WINDOW_SIZE then
        if config.debug then
          print(self, string.format("drop package outside window, win:%d pkg:%d", window, output_index))
        end
        return
      end
      
      table.insert(self._output_ack_package, head)
      if config.debug then
        print(self, "sending ack", #(self._output_ack_package), self._pending_for_send)
      end
      self:send_req()

      if config.debug then
        print(self, string.format("recv<data> %s:%d\t[%d] %d/%d (body=%d)", self.host, self.port, output_index, package_index, count, #(body)))
      end

      if self._recv_window_holes[output_index] then
        if config.debug then
          print(self, "drop dup package.")
        end
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

      self._recv_window_holes[output_index] = true
      -- print("output_index", output_index, "self._recv_window", self._recv_window)
      while self._recv_window_holes[self._recv_window] do
        self._recv_window_holes[self._recv_window] = nil

        self._recv_window = self._recv_window + 1
        if self._recv_window == MAX_OUTPUT_INDEX then
          self._recv_window = 1
        end
      end

      if self.onread then
        coroutine.wrap(self.onread)(table.concat(incoming_object.items))
      end

      incoming_object.items = nil
      self._incoming_map[output_index] = nil
    end
  else
    print("unsupported package", #(buf), fan.data2hex(buf))
  end
end

function apt_mt:_send(buf, kind)
  -- print("send", #(buf))
  if config.debug then
    local output_index,count,package_index = string.unpack("<I4I2I2", buf)
    print(self, string.format("send<%s> %s\t[%d] %d/%d", kind, self.dest, output_index, package_index, count))
  end

  local result = self.conn:send(buf, self.dest)
  if result < 0 then
    self.conn:rebind()
  end

  self.outgoing_bytes_total = self.outgoing_bytes_total + #(buf)
  
  config.udp_send_total = config.udp_send_total + 1

  self:send_req()
  -- print("send_req")
end

function apt_mt:_check_timeout()
  local has_timeout = false
  for k,map in pairs(self._output_wait_package_parts_map) do
    local last_output_time = self._output_wait_ack[k]

    if last_output_time then
      local timeout = math.min(math.max(self.latency and self.latency * 3 or TIMEOUT, 0.1), TIMEOUT)
      local difftime = gettime() - last_output_time
      if difftime >= timeout then
        has_timeout = true
        self.latency = difftime
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

function apt_mt:_onsendready()
  if not self._window_sent then
    self:_send(window_ctrl_head_set .. string.pack("<I4", self._send_window), "wind")
    self._window_sent = true
    return true
  end
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
    if package.output_index - self._send_window > UDP_WINDOW_SIZE
    and package.output_index + MAX_OUTPUT_INDEX - self._send_window > UDP_WINDOW_SIZE then
      self:_output_chain_push(head, package)
      return false
    end
    if self._output_wait_package_parts_map[head] then
      if not self._output_wait_ack[head] then
        self._output_wait_count = self._output_wait_count + 1
      end
      self._output_wait_ack[head] = gettime()
      -- print("_output_wait_count", self._output_wait_count)
      if package.body then
        self:_send(package.head .. package.body, "data")
      else
        local body = string.sub(package.buf, package.body_begin, package.body_end)
        self:_send(package.head .. body, "data")
      end
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
    _output_index = math.random(MAX_OUTPUT_INDEX),
    _output_chain = {_head = nil, _tail = nil},
    _output_wait_ack = {},
    _output_wait_package_parts_map = {},
    _output_wait_count = 0,
    _output_ack_package = {},
    _incoming_map = {},
    _recv_window_holes = {},
    _send_window_holes = {},
    incoming_bytes_total = 0,
    outgoing_bytes_total = 0,
  }

  t._send_window = t._output_index
  
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
      local obj = weak_obj
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
          _parent = obj,
          _output_index = math.random(MAX_OUTPUT_INDEX),
          _output_chain = {_head = nil, _tail = nil},
          _output_wait_ack = {},
          _output_wait_package_parts_map = {},
          _output_wait_count = 0,
          _output_ack_package = {},
          _incoming_map = {},
          _client_key = client_key,
          _recv_window_holes = {},
          _send_window_holes = {},
          incoming_bytes_total = 0,
          outgoing_bytes_total = 0,
        }

        apt._send_window = apt._output_index

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
        local obj = weak_obj
        obj._pending_for_send = nil

        -- TODO: schedule, otherwise some client with heavy traffic may block others.
        for k,apt in pairs(obj.clientmap) do
          if apt:_moretosend() and apt:_onsendready() then
            return
          end
        end
      end,
      onread = function(buf, from)
        -- if math.random(100) < 10 then
        --   return
        -- end
        local obj = weak_obj
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
