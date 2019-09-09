
#include "bytearray.h"
#include <memory.h>
#include <stdlib.h>

#define WRITE_VALUE(type)                                \
  while (ba->total - ba->offset < sizeof(type))          \
  {                                                      \
    if (!ba->wrapbuffer)                                 \
    {                                                    \
      ba->buflen += ba->buflen;                          \
      ba->total = ba->buflen;                            \
      ba->buffer = realloc(ba->buffer, ba->buflen);      \
    }                                                    \
    else                                                 \
    {                                                    \
      return false;                                      \
    }                                                    \
  }                                                      \
  memcpy(ba->buffer + ba->offset, &value, sizeof(type)); \
  ba->offset += sizeof(type);                            \
  return true;

#define WRITE_STRING(type)                                     \
  while (ba->total - ba->offset < length + sizeof(type))       \
  {                                                            \
    if (!ba->wrapbuffer)                                       \
    {                                                          \
      ba->buflen += ba->buflen;                                \
      ba->total = ba->buflen;                                  \
      ba->buffer = realloc(ba->buffer, ba->buflen);            \
    }                                                          \
    else                                                       \
    {                                                          \
      return false;                                            \
    }                                                          \
  }                                                            \
  memcpy(ba->buffer + ba->offset, &length, sizeof(type));      \
  memcpy(ba->buffer + ba->offset + sizeof(type), str, length); \
  ba->offset += sizeof(type) + length;                         \
  return true;

#define READ_VALUE(type)                                \
  if (ba->total - ba->offset < sizeof(type))            \
  {                                                     \
    return false;                                       \
  }                                                     \
  memcpy(value, ba->buffer + ba->offset, sizeof(type)); \
  ba->offset += sizeof(type);                           \
  return true;

#define READ_STRING(type)                                       \
  if (ba->total - ba->offset < sizeof(type))                    \
  {                                                             \
    return false;                                               \
  }                                                             \
  memcpy(count, ba->buffer + ba->offset, sizeof(type));         \
  if (ba->total - ba->offset < sizeof(type) + *count)           \
  {                                                             \
    *count = 0;                                                 \
    return false;                                               \
  }                                                             \
  type readLen = length < *count ? length : *count;             \
  memcpy(str, ba->buffer + ba->offset + sizeof(type), readLen); \
  ba->offset += sizeof(type) + *count;                          \
  *count = readLen;                                             \
  return true;

bool bytearray_alloc(BYTEARRAY *ba, uint32_t length)
{
  if (length == 0)
  {
    length = 64;
  }
  ba->buffer = realloc(NULL, length);
  ba->offset = 0;
  ba->total = length;
  ba->buflen = length;
  ba->wrapbuffer = false;
  ba->reading = false;

  return true;
}

bool bytearray_dealloc(BYTEARRAY *ba)
{
  if (!ba->wrapbuffer)
  {
    free(ba->buffer);
    ba->buffer = NULL;
  }

  ba->offset = 0;
  ba->total = 0;
  return true;
}

bool bytearray_wrap_buffer(BYTEARRAY *ba, uint8_t *buff, uint32_t length)
{
  ba->buffer = buff;
  ba->total = length;
  ba->offset = 0;
  ba->buflen = length;
  ba->reading = true;
  ba->wrapbuffer = true;

  return true;
}

bool bytearray_read_ready(BYTEARRAY *ba)
{
  if (ba == NULL || ba->buffer == NULL || ba->reading)
  {
    return false;
  }
  ba->total = ba->offset;
  ba->offset = 0;
  ba->reading = true;
  return true;
}

size_t bytearray_read_available(BYTEARRAY *ba)
{
  if (ba == NULL || ba->buffer == NULL || !ba->reading)
  {
    return 0;
  }
  return ba->total - ba->offset;
}

bool bytearray_mark(BYTEARRAY *ba)
{
  if (ba == NULL || ba->buffer == NULL || !ba->reading)
  {
    return 0;
  }

  ba->mark = ba->offset;
  return 1;
}

