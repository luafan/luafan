#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"

#define LUA_STREAM_TYPE "<fan.stream available=%d>"

LUA_API int luafan_stream_new(lua_State *L) {
  size_t len = 0;
  const char *data = luaL_optlstring(L, 1, NULL, &len);

  BYTEARRAY *ba = (BYTEARRAY *)lua_newuserdata(L, sizeof(BYTEARRAY));
  luaL_getmetatable(L, LUA_STREAM_TYPE);
  lua_setmetatable(L, -2);

  if (data && len > 0) {
    bytearray_alloc(ba, len);
    bytearray_writebuffer(ba, data, len);
    bytearray_read_ready(ba);
  } else {
    bytearray_alloc(ba, 0);
  }

  return 1;
}

LUA_API int luafan_stream_gc(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  bytearray_dealloc(ba);

  return 0;
}

LUA_API int luafan_stream_available(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  lua_pushinteger(L, bytearray_read_available(ba));
  return 1;
}

LUA_API int luafan_stream_get_u8(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint8_t value;
  bytearray_read8(ba, &value);

  lua_pushinteger(L, value);
  return 1;
}

LUA_API int luafan_stream_add_u8(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint8_t value = luaL_checkinteger(L, 2);
  bytearray_write8(ba, value);
  return 0;
}

LUA_API int luafan_stream_get_u16(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint16_t value;
  bytearray_read16(ba, &value);

  lua_pushinteger(L, value);
  return 1;
}

LUA_API int luafan_stream_add_u16(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint16_t value = luaL_checkinteger(L, 2);
  bytearray_write16(ba, value);
  return 0;
}

LUA_API int luafan_stream_get_u32(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t value;
  bytearray_read32(ba, &value);

  lua_pushinteger(L, value);
  return 1;
}

static uint32_t _luafan_stream_get_u30(BYTEARRAY *ba) {
  uint8_t b;
  uint32_t value = 0;
  uint8_t shift = 0;

  while (true) {
    bytearray_read8(ba, &b);
    value |= ((b & 127) << shift);
    shift += 7;

    if ((b & 128) == 0 || shift > 30) {
      break;
    }
  }

  return value;
}

LUA_API int luafan_stream_get_u30(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  lua_pushinteger(L, _luafan_stream_get_u30(ba));
  return 1;
}

static void _luafan_stream_add_u30(BYTEARRAY *ba, uint32_t u) {
  do {
    bytearray_write8(ba, ((u & ~0x7f) != 0 ? 0x80 : 0) | (u & 0x7F));
    u = u >> 7;
  } while (u != 0);
}

LUA_API int luafan_stream_add_u30(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t value = luaL_checkinteger(L, 2);
  _luafan_stream_add_u30(ba, value);

  return 0;
}

LUA_API int luafan_stream_get_s24(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  uint8_t value[3];
  bytearray_readbuffer(ba, value, 3);

  if (value[2] & 0x80) {
    lua_pushinteger(
        L, -1 - ((value[2] << 16 | value[1] << 8 | value[0]) ^ 0xffffff));
  } else {
    lua_pushinteger(L, value[2] << 16 | value[1] << 8 | value[0]);
  }

  return 1;
}

LUA_API int luafan_stream_get_u24(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  uint8_t value[3];
  bytearray_readbuffer(ba, value, 3);

  lua_pushinteger(L, value[2] << 16 | value[1] << 8 | value[0]);
  return 1;
}

LUA_API int luafan_stream_add_u24(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t u = luaL_checkinteger(L, 2);
  uint8_t value[3];
  value[2] = (u >> 16) & 0xff;
  value[1] = (u >> 8) & 0xff;
  value[0] = u & 0xff;
  bytearray_writebuffer(ba, value, 3);

  return 0;
}

LUA_API int luafan_stream_get_d64(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  double value;
  bytearray_read64d(ba, &value);

  lua_pushnumber(L, value);
  return 1;
}

LUA_API int luafan_stream_add_d64(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  double value = luaL_checknumber(L, 2);
  bytearray_write64d(ba, value);

  return 0;
}

