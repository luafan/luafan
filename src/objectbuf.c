#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"

#define HAS_NUMBER_MASK 1 << 7
#define HAS_U30_MASK 1 << 6
#define HAS_STRING_MASK 1 << 5
#define HAS_FUNCTION_MASK 1 << 4
#define HAS_TABLE_MASK 1 << 3
/* if none of the above mask was set, that means it's boolean value. */
#define TRUE_FALSE_MASK 1 << 0

#define MAX_U30 4294967296 // 2^32

#define CTX_INDEX_TABLES 1
#define CTX_INDEX_NUMBERS 2
#define CTX_INDEX_STRINGS 3
#define CTX_INDEX_FUNCS 4
#define CTX_INDEX_U30S 5
#define CTX_INDEX_TABLE_IDXS 6
#define CTX_INDEX_NUM_IDXS 7
#define CTX_INDEX_STRING_IDXS 8
#define CTX_INDEX_FUNC_IDXS 9

#define SYM_INDEX_MAP 1
#define SYM_INDEX_MAP_VK 2
#define SYM_INDEX_INDEX 3

#define FALSE_INDEX 1
#define TRUE_INDEX 2

typedef struct
{
  lua_Integer table_count;
  lua_Integer number_count;
  lua_Integer u30_count;
  lua_Integer string_count;
  lua_Integer func_count;

  int index;
} CTX;

void ffi_stream_add_u30(BYTEARRAY *ba, uint32_t u);
void ffi_stream_add_d64(BYTEARRAY *ba, double value);
void ffi_stream_add_string(BYTEARRAY *ba, const char *data, size_t len);
void ffi_stream_add_bytes(BYTEARRAY *ba, const char *data, size_t len);

bool ffi_stream_get_u30(BYTEARRAY *ba, uint32_t *result);
bool ffi_stream_get_d64(BYTEARRAY *ba, double *result);
void ffi_stream_get_string(BYTEARRAY *ba, uint8_t **buff, size_t *buflen);

static void packer(lua_State *L, CTX *ctx, int obj_index);

static void packer_number(lua_State *L, CTX *ctx, int obj_index)
{
  lua_Number value = lua_tonumber(L, obj_index);

  lua_rawgeti(L, ctx->index, CTX_INDEX_NUM_IDXS);
  lua_pushvalue(L, obj_index);
  lua_rawget(L, -2);

  if (lua_isnil(L, -1))
  {
    if (floor(value) != value || value >= MAX_U30 || value < 0)
    {
      lua_rawgeti(L, ctx->index, CTX_INDEX_NUMBERS);
      lua_pushvalue(L, obj_index);
      lua_rawseti(L, -2, ++ctx->number_count);

      lua_rawgeti(L, ctx->index, CTX_INDEX_NUM_IDXS);
      lua_pushvalue(L, obj_index);
      lua_pushinteger(L, ctx->number_count);
      lua_rawset(L, -3);

      // pop CTX_INDEX_NUMBERS & CTX_INDEX_NUM_IDXS
      lua_pop(L, 2);
    }
    else
    {
      // table.insert(ctx.integer_u30s, num)
      // ctx.number_index[num] = #(ctx.integer_u30s)

      lua_rawgeti(L, ctx->index, CTX_INDEX_U30S);
      lua_pushvalue(L, obj_index);
      lua_rawseti(L, -2, ++ctx->u30_count);

      lua_rawgeti(L, ctx->index, CTX_INDEX_NUM_IDXS);
      lua_pushvalue(L, obj_index);
      lua_pushinteger(L, ctx->u30_count);
      lua_rawset(L, -3);

      // pop CTX_INDEX_U30S & CTX_INDEX_NUM_IDXS
      lua_pop(L, 2);
    }
  }

  lua_pop(L, 2);
}

