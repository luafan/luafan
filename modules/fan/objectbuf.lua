local stream = require "fan.stream"
local type = type
local table = table
local pairs = pairs
local ipairs = ipairs
local math = math
local string = string
local load = load

local HAS_NUMBER_MASK = 2^7
local HAS_U30_MASK = 2^6
local HAS_STRING_MASK = 2^5
local HAS_FUNCTION_MASK = 2^4
local HAS_TABLE_MASK = 2^3

local packer = {}

packer["boolean"] = function(ctx, obj)
  -- no need any packer job.
end

packer["object"] = function(ctx, obj)
  local p = packer[type(obj)]
  if p then
    p(ctx, obj)
  else
    error(string.format("unsupported pack type '%s' => '%s'", type(obj), obj))
  end
end

packer["table"] = function(ctx, tb)
  if ctx.table_index[tb] then
    return
  end

  table.insert(ctx.tables, tb)
  ctx.table_index[tb] = #(ctx.tables)

  for k,v in pairs(tb) do
    packer.object(ctx, k)
    packer.object(ctx, v)
  end
end

packer["number"] = function(ctx, num)
  if ctx.number_index[num] then
    return
  end

  if math.floor(num) ~= num or num >= 2^32 or num < 0 then
    table.insert(ctx.numbers, num)
    ctx.number_index[num] = #(ctx.numbers)
  else
    table.insert(ctx.integer_u30s, num)
    ctx.number_index[num] = #(ctx.integer_u30s)
  end
end

packer["string"] = function(ctx, str)
  if ctx.string_index[str] then
    return
  end

  table.insert(ctx.strings, str)
  ctx.string_index[str] = #(ctx.strings)
end

packer["function"] = function(ctx, func)
  if ctx.function_index[func] then
    return
  end

  table.insert(ctx.functions, func)
  ctx.function_index[func] = #(ctx.functions)
end

local function decode(buf, sym)
  local input = stream.new(buf)
  local flag = input:GetU8() -- in order to support both lua5.1 and 5.3, we can't use bit op.
  local index_map = {}
  local index = sym and sym.index or 2
  local sym_map_vk = sym and sym.map_vk or {}
  local last_top = index + 1

  if not sym then
    index_map[1] = false
    index_map[2] = true
  end

  local has_number = flag >= HAS_NUMBER_MASK
  if has_number then
    last_top = index + 1
    flag = flag - HAS_NUMBER_MASK
    local count = input:GetU30()
    for i=1,count do
      index_map[index + i] = input:GetD64()
    end
    index = index + count
  end

  local has_u30 = flag >= HAS_U30_MASK
  if has_u30 then
    last_top = index + 1
    flag = flag - HAS_U30_MASK
    local count = input:GetU30()
    for i=1,count do
      index_map[index + i] = input:GetU30()
    end
    index = index + count
  end

  local has_string = flag >= HAS_STRING_MASK
  if has_string then
    last_top = index + 1
    flag = flag - HAS_STRING_MASK
    local count = input:GetU30()
    for i=1,count do
      index_map[index + i] = input:GetString()
    end
    index = index + count
  end

  local has_function = flag >= HAS_FUNCTION_MASK
  if has_function then
    last_top = index + 1
    flag = flag - HAS_FUNCTION_MASK
    local count = input:GetU30()
    for i=1,count do
      index_map[index + i] = load(input:GetString(), nil, "b", _ENV)
    end
    index = index + count
  end

  local has_table = flag >= HAS_TABLE_MASK
  if has_table then
    last_top = index + 1
    flag = flag - HAS_TABLE_MASK
    local count = input:GetU30()
    for i=1,count do
      index_map[index + i] = {}
    end
    for i=1,count do
      local d = stream.new(input:GetString())
      local t = index_map[index + i]
      while d:available() > 0 do
        local ki = d:GetU30()
        local vi = d:GetU30()
        local k = sym_map_vk[ki] or index_map[ki]
        local v = sym_map_vk[vi] or index_map[vi]
        t[k] = v
      end
    end
  end

  return sym_map_vk[last_top] or index_map[last_top] -- refer to the first of table item
