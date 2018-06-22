local tonumber = tonumber

local ffi = require("ffi")

ffi.cdef [[
typedef struct {
  size_t offset;
  size_t total;
  uint8_t *buffer;
  size_t buflen;
  bool reading;
  bool wrapbuffer;
} BYTEARRAY;

void ffi_stream_new(BYTEARRAY *ba, const char *data, size_t len);
void ffi_stream_gc(BYTEARRAY *ba);
size_t ffi_stream_available(BYTEARRAY *ba);

bool ffi_stream_mark(BYTEARRAY *ba);
bool ffi_stream_reset(BYTEARRAY *ba);

bool ffi_stream_get_u8(BYTEARRAY *ba, uint8_t *result);
bool ffi_stream_get_u16(BYTEARRAY *ba, uint16_t *result);
bool ffi_stream_get_u32(BYTEARRAY *ba, uint32_t *result);
bool ffi_stream_get_u30(BYTEARRAY *ba, uint32_t *result);
bool ffi_stream_get_s24(BYTEARRAY *ba, int32_t *result);
bool ffi_stream_get_u24(BYTEARRAY *ba, uint32_t *result);
bool ffi_stream_get_d64(BYTEARRAY *ba, double *result);
void ffi_stream_get_string(BYTEARRAY *ba, uint8_t **buff, size_t *buflen);
void ffi_stream_get_bytes(BYTEARRAY *ba, uint8_t **buff, size_t *buflen);

void ffi_stream_add_u8(BYTEARRAY *ba, uint8_t value);
void ffi_stream_add_u16(BYTEARRAY *ba, uint16_t value);
void ffi_stream_add_u30(BYTEARRAY *ba, uint32_t u);
void ffi_stream_add_u24(BYTEARRAY *ba, uint32_t u);
void ffi_stream_add_d64(BYTEARRAY *ba, double value);
void ffi_stream_add_string(BYTEARRAY *ba, const char *data, size_t len);
void ffi_stream_add_bytes(BYTEARRAY *ba, const char *data, size_t len);

void ffi_stream_package(BYTEARRAY *ba, uint8_t **buff, size_t *buflen);
void ffi_stream_prepare_get(BYTEARRAY *ba);
void ffi_stream_prepare_add(BYTEARRAY *ba);
void ffi_stream_empty(BYTEARRAY *ba);
]]

--[[
# macosx
gcc -fPIC -dynamiclib -shared stream_ffi.c bytearray.c -o libstream_ffi.dylib
]]
local stream_ffi = ffi.load("stream_ffi")
local bytearray_t = ffi.typeof("BYTEARRAY")

local stream_mt = {}
stream_mt.__index = stream_mt
stream_mt.__gc = stream_ffi.ffi_stream_gc

function stream_mt.new(data)
  ba = ffi.new(bytearray_t)
  stream_ffi.ffi_stream_new(ba, data, data and #data or 0)
  return ba
end

function stream_mt:available()
  return tonumber(stream_ffi.ffi_stream_available(self))
end

function stream_mt:package()
  local buff = ffi.new("uint8_t* [1]")
  local buflen = ffi.new("size_t [1]")
  stream_ffi.ffi_stream_package(self, buff, buflen)
  if buff[0] ~= ffi.NULL and buflen[0] > 0 then
    return ffi.string(buff[0], buflen[0])
  else
    return ""
  end
end

function stream_mt:prepare_get()
  stream_ffi.ffi_stream_prepare_get(self)
end

function stream_mt:prepare_add()
  stream_ffi.ffi_stream_prepare_add(self)
end

function stream_mt:empty()
  stream_ffi.ffi_stream_empty(self)
end

function stream_mt:mark()
  return stream_ffi.ffi_stream_mark(self)
end

function stream_mt:reset()
  return stream_ffi.ffi_stream_reset(self)
end

function stream_mt:GetU8()
  local uint8 = ffi.new("uint8_t [1]")
  if stream_ffi.ffi_stream_get_u8(self, uint8) then
    return uint8[0]
  end
end

function stream_mt:GetS24()
  local int32 = ffi.new("int32_t [1]")
  if stream_ffi.ffi_stream_get_s24(self, int32) then
    return int32[0]
  end
end

function stream_mt:GetU24()
  local uint32 = ffi.new("uint32_t [1]")
  if stream_ffi.ffi_stream_get_u24(self, uint32) then
    return uint32[0]
  end
end

function stream_mt:GetU16()
  local uint16 = ffi.new("uint16_t [1]")
  if stream_ffi.ffi_stream_get_u16(self, uint16) then
    return uint16[0]
  end
end

function stream_mt:GetU32()
  local uint32 = ffi.new("uint32_t [1]")
  if stream_ffi.ffi_stream_get_u32(self, uint32) then
    return uint32[0]
  end
end

function stream_mt:GetU30()
  local uint32 = ffi.new("uint32_t [1]")
  if stream_ffi.ffi_stream_get_u30(self, uint32) then
    return uint32[0]
  end
end

function stream_mt:GetABCS32()
  return self:GetU30()
end

function stream_mt:GetABCU32()
  return self:GetU30()
end

function stream_mt:GetD64()
  local d64 = ffi.new("double [1]")
  if stream_ffi.ffi_stream_get_d64(self, d64) then
    return d64[0]
  end
end

function stream_mt:GetBytes()
  local buff = ffi.new("uint8_t* [1]")
  local buflen = ffi.new("size_t [1]")
  stream_ffi.ffi_stream_get_bytes(self, buff, buflen)
  if buff[0] ~= ffi.NULL and buflen[0] > 0 then
    return ffi.string(buff[0], buflen[0])
  end
end

function stream_mt:GetString()
  local buff = ffi.new("uint8_t* [1]")
  local buflen = ffi.new("size_t [1]")
  stream_ffi.ffi_stream_get_string(self, buff, buflen)
  if buff[0] ~= ffi.NULL then
    return ffi.string(buff[0], buflen[0])
  else
    return nil, buflen[0]
  end
end

function stream_mt:AddU8(u)
  stream_ffi.ffi_stream_add_u8(self, u)
end

function stream_mt:AddU16(u)
  stream_ffi.ffi_stream_add_u16(self, u)
end

function stream_mt:AddS24(u)
  stream_ffi.ffi_stream_add_u24(self, u)
end

function stream_mt:AddU24(u)
  stream_ffi.ffi_stream_add_u24(self, u)
end

function stream_mt:AddU30(u)
  stream_ffi.ffi_stream_add_u30(self, u)
end

function stream_mt:AddABCU32(u)
  stream_ffi.ffi_stream_add_u30(self, u)
end

function stream_mt:AddABCS32(u)
  stream_ffi.ffi_stream_add_u30(self, u)
end

function stream_mt:AddD64(u)
  stream_ffi.ffi_stream_add_d64(self, u)
end

function stream_mt:AddBytes(u)
  stream_ffi.ffi_stream_add_bytes(self, u, #u)
end

function stream_mt:AddString(u)
  stream_ffi.ffi_stream_add_string(self, u, #u)
end

ffi.metatype(bytearray_t, stream_mt)

return {
  new = stream_mt.new
}