static void packer_string(lua_State *L, CTX *ctx, int obj_index)
{
  lua_rawgeti(L, ctx->index, CTX_INDEX_STRING_IDXS);
  lua_pushvalue(L, obj_index);
  lua_rawget(L, -2);

  if (lua_isnil(L, -1))
  {
    lua_rawgeti(L, ctx->index, CTX_INDEX_STRINGS);
    lua_pushvalue(L, obj_index);
    lua_rawseti(L, -2, ++ctx->string_count);

    lua_rawgeti(L, ctx->index, CTX_INDEX_STRING_IDXS);
    lua_pushvalue(L, obj_index);
    lua_pushinteger(L, ctx->string_count);
    lua_rawset(L, -3);

    // pop CTX_INDEX_STRINGS & CTX_INDEX_STRING_IDXS
    lua_pop(L, 2);
  }

  lua_pop(L, 2);
}

static void packer_function(lua_State *L, CTX *ctx, int obj_index)
{
  lua_rawgeti(L, ctx->index, CTX_INDEX_FUNC_IDXS);
  lua_pushvalue(L, obj_index);
  lua_rawget(L, -2);

  if (lua_isnil(L, -1))
  {
    lua_rawgeti(L, ctx->index, CTX_INDEX_FUNCS);
    lua_pushvalue(L, obj_index);
    lua_rawseti(L, -2, ++ctx->func_count);

    lua_rawgeti(L, ctx->index, CTX_INDEX_FUNC_IDXS);
    lua_pushvalue(L, obj_index);
    lua_pushinteger(L, ctx->func_count);
    lua_rawset(L, -3);

    // pop CTX_INDEX_FUNCS & CTX_INDEX_FUNC_IDXS
    lua_pop(L, 2);
  }

  lua_pop(L, 2);
}

static void packer_table(lua_State *L, CTX *ctx, int obj_index)
{
  lua_rawgeti(L, ctx->index, CTX_INDEX_TABLE_IDXS);
  lua_pushvalue(L, obj_index);
  lua_rawget(L, -2);

  if (lua_isnil(L, -1))
  {
    lua_rawgeti(L, ctx->index, CTX_INDEX_TABLES);
    lua_pushvalue(L, obj_index);
    lua_rawseti(L, -2, ++ctx->table_count);

    lua_rawgeti(L, ctx->index, CTX_INDEX_TABLE_IDXS);
    lua_pushvalue(L, obj_index);
    lua_pushinteger(L, ctx->table_count);
    lua_rawset(L, -3);

    // pop CTX_INDEX_TABLES & CTX_INDEX_TABLE_IDXS
    lua_pop(L, 2);
  }
  else
  {
    lua_pop(L, 2);
    return;
  }

  lua_pop(L, 2);

  lua_pushnil(L);
  while (lua_next(L, obj_index) != 0)
  {
    int value_idx = lua_gettop(L);
    int key_idx = lua_gettop(L) - 1;

    packer(L, ctx, key_idx);
    packer(L, ctx, value_idx);
    lua_pop(L, 1);
  }
}

static void packer(lua_State *L, CTX *ctx, int obj_index)
{
  switch (lua_type(L, obj_index))
  {
  case LUA_TTABLE:
    packer_table(L, ctx, obj_index);
    break;
  case LUA_TBOOLEAN:
    // no need any packer job.
    break;
  case LUA_TSTRING:
    packer_string(L, ctx, obj_index);
    break;
  case LUA_TNUMBER:
    packer_number(L, ctx, obj_index);
    break;
  case LUA_TFUNCTION:
    packer_function(L, ctx, obj_index);
    break;
  default:
    break;
  }
}

static void ctx_init(CTX *ctx, lua_State *L)
{
  lua_newtable(L);
  ctx->index = lua_gettop(L);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_TABLES);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_NUMBERS);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_U30S);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_STRINGS);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_FUNCS);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_TABLE_IDXS);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_NUM_IDXS);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_STRING_IDXS);

  lua_newtable(L);
  lua_rawseti(L, ctx->index, CTX_INDEX_FUNC_IDXS);
}

