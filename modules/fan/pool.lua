local setmetatable = setmetatable
local pairs = pairs
local xpcall = xpcall
local table = table
local ipairs = ipairs
local coroutine = coroutine

local config = require "config"

local pool_mt = {}
pool_mt.__index = pool_mt

local function maxn(t)
  local n = 0
  for k, v in pairs(t) do
    if k > n then
      n = k
    end
  end

  return n
end

function pool_mt:pop()
  local pool_item

  if #(self.idle) > 0 then
    pool_item = table.remove(self.idle)
  elseif self.count and self.count > 0 and self.onnew then
    self.count = self.count - 1
    pool_item = self.onnew(self)
  else
    if not self.yielding.head then
      self.yielding.head = {value = coroutine.running()}
      self.yielding.tail = self.yielding.head
    else
      self.yielding.tail.next = {value = coroutine.running()}
      self.yielding.tail = self.yielding.tail.next
    end
    pool_item = coroutine.yield()
  end

  if not self.onbind then
    return pool_item
  else
    return self.onbind(pool_item, self.bind_args)
  end
end

function pool_mt:push(ctx)
  local pool_item
  if self.onunbind then
    pool_item = self.onunbind(ctx)
  else
    pool_item = ctx
  end

  -- If onunbind returned nil, don't put the resource back in the pool
  if pool_item == nil then
    self.count = self.count + 1  -- Increment available count
    return
  end

  if self.yielding.head then
    local co = self.yielding.head.value
    self.yielding.head = self.yielding.head.next
    if not self.yielding.head then
      self.yielding.tail = nil
    end
    local st, msg = coroutine.resume(co, pool_item)
    if not st then
      print(msg)
    end
  else
    table.insert(self.idle, pool_item)
  end
end

function pool_mt:safe(func, ...)
  local ctx = self:pop()
  local st, msg = xpcall(func, debug.traceback, ctx, ...)
  self:push(ctx)

  if not st then
    print(msg)
    return nil  -- Return nil on error
  else
    return msg
  end
end

local function new(arg_map, ...)
  local bind_args = {...}
  local obj = {
    onnew = arg_map and arg_map.onnew,
    onbind = arg_map and arg_map.onbind,
    onunbind = arg_map and arg_map.onunbind,
    bind_args = bind_args,
    map = {},
    idle = {},
    yielding = {head = nil, tail = nil},
    count = config.pool_size or 10
  }
  setmetatable(obj, pool_mt)

  return obj
end

return {
  new = new
}
