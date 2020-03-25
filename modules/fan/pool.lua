local setmetatable = setmetatable
local pairs = pairs
local xpcall = xpcall
local table = table
local ipairs = ipairs
local coroutine = coroutine

local config = require "config"

local pool_mt = {}
pool_mt.__index = pool_mt

local pool = {idle = {}, yielding = {head = nil, tail = nil}, count = config.pool_size or 10}

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

  if #(pool.idle) > 0 then
    pool_item = table.remove(pool.idle)
  elseif pool.count and pool.count > 0 and self.onnew then
    pool.count = pool.count - 1
    pool_item = self.onnew(self)
  else
    if not pool.yielding.head then
      pool.yielding.head = {value = coroutine.running()}
      pool.yielding.tail = pool.yielding.head
    else
      pool.yielding.tail.next = {value = coroutine.running()}
      pool.yielding.tail = pool.yielding.tail.next
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
  local pool_item = self.onunbind and self.onunbind(ctx) or ctx

  if pool.yielding.head then
    local co = pool.yielding.head.value
    pool.yielding.head = pool.yielding.head.next
    if not pool.yielding.head then
      self.pool.yielding.tail = nil
    end
    local st, msg = coroutine.resume(co, pool_item)
    if not st then
      print(msg)
    end
  else
    table.insert(pool.idle, pool_item)
  end
end

function pool_mt:safe(func, ...)
  local ctx = self:pop()
  local st, msg = xpcall(func, debug.traceback, ctx, ...)
  self:push(ctx)

  if not st then
    print(msg)
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
    map = {}
  }
  setmetatable(obj, pool_mt)

  return obj
end

return {
  new = new
}
