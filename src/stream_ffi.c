#include "bytearray.h"

void ffi_stream_new(BYTEARRAY *ba, const char *data, size_t len)
{
  if (data && len > 0)
  {
    bytearray_alloc(ba, len);
    bytearray_writebuffer(ba, data, len);
    bytearray_read_ready(ba);
  }
  else
  {
    bytearray_alloc(ba, 0);
  }
}

void ffi_stream_gc(BYTEARRAY *ba)
{
  bytearray_dealloc(ba);
}

size_t ffi_stream_available(BYTEARRAY *ba)
{
  return bytearray_read_available(ba);
}

// ========== GET ==========
bool ffi_stream_get_u8(BYTEARRAY *ba, uint8_t *result)
{
  return bytearray_read8(ba, result);
}

bool ffi_stream_get_u16(BYTEARRAY *ba, uint16_t *result)
{
  return bytearray_read16(ba, result);
}

bool ffi_stream_get_u32(BYTEARRAY *ba, uint32_t *result)
{
  return bytearray_read32(ba, result);
}

bool ffi_stream_get_u30(BYTEARRAY *ba, uint32_t *result)
{
  uint8_t b;
  uint32_t value = 0;
  uint8_t shift = 0;

  while (true)
  {
    if (!bytearray_read8(ba, &b))
    {
      return false;
    }
    value |= ((b & 127) << shift);
    shift += 7;

    if ((b & 128) == 0 || shift > 30)
    {
      break;
    }
  }

  *result = value;
  return true;
}

bool ffi_stream_get_s24(BYTEARRAY *ba, int32_t *result)
{
  uint8_t value[3];
  if (!bytearray_readbuffer(ba, value, 3))
  {
    return false;
  }

  if (value[2] & 0x80)
  {
    *result = -1 - ((value[2] << 16 | value[1] << 8 | value[0]) ^ 0xffffff);
  }
  else
  {
    *result = value[2] << 16 | value[1] << 8 | value[0];
  }

  return true;
}

bool ffi_stream_get_u24(BYTEARRAY *ba, uint32_t *result)
{
  uint8_t value[3];
  if (!bytearray_readbuffer(ba, value, 3))
  {
    return false;
  }

  *result = value[2] << 16 | value[1] << 8 | value[0];
  return true;
}

bool ffi_stream_get_d64(BYTEARRAY *ba, double *result)
{
  return bytearray_read64d(ba, result);
}

bool ffi_stream_mark(BYTEARRAY *ba)
{
  return bytearray_mark(ba);
}

bool ffi_stream_reset(BYTEARRAY *ba)
{
  return bytearray_reset(ba);
}

void ffi_stream_get_string(BYTEARRAY *ba, uint8_t **buff, size_t *buflen)
{
  size_t offset = ba->offset;
  uint32_t len = 0;
  if (!ffi_stream_get_u30(ba, &len))
  {
    ba->offset = offset;

    *buff = NULL;
    *buflen = bytearray_read_available(ba) + 1;
    return;
  }
  size_t available = bytearray_read_available(ba);

  if (len > available)
  {
    // reset offset.
    size_t diff = ba->offset - offset;
    ba->offset = offset;

    *buff = NULL;
    *buflen = len + diff;
  }
  else
  {
    *buff = ba->buffer + ba->offset;
    *buflen = len;
    bytearray_readbuffer(ba, NULL, len);
  }
}

void ffi_stream_get_bytes(BYTEARRAY *ba, uint8_t **buff, size_t *buflen)
{
  size_t available = bytearray_read_available(ba);
  uint32_t len = *buflen > 0 ? (*buflen > available ? available : *buflen) : available;

  *buff = ba->buffer + ba->offset;
  *buflen = len;
  bytearray_readbuffer(ba, NULL, len);
}

// ========== ADD ==========
void ffi_stream_add_u8(BYTEARRAY *ba, uint8_t value)
{
  bytearray_write8(ba, value);
}

void ffi_stream_add_u16(BYTEARRAY *ba, uint16_t value)
{
  bytearray_write16(ba, value);
}

void ffi_stream_add_u30(BYTEARRAY *ba, uint32_t u)
{
  do
  {
    bytearray_write8(ba, ((u & ~0x7f) != 0 ? 0x80 : 0) | (u & 0x7F));
    u = u >> 7;
  } while (u != 0);
}

void ffi_stream_add_u24(BYTEARRAY *ba, uint32_t u)
{
  uint8_t value[3];
  value[2] = (u >> 16) & 0xff;
  value[1] = (u >> 8) & 0xff;
  value[0] = u & 0xff;
  bytearray_writebuffer(ba, value, 3);
}

void ffi_stream_add_d64(BYTEARRAY *ba, double value)
{
  bytearray_write64d(ba, value);
}

void ffi_stream_add_string(BYTEARRAY *ba, const char *data, size_t len)
{
  ffi_stream_add_u30(ba, len);
  bytearray_writebuffer(ba, data, len);
}

void ffi_stream_add_bytes(BYTEARRAY *ba, const char *data, size_t len)
{
  bytearray_writebuffer(ba, data, len);
}

// ========== Others ==========
void ffi_stream_package(BYTEARRAY *ba, uint8_t **buff, size_t *buflen)
{
  bytearray_read_ready(ba);
  *buff = ba->buffer;
  *buflen = ba->total;
}

bool ffi_stream_prepare_get(BYTEARRAY *ba)
{
  return bytearray_read_ready(ba);
}

bool ffi_stream_prepare_add(BYTEARRAY *ba)
{
  return bytearray_write_ready(ba);
}

bool ffi_stream_empty(BYTEARRAY *ba)
{
  return bytearray_empty(ba);
}
