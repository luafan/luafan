#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"

#define LUA_STREAM_TYPE "<fan.stream available=%d>"

#include "stream_ffi.c"

LUA_API int luafan_stream_new(lua_State *L)
{
  size_t len = 0;
  const char *data = luaL_optlstring(L, 1, NULL, &len);

  BYTEARRAY *ba = (BYTEARRAY *)lua_newuserdata(L, sizeof(BYTEARRAY));
  luaL_getmetatable(L, LUA_STREAM_TYPE);
  lua_setmetatable(L, -2);

  ffi_stream_new(ba, data, len);
  return 1;
}

LUA_API int luafan_stream_gc(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  ffi_stream_gc(ba);

  return 0;
}

LUA_API int luafan_stream_available(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  lua_pushinteger(L, ffi_stream_available(ba));
  return 1;
}

LUA_API int luafan_stream_get_u8(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint8_t result = 0;
  if (ffi_stream_get_u8(ba, &result))
  {
    lua_pushinteger(L, result);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_add_u8(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint8_t value = luaL_checkinteger(L, 2);
  ffi_stream_add_u8(ba, value);
  return 0;
}

LUA_API int luafan_stream_get_u16(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint16_t result = 0;
  if (ffi_stream_get_u16(ba, &result))
  {
    lua_pushinteger(L, result);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_add_u16(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint16_t value = luaL_checkinteger(L, 2);
  ffi_stream_add_u16(ba, value);
  return 0;
}

LUA_API int luafan_stream_get_u32(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t result = 0;
  if (ffi_stream_get_u32(ba, &result))
  {
    lua_pushinteger(L, result);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_get_u30(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t value = 0;
  if (ffi_stream_get_u30(ba, &value))
  {
    lua_pushinteger(L, value);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_add_u30(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t value = luaL_checkinteger(L, 2);
  ffi_stream_add_u30(ba, value);

  return 0;
}

LUA_API int luafan_stream_get_s24(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  int32_t result = 0;
  if (ffi_stream_get_s24(ba, &result))
  {
    lua_pushinteger(L, result);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_get_u24(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t result = 0;
  if (ffi_stream_get_u24(ba, &result))
  {
    lua_pushinteger(L, result);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_add_u24(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint32_t u = luaL_checkinteger(L, 2);
  ffi_stream_add_u24(ba, u);

  return 0;
}

LUA_API int luafan_stream_get_d64(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  double result = 0;
  if (ffi_stream_get_d64(ba, &result))
  {
    lua_pushnumber(L, result);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_add_d64(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  double value = luaL_checknumber(L, 2);
  ffi_stream_add_d64(ba, value);

  return 0;
}

LUA_API int luafan_stream_get_string(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint8_t *buff = NULL;
  size_t buflen = 0;
  ffi_stream_get_string(ba, &buff, &buflen);

  if (buff)
  {
    lua_pushlstring(L, (char *)buff, buflen);
    return 1;
  }
  else
  {
    lua_pushnil(L);
    lua_pushinteger(L, buflen);
    return 2;
  }
}

LUA_API int luafan_stream_mark(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  if (ffi_stream_mark(ba)) {
    lua_pushboolean(L, true);
    return 1;
  } else {
    return 0;
  }
}

LUA_API int luafan_stream_reset(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  if (ffi_stream_reset(ba)) {
    lua_pushboolean(L, true);
    return 1;
  } else {
    return 0;
  }
}

LUA_API int luafan_stream_get_bytes(lua_State *L)
{
  size_t buflen = luaL_optinteger(L, 2, -1);
  if (buflen == 0)
  {
    return 0;
  }

  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  uint8_t *buff = NULL;
  ffi_stream_get_bytes(ba, &buff, &buflen);
  if (buff && buflen > 0)
  {
    lua_pushlstring(L, (char *)buff, buflen);
    return 1;
  }
  else
  {
    return 0;
  }
}

LUA_API int luafan_stream_add_string(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);
  ffi_stream_add_string(ba, data, len);

  return 0;
}

LUA_API int luafan_stream_add_bytes(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);
  ffi_stream_add_bytes(ba, data, len);

  return 0;
}

LUA_API int luafan_stream_package(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  uint8_t *buff = NULL;
  size_t buflen = 0;

  ffi_stream_package(ba, &buff, &buflen);
  lua_pushlstring(L, (char *)buff, buflen);

  return 1;
}

LUA_API int luafan_stream_prepare_get(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  lua_pushboolean(L, ffi_stream_prepare_get(ba));
  return 1;
}

LUA_API int luafan_stream_prepare_add(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  lua_pushboolean(L, ffi_stream_prepare_add(ba));
  return 1;
}

LUA_API int luafan_stream_empty(lua_State *L)
{
  BYTEARRAY *ba = (BYTEARRAY *)luaL_checkudata(L, 1, LUA_STREAM_TYPE);

  lua_pushboolean(L, ffi_stream_empty(ba));
  return 1;
}

LUA_API int luafan_stream_tostring(lua_State *L)
{
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

    {"mark", luafan_stream_mark},
    {"reset", luafan_stream_reset},

    {"package", luafan_stream_package},
    {NULL, NULL},
};

LUA_API int luaopen_fan_stream_core(lua_State *L)
{
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