LUA_API int luafan_objectbuf_encode(lua_State *L)
{
  int obj_index = 1;
  int sym_idx = 0;
  if (lua_isnoneornil(L, 1))
  {
    luaL_error(L, "no argument.");
    return 0;
  }
  if (lua_istable(L, 2))
  {
    sym_idx = 2;
  }

  if (lua_isboolean(L, 1))
  {
    int value = lua_toboolean(L, 1);
    lua_pushstring(L, value ? "\x01" : "\x00");
    return 1;
  }

  CTX ctx = {0};
  ctx_init(&ctx, L);

  packer(L, &ctx, obj_index);

  uint8_t flag = 0;
  BYTEARRAY bodystream;
  bytearray_alloc(&bodystream, 64);
  bytearray_write8(&bodystream, flag); // place holder.

  lua_newtable(L);
  int index_map_idx = lua_gettop(L);
  uint32_t index = 2;

  if (!sym_idx)
  {
    lua_newtable(L);
  }
  else
  {
    lua_rawgeti(L, sym_idx, SYM_INDEX_INDEX);
    index = lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_rawgeti(L, sym_idx, SYM_INDEX_MAP);
  }
  int sym_map_idx = lua_gettop(L);

  lua_pushboolean(L, false);
  lua_pushinteger(L, FALSE_INDEX);
  lua_rawset(L, index_map_idx);

  lua_pushboolean(L, true);
  lua_pushinteger(L, TRUE_INDEX);
  lua_rawset(L, index_map_idx);

  // ---------------------------------------------------------------------------
  if (ctx.number_count > 0)
  {
    flag |= HAS_NUMBER_MASK;
    uint32_t realcount = 0;

    BYTEARRAY d;
    bytearray_alloc(&d, 0);

    lua_rawgeti(L, ctx.index, CTX_INDEX_NUMBERS);
    int i = 1;
    for (; i <= ctx.number_count; i++)
    {
      lua_rawgeti(L, -1, i);

      lua_pushvalue(L, -1);
      lua_rawget(L, sym_map_idx);
      if (lua_isnil(L, -1))
      {
        lua_pop(L, 1);
        ffi_stream_add_d64(&d, lua_tonumber(L, -1));
        lua_pushinteger(L, index + (++realcount));
        lua_rawset(L, index_map_idx);
      }
      else
      {
        lua_pop(L, 2); // pop sym number idx & number
      }
    }
    lua_pop(L, 1);

    ffi_stream_add_u30(&bodystream, realcount);
    bytearray_read_ready(&d);
    bytearray_writebuffer(&bodystream, d.buffer, d.total);

    bytearray_dealloc(&d);

    index += realcount;
  }

  // ---------------------------------------------------------------------------
  if (ctx.u30_count > 0)
  {
    flag |= HAS_U30_MASK;
    uint32_t realcount = 0;

    BYTEARRAY d;
    bytearray_alloc(&d, 0);

    lua_rawgeti(L, ctx.index, CTX_INDEX_U30S);
    int i = 1;
    for (; i <= ctx.u30_count; i++)
    {
      lua_rawgeti(L, -1, i);

      lua_pushvalue(L, -1);
      lua_rawget(L, sym_map_idx);
      if (lua_isnil(L, -1))
      {
        lua_pop(L, 1);
        ffi_stream_add_u30(&d, lua_tointeger(L, -1));
        lua_pushinteger(L, index + (++realcount));
        lua_rawset(L, index_map_idx);
      }
      else
      {
        lua_pop(L, 2); // pop sym u30 idx & u30
      }
    }
    lua_pop(L, 1);

    ffi_stream_add_u30(&bodystream, realcount);
    bytearray_read_ready(&d);
    bytearray_writebuffer(&bodystream, d.buffer, d.total);

    bytearray_dealloc(&d);

    index += realcount;
  }

  // ---------------------------------------------------------------------------
  if (ctx.string_count > 0)
  {
    flag |= HAS_STRING_MASK;
    uint32_t realcount = 0;

    BYTEARRAY d;
    bytearray_alloc(&d, 0);

    lua_rawgeti(L, ctx.index, CTX_INDEX_STRINGS);
    int i = 1;
    for (; i <= ctx.string_count; i++)
    {
      lua_rawgeti(L, -1, i);

      lua_pushvalue(L, -1);
      lua_rawget(L, sym_map_idx);
      if (lua_isnil(L, -1))
      {
        lua_pop(L, 1);
        size_t len;
        const char *buf = lua_tolstring(L, -1, &len);
        ffi_stream_add_string(&d, buf, len);
        lua_pushinteger(L, index + (++realcount));
        lua_rawset(L, index_map_idx);
      }
      else
      {
        lua_pop(L, 2); // pop sym u30 idx & u30
      }
    }
    lua_pop(L, 1);

    ffi_stream_add_u30(&bodystream, realcount);
    bytearray_read_ready(&d);
    bytearray_writebuffer(&bodystream, d.buffer, d.total);

    bytearray_dealloc(&d);

    index += realcount;
  }
  // ---------------------------------------------------------------------------
  if (ctx.table_count)
  {
    flag |= HAS_TABLE_MASK;
    ffi_stream_add_u30(&bodystream, ctx.table_count);

    lua_rawgeti(L, ctx.index, CTX_INDEX_TABLES);
    int i = 1;
    for (; i <= ctx.table_count; i++)
    {
      lua_rawgeti(L, -1, i);
      lua_pushinteger(L, index + i);
      lua_rawset(L, index_map_idx);
    }

    for (i = 1; i <= ctx.table_count; i++)
    {
      lua_rawgeti(L, -1, i);
      int tb_idx = lua_gettop(L);

      BYTEARRAY d;
      bytearray_alloc(&d, 0);

      int tb_count = 0;
      while (true) {
        lua_rawgeti(L, tb_idx, tb_count + 1);
        if(lua_isnil(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        tb_count++;

        int value_idx = lua_gettop(L);
        lua_pushvalue(L, value_idx);
        lua_rawget(L, sym_map_idx);
        if (lua_isnil(L, -1))
        {
          lua_pop(L, 1);
          lua_pushvalue(L, value_idx);
          lua_rawget(L, index_map_idx);
        }
        ffi_stream_add_u30(&d, lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_pop(L, 1);
      }

      lua_pushnil(L);
      while (lua_next(L, tb_idx) != 0)
      {
        int value_idx = lua_gettop(L);
        int key_idx = lua_gettop(L) - 1;

        // ignore previously added array part.
        lua_Number value = lua_tonumber(L, -2);
        // suppose index no more than max int.
        if (value == (int)value && value <= tb_count && value > 0) {
          lua_pop(L, 1);
          continue;
        }

        // d:AddU30(sym_map[k] or index_map[k])
        lua_pushvalue(L, key_idx);
        lua_rawget(L, sym_map_idx);
        if (lua_isnil(L, -1))
        {
          lua_pop(L, 1);
          lua_pushvalue(L, key_idx);
          lua_rawget(L, index_map_idx);
        }
        ffi_stream_add_u30(&d, lua_tointeger(L, -1));
        lua_pop(L, 1);

        // d:AddU30(sym_map[v] or index_map[v])
        lua_pushvalue(L, value_idx);
        lua_rawget(L, sym_map_idx);
        if (lua_isnil(L, -1))
        {
          lua_pop(L, 1);
          lua_pushvalue(L, value_idx);
          lua_rawget(L, index_map_idx);
        }
        ffi_stream_add_u30(&d, lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_pop(L, 1);
      }

      lua_pop(L, 1);

      bytearray_read_ready(&d);

      BYTEARRAY d2;
      bytearray_alloc(&d2, 0);
      ffi_stream_add_u30(&d2, tb_count);
      bytearray_read_ready(&d2);

      ffi_stream_add_u30(&bodystream, d.total + d2.total);
      ffi_stream_add_bytes(&bodystream, (const char *)d2.buffer, d2.total);
      ffi_stream_add_bytes(&bodystream, (const char *)d.buffer, d.total);

      bytearray_dealloc(&d2);
      bytearray_dealloc(&d);
    }
    lua_pop(L, 1);
  }

  bytearray_read_ready(&bodystream);
  *((uint8_t *)bodystream.buffer) = flag;
  lua_pushlstring(L, (const char *)bodystream.buffer, bodystream.total);

  bytearray_dealloc(&bodystream);
  return 1;
}

LUA_API int luafan_objectbuf_decode(lua_State *L)
{
  size_t len;
  const char *buf = luaL_checklstring(L, 1, &len);
  BYTEARRAY input;
  bytearray_wrap_buffer(&input, (uint8_t *)buf, len); // will not change buf.

  int sym_idx = 0;
  if (lua_istable(L, 2))
  {
    sym_idx = 2;
  }

  uint8_t flag = 0;
  bytearray_read8(&input, &flag);

  switch (flag)
  {
  case 0:
    lua_pushboolean(L, false);
    return 1;
  case 1:
    lua_pushboolean(L, true);
    return 1;
  default:
    break;
  }

  lua_newtable(L);
  int index_map_idx = lua_gettop(L);

  uint32_t index = 2;

  if (!sym_idx)
  {
    lua_newtable(L);
  }
  else
  {
    lua_rawgeti(L, sym_idx, SYM_INDEX_INDEX);
    index = lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_rawgeti(L, sym_idx, SYM_INDEX_MAP_VK);
  }
  int sym_map_vk_idx = lua_gettop(L);

  int last_top = index + 1;

  if (!sym_idx)
  {
    lua_pushboolean(L, false);
    lua_rawseti(L, index_map_idx, FALSE_INDEX);
    lua_pushboolean(L, true);
    lua_rawseti(L, index_map_idx, TRUE_INDEX);
  }

  if (flag & HAS_NUMBER_MASK)
  {
    last_top = index + 1;
    uint32_t count = 0;
    if (!ffi_stream_get_u30(&input, &count))
    {
      lua_pushnil(L);
      lua_pushliteral(L, "decode failed, can't get `number` count.");
      return 2;
    }
    uint32_t i = 1;
    for (; i <= count; i++)
    {
      double result = 0;
      if (!ffi_stream_get_d64(&input, &result))
      {
        lua_pushnil(L);
        lua_pushfstring(L, "decode failed, can't decode `number`, %d/%d", i, count);
        return 2;
      }
      lua_pushnumber(L, result);
      lua_rawseti(L, index_map_idx, ++index);
    }
  }

  if (flag & HAS_U30_MASK)
  {
    last_top = index + 1;
    uint32_t count = 0;
    if (!ffi_stream_get_u30(&input, &count))
    {
      lua_pushnil(L);
      lua_pushliteral(L, "decode failed.");
      return 2;
    }
    uint32_t i = 1;
    for (; i <= count; i++)
    {
      uint32_t count = 0;
      if (!ffi_stream_get_u30(&input, &count))
      {
        lua_pushnil(L);
        lua_pushliteral(L, "decode failed.");
        return 2;
      }
      lua_pushinteger(L, count);
      lua_rawseti(L, index_map_idx, ++index);
    }
  }

  if (flag & HAS_STRING_MASK)
  {
    last_top = index + 1;
    uint32_t count = 0;
    if (!ffi_stream_get_u30(&input, &count))
    {
      lua_pushnil(L);
      lua_pushliteral(L, "decode failed.");
      return 2;
    }
    uint32_t i = 1;
    for (; i <= count; i++)
    {
      uint8_t *buff = NULL;
      size_t buflen = 0;
      ffi_stream_get_string(&input, &buff, &buflen);
      if (!buff)
      {
        lua_pushnil(L);
        lua_pushliteral(L, "decode failed.");
        return 2;
      }
      lua_pushlstring(L, (const char *)buff, buflen);
      lua_rawseti(L, index_map_idx, ++index);
    }
  }

  if (flag & HAS_TABLE_MASK)
  {
    last_top = index + 1;
    uint32_t count = 0;
    if (!ffi_stream_get_u30(&input, &count))
    {
      lua_pushnil(L);
      lua_pushliteral(L, "decode failed.");
      return 2;
    }
    uint32_t i = 1;
    for (; i <= count; i++)
    {
      lua_newtable(L);
      lua_rawseti(L, index_map_idx, index + i);
    }

    i = 1;
    for (; i <= count; i++)
    {
      uint8_t *buff = NULL;
      size_t buflen = 0;
      ffi_stream_get_string(&input, &buff, &buflen);
      if (!buff)
      {
        lua_pushnil(L);
        lua_pushliteral(L, "decode failed.");
        return 2;
      }
      BYTEARRAY d = {0};
      bytearray_wrap_buffer(&d, buff, buflen);

      lua_rawgeti(L, index_map_idx, index + i);

      uint32_t count = 0;
      if (!ffi_stream_get_u30(&d, &count))
      {
        lua_pushnil(L);
        lua_pushliteral(L, "'count' decode failed.");
        return 2;
      }

      int j = 1;
      for(; j <= count; j++) {
        uint32_t vi = 0;
        if (!ffi_stream_get_u30(&d, &vi))
        {
          lua_pushnil(L);
          lua_pushliteral(L, "'i value' decode failed.");
          return 2;
        }

        lua_rawgeti(L, sym_map_vk_idx, vi);
        if (lua_isnil(L, -1))
        {
          lua_pop(L, 1);
          lua_rawgeti(L, index_map_idx, vi);

          if (lua_isnil(L, -1))
          {
            luaL_error(L, "vi=%d not found.", vi);
          }
        }

        lua_rawseti(L, -2, j);
      }

      while (bytearray_read_available(&d) > 0)
      {
        uint32_t ki = 0;
        if (!ffi_stream_get_u30(&d, &ki))
        {
          lua_pushnil(L);
          lua_pushliteral(L, "decode failed.");
          return 2;
        }
        uint32_t vi = 0;
        if (!ffi_stream_get_u30(&d, &vi))
        {
          lua_pushnil(L);
          lua_pushliteral(L, "decode failed.");
          return 2;
        }

        lua_rawgeti(L, sym_map_vk_idx, ki);
        if (lua_isnil(L, -1))
        {
          lua_pop(L, 1);
          lua_rawgeti(L, index_map_idx, ki);

          if (lua_isnil(L, -1))
          {
            luaL_error(L, "ki=%d not found.", ki);
          }
        }

        lua_rawgeti(L, sym_map_vk_idx, vi);
        if (lua_isnil(L, -1))
        {
          lua_pop(L, 1);
          lua_rawgeti(L, index_map_idx, vi);

          if (lua_isnil(L, -1))
          {
            luaL_error(L, "vi=%d not found.", vi);
          }
        }

        lua_rawset(L, -3);
      }
      lua_pop(L, 1);
    }
  }

  lua_rawgeti(L, sym_map_vk_idx, last_top);
  if (lua_isnil(L, -1))
  {
    lua_rawgeti(L, index_map_idx, last_top);
  }
  // return sym_map_vk[last_top] or index_map[last_top]
  // lua_pushvalue(L, index_map_idx);
  // lua_pushinteger(L, last_top);
  return 1;
}

LUA_API int luafan_objectbuf_symbol(lua_State *L)
{
  if (lua_gettop(L) == 0)
  {
    luaL_error(L, "no argument.");
  }

  CTX ctx = {0};
  ctx_init(&ctx, L);

  packer(L, &ctx, 1);

  lua_settop(L, ctx.index);
  return 1;
}

LUA_API int luaopen_fan_objectbuf_core(lua_State *L)
{
  struct luaL_Reg objectbuflib[] = {
      {"encode", luafan_objectbuf_encode},
      {"decode", luafan_objectbuf_decode},
      {"symbol", luafan_objectbuf_symbol},
      {NULL, NULL},
  };

  lua_newtable(L);
  luaL_register(L, NULL, objectbuflib);
  return 1;
}
