local table = table
local assert = assert
local string = string

local bnot = bit.bnot
local band, bor, bxor = bit.band, bit.bor, bit.bxor
local lshift, rshift, rol = bit.lshift, bit.rshift, bit.rol

local stream_mt = {}
stream_mt.__index = stream_mt

function stream_mt:available()
  return #(self.data) - self.offset + 1
end

function stream_mt:GetU8()
  local b = self.data:byte(self.offset)
  self.offset = self.offset + 1
  return b
end

function stream_mt:GetS24()
  local b1 = self:GetU8()
  local b2 = self:GetU8()
  local b3 = self:GetU8()
  if band(b3, 0x80) ~= 0 then
    return -1 - bxor(bor(lshift(b3, 16), lshift(b2, 8), b1), 0xffffff)
  else
    return bor(lshift(b3, 16), lshift(b2, 8), b1)
  end
end

function stream_mt:getOffset()
  return self.offset
end

function stream_mt:setOffset(v)
  self.offset = v
end

function stream_mt:GetU16()
  local a, b = string.byte(self.data, self.offset, self.offset + 1)
  self.offset = self.offset + 2
  return a + lshift(b, 8)
end

function stream_mt:GetU24()
  local a, b, c = string.byte(self.data, self.offset, self.offset + 2)
  self.offset = self.offset + 3
  return a + lshift(b, 8) + lshift(c, 16)
end

function stream_mt:GetU32()
  local a, b, c, d = string.byte(self.data, self.offset, self.offset + 3)
  self.offset = self.offset + 4
  return a + lshift(b, 8) + lshift(c, 16) + d * 2 ^ 24
end

function stream_mt:GetU30()
  local index = 1
  local value = 0
  local shift = 0

  for i = 1, 5 do
    local b = self:GetU8()
    if not b then
      return nil
    end
    value = value + band(b, 127) * 2 ^ shift
    shift = shift + 7

    if band(b, 128) == 0 or shift >= 32 then
      break
    end
  end

  -- if value > 10000 then
  -- print(value)
  -- end

  return value
end

function stream_mt:GetABCS32()
  return self:GetABCU32()
end

function stream_mt:GetABCU32()
  return self:GetU30()
end

function stream_mt:GetD64()
  local s = string.sub(self.data, self.offset, self.offset + 7)
  self.offset = self.offset + 8
  return (string.unpack("<d", s))
end

function stream_mt:GetString()
  local offset = self.offset
  local len = self:GetU30()
  if not len or len > self:available() then
    local diff = self.offset - offset
    self.offset = offset
    return nil, len and len + diff or self:available() + 1
  else
    local s = string.sub(self.data, self.offset, self.offset + len - 1)
    self.offset = self.offset + len
    return s
  end
end

function stream_mt:AddString(s)
  self:AddU30(#(s))
  self.data = self.data .. s
end

function stream_mt:AddU8(u)
  self.data = self.data .. string.format("%c", u)
end

function stream_mt:AddU16(u)
  local s = string.format("%c%c", band(u, 0xff), band(rshift(u, 8), 0xff))
  self.data = self.data .. s
end

function stream_mt:AddU24(u)
  local s = string.format("%c%c%c", band(u, 0xff), band(rshift(u, 8), 0xff), band(rshift(u, 16), 0xff))
  self.data = self.data .. s
end

function stream_mt:AddU32(u)
  local s =
    string.format(
    "%c%c%c%c",
    band(u, 0xff),
    band(rshift(u, 8), 0xff),
    band(rshift(u, 16), 0xff),
    band(rshift(u, 24), 0xff)
  )
  self.data = self.data .. s
end

function stream_mt:AddS24(u)
  self:AddU24(u)
end

function stream_mt:AddU30(u)
  for i = 1, 5 do
    local mask = u >= 2 ^ 7 and 0x80 or 0
    self:AddU8(bor(mask, band(u, 0x7F)))

    u = rshift(u, 7)

    if u == 0 then
      break
    end
  end
end

function stream_mt:AddABCU32(u)
  self:AddU30(u)
end

function stream_mt:AddABCS32(u)
  self:AddABCU32(u)
end

function stream_mt:AddD64(d)
  local s = string.pack("<d", d)
  self.data = self.data .. s
end

function stream_mt:AddBytes(s)
  self.data = self.data .. s
end

function stream_mt:GetBytes(len)
  local available = self:available()
  if not len or len > available then
    len = available
  end

  if len <= 0 then
    return nil
  end

  local s = string.sub(self.data, self.offset, self.offset + len - 1)
  self.offset = self.offset + len

  return s
end

function stream_mt:package()
  return self.data
end

function stream_mt:prepare_get()
  if self.rw == "w" then
    self.length = #(self.data)
    self.offset = 1
    self.rw = "r"
  end
end

function stream_mt:prepare_add()
  if self.rw == "r" then
    self.data = string.sub(self.data, self.offset)
    self.rw = "w"
    self.offset_mark = 0
  end
end

function stream_mt:mark()
  if self.rw == "r" then
    self.offset_mark = self.offset
    return true
  end
end

function stream_mt:reset()
  if self.rw == "r" then
    self.offset = self.offset_mark
    return true
  end
end

local function stream_mt_new(data)
  if data then
    local obj = {data = data, offset = 1, length = #(data), rw = "r"}
    setmetatable(obj, stream_mt)
    return obj
  else
    local obj = {data = "", offset = 1, length = 0, rw = "w"}
    setmetatable(obj, stream_mt)
    return obj
  end
end

return {
  ["new"] = stream_mt_new
}