LUA_API int luafan_stream_get_string(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  size_t offset = ba->offset;
  uint32_t len = _luafan_stream_get_u30(ba);
  size_t available = bytearray_read_available(ba);

  if (len > available) {
    // reset offset.
    size_t diff = ba->offset - offset;
    ba->offset = offset;
    lua_pushnil(L);
    lua_pushinteger(L, len + diff);
    return 2;
  }

  char *buff = malloc(len);
  bytearray_readbuffer(ba, buff, len);
  lua_pushlstring(L, buff, len);
  free(buff);

  return 1;
}

LUA_API int luafan_stream_get_bytes(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  size_t available = bytearray_read_available(ba);
  uint32_t len = luaL_optinteger(L, 2, available);
  len = len > available ? available : len;

  char *buff = malloc(len);
  memset(buff, 0, len);
  bytearray_readbuffer(ba, buff, len);
  lua_pushlstring(L, buff, len);
  free(buff);

  return 1;
}

LUA_API int luafan_stream_add_string(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);
  _luafan_stream_add_u30(ba, len);
  bytearray_writebuffer(ba, data, len);

  return 0;
}

LUA_API int luafan_stream_add_bytes(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);
  bytearray_writebuffer(ba, data, len);

  return 0;
}

LUA_API int luafan_stream_package(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  bytearray_read_ready(ba);
  lua_pushlstring(L, (char *)ba->buffer, ba->total);
  bytearray_write_ready(ba);

  return 1;
}

LUA_API int luafan_stream_prepare_get(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  lua_pushboolean(L, bytearray_read_ready(ba));
  return 1;
}

LUA_API int luafan_stream_prepare_add(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  lua_pushboolean(L, bytearray_write_ready(ba));
  return 1;
}

LUA_API int luafan_stream_empty(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  lua_pushboolean(L, bytearray_empty(ba));
  return 1;
}

LUA_API int luafan_stream_tostring(lua_State *L) {
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  lua_pushfstring(L, LUA_STREAM_TYPE, bytearray_read_available(ba));
  return 1;
}

static const struct luaL_Reg streamlib[] = {
    {"new", luafan_stream_new}, {NULL, NULL},
};

static const struct luaL_Reg streammtlib[] = {
    {"prepare_get", luafan_stream_prepare_get},
    {"prepare_add", luafan_stream_prepare_add},
    {"empty", luafan_stream_empty},
    {"available", luafan_stream_available},
    {"GetU8", luafan_stream_get_u8},
    {"GetS24", luafan_stream_get_s24},
    {"GetU24", luafan_stream_get_u24},
    {"GetU16", luafan_stream_get_u16},
    {"GetU32", luafan_stream_get_u32},

    {"GetU30", luafan_stream_get_u30},
    {"GetABCS32", luafan_stream_get_u30},
    {"GetABCU32", luafan_stream_get_u30},

    {"GetD64", luafan_stream_get_d64},
    {"GetBytes", luafan_stream_get_bytes},
    {"GetString", luafan_stream_get_string},

    {"AddU8", luafan_stream_add_u8},
    {"AddU16", luafan_stream_add_u16},
    {"AddS24", luafan_stream_add_u24},
    {"AddU24", luafan_stream_add_u24},

    {"AddU30", luafan_stream_add_u30},
    {"AddABCU32", luafan_stream_add_u30},
    {"AddABCS32", luafan_stream_add_u30},

    {"AddD64", luafan_stream_add_d64},
    {"AddBytes", luafan_stream_add_bytes},
    {"AddString", luafan_stream_add_string},

    {"package", luafan_stream_package},
    {NULL, NULL},
};

LUA_API int luaopen_fan_stream(lua_State *L) {
  luaL_newmetatable(L, LUA_STREAM_TYPE);
  luaL_register(L, NULL, streammtlib);

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pushstring(L, "__tostring");
  lua_pushcfunction(L, &luafan_stream_tostring);
  lua_rawset(L, -3);

  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, &luafan_stream_gc);
  lua_rawset(L, -3);

  lua_pop(L, 1);

  lua_newtable(L);
  luaL_register(L, "stream", streamlib);
  return 1;
}