bool bytearray_reset(BYTEARRAY *ba)
{
  if (ba == NULL || ba->buffer == NULL || !ba->reading)
  {
    return 0;
  }

  ba->offset = ba->mark;
  return 1;
}

bool bytearray_empty(BYTEARRAY *ba)
{
  if (ba != NULL)
  {
    ba->offset = 0;
    ba->total = 0;
    return true;
  }
  return false;
}

bool bytearray_write_ready(BYTEARRAY *ba)
{
  if (ba == NULL || !ba->reading || ba->buffer == NULL)
  {
    return false;
  }
  if (ba->offset > 0)
  {
    size_t unreadleft = ba->total - ba->offset;
    uint8_t *buf = ba->buffer;

    memmove(buf, buf + ba->offset, unreadleft);
    ba->offset = unreadleft;
  }
  else
  {
    ba->offset = ba->total;
  }

  ba->mark = 0;
  ba->total = ba->buflen;
  ba->reading = false;

  return true;
}
#define type uint8_t
bool bytearray_write8(BYTEARRAY *ba, const uint8_t value)
{
  WRITE_VALUE(uint8_t)
}

bool bytearray_write16(BYTEARRAY *ba, const uint16_t value)
{
  WRITE_VALUE(uint16_t)
}

bool bytearray_write32(BYTEARRAY *ba, const uint32_t value)
{
  WRITE_VALUE(uint32_t)
}

bool bytearray_write64(BYTEARRAY *ba, const uint64_t value)
{
  WRITE_VALUE(uint64_t)
}

bool bytearray_write64d(BYTEARRAY *ba, const double value)
{
  WRITE_VALUE(double)
}

bool bytearray_writebuffer(BYTEARRAY *ba, const void *buff,
                           const size_t length)
{
  if (ba->total - ba->offset < length)
  {
    ba->total = length + ba->offset;
    ba->buflen = ba->total;
    ba->buffer = realloc(ba->buffer, ba->buflen);
  }
  memcpy(ba->buffer + ba->offset, buff, length);
  ba->offset += length;
  return true;
}

bool bytearray_writestring8(BYTEARRAY *ba, const uint8_t *str,
                            const uint8_t length)
{
  WRITE_STRING(uint8_t)
}

bool bytearray_writestring16(BYTEARRAY *ba, const uint8_t *str,
                             const uint16_t length)
{
  WRITE_STRING(uint16_t)
}

bool bytearray_writestring32(BYTEARRAY *ba, const uint8_t *str,
                             const uint32_t length)
{
  WRITE_STRING(uint32_t)
}

bool bytearray_read8(BYTEARRAY *ba, uint8_t *value) { READ_VALUE(uint8_t) }

bool bytearray_read16(BYTEARRAY *ba, uint16_t *value) { READ_VALUE(uint16_t) }

bool bytearray_read32(BYTEARRAY *ba, uint32_t *value) { READ_VALUE(uint32_t) }

bool bytearray_read64(BYTEARRAY *ba, uint64_t *value) { READ_VALUE(uint64_t) }

bool bytearray_read64d(BYTEARRAY *ba, double *value) { READ_VALUE(double) }

bool bytearray_readbuffer(BYTEARRAY *ba, void *buff, uint32_t length)
{
  if (ba->total - ba->offset < length)
  {
    return false;
  }
  if (buff)
  {
    memcpy(buff, ba->buffer + ba->offset, length);
  }
  ba->offset += length;

  return true;
}

bool bytearray_readstring8(BYTEARRAY *ba, uint8_t *str, uint8_t length,
                           uint8_t *count)
{
  READ_STRING(uint8_t)
}

bool bytearray_readstring16(BYTEARRAY *ba, uint8_t *str, uint16_t length,
                            uint16_t *count)
{
  READ_STRING(uint16_t)
}

bool bytearray_readstring32(BYTEARRAY *ba, uint8_t *str, uint32_t length,
                            uint32_t *count)
{
  READ_STRING(uint32_t)
}