end

local function symbol(cls)
  local ctx = {tables = {}, numbers = {}, integer_u30s = {}, strings = {}, functions = {}, table_index = {}, number_index = {}, string_index = {}, function_index = {}}
  packer.object(ctx, cls)

  local index_map = {}
  local index = 2

  index_map[false] = 1
  index_map[true] = 2

  table.sort(ctx.strings, function(a,b)
      return a < b
    end)

  for i,v in ipairs(ctx.strings) do
    index_map[v] = index + i
  end

  index = index + #(ctx.strings)

  table.sort(ctx.numbers, function(a,b)
      return a < b
    end)

  for i,v in ipairs(ctx.numbers) do
    index_map[v] = index + i
  end

  index = index + #(ctx.numbers)

  table.sort(ctx.integer_u30s, function(a,b)
      return a < b
    end)

  for i,v in ipairs(ctx.integer_u30s) do
    index_map[v] = index + i
  end

  index = index + #(ctx.integer_u30s)

  local index_map_vk = {}
  for k,v in pairs(index_map) do
    index_map_vk[v] = k
  end

  return {map = index_map, map_vk = index_map_vk, index = index}
end

local function encode(obj, sym)
  local ctx = {tables = {}, numbers = {}, integer_u30s = {}, strings = {}, functions = {}, table_index = {}, number_index = {}, string_index = {}, function_index = {}}
  packer.object(ctx, obj)

  local flag = 0
  local bodystream = stream.new()
  local index_map = {}
  local sym_map = sym and sym.map or {}
  local index = sym and sym.index or 2

  index_map[false] = 1
  index_map[true] = 2

  if #(ctx.numbers) > 0 then
    flag = flag + HAS_NUMBER_MASK
    local realcount = 0
    local d = stream.new()
    for i,v in ipairs(ctx.numbers) do
      if not sym_map[v] then
        d:AddD64(v)
        index_map[v] = index + realcount + 1
        realcount = realcount + 1
      end
    end
    bodystream:AddU30(realcount)
    bodystream:AddBytes(d:package())
    index = index + realcount
  end
  if #(ctx.integer_u30s) > 0 then
    flag = flag + HAS_U30_MASK
    local realcount = 0
    local d = stream.new()
    for i,v in ipairs(ctx.integer_u30s) do
      if not sym_map[v] then
        d:AddU30(v)
        index_map[v] = index + realcount + 1
        realcount = realcount + 1
      end
    end
    bodystream:AddU30(realcount)
    bodystream:AddBytes(d:package())
    index = index + realcount
  end
  if #(ctx.strings) > 0 then
    flag = flag + HAS_STRING_MASK
    local realcount = 0
    local d = stream.new()
    for i,v in ipairs(ctx.strings) do
      if not sym_map[v] then
        d:AddString(v)
        index_map[v] = index + realcount + 1
        realcount = realcount + 1
      end
    end

    bodystream:AddU30(realcount)
    bodystream:AddBytes(d:package())
    index = index + realcount
  end
  if #(ctx.functions) > 0 then
    flag = flag + HAS_FUNCTION_MASK
    bodystream:AddU30(#(ctx.functions))
    for i,v in ipairs(ctx.functions) do
      bodystream:AddString(string.dump(v))
      index_map[v] = index + i
    end
  end
  index = index + #(ctx.functions)
  if (#ctx.tables) > 0 then
    flag = flag + HAS_TABLE_MASK
    bodystream:AddU30(#(ctx.tables))
    for i,v in ipairs(ctx.tables) do
      index_map[v] = index + i
    end
    for i,tb in ipairs(ctx.tables) do
      local d = stream.new()
      for k,v in pairs(tb) do
        d:AddU30(sym_map[k] or index_map[k])
        d:AddU30(sym_map[v] or index_map[v])
      end
      bodystream:AddString(d:package())
    end
  end

  return string.format("%c", flag) .. bodystream:package()
end

return {
  encode = encode,
  decode = decode,
  symbol = symbol,
}
