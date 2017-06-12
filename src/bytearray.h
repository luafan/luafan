
#ifndef bytearray_h
#define bytearray_h

#include <inttypes.h>
#include <stdio.h>

#if !defined(__cplusplus)
#include <stdbool.h>
#endif

typedef struct
{
  size_t offset;
  size_t total;
  size_t mark;
  uint8_t *buffer;
  size_t buflen;
  bool reading;
  bool wrapbuffer;
} BYTEARRAY;

bool bytearray_alloc(BYTEARRAY *ba, uint32_t length);
bool bytearray_dealloc(BYTEARRAY *ba);
bool bytearray_wrap_buffer(BYTEARRAY *ba, uint8_t *buff, uint32_t length);

bool bytearray_read_ready(BYTEARRAY *ba);
bool bytearray_write_ready(BYTEARRAY *ba);
bool bytearray_empty(BYTEARRAY *ba);
size_t bytearray_read_available(BYTEARRAY *ba);

bool bytearray_mark(BYTEARRAY *ba);
bool bytearray_reset(BYTEARRAY *ba);

bool bytearray_write8(BYTEARRAY *ba, const uint8_t value);
bool bytearray_write16(BYTEARRAY *ba, const uint16_t value);
bool bytearray_write32(BYTEARRAY *ba, const uint32_t value);
bool bytearray_write64(BYTEARRAY *ba, const uint64_t value);
bool bytearray_write64d(BYTEARRAY *ba, const double value);
bool bytearray_writebuffer(BYTEARRAY *ba, const void *buff,
                           const size_t length);

bool bytearray_writestring8(BYTEARRAY *ba, const uint8_t *str,
                            const uint8_t length);
bool bytearray_writestring16(BYTEARRAY *ba, const uint8_t *str,
                             const uint16_t length);
bool bytearray_writestring32(BYTEARRAY *ba, const uint8_t *str,
                             const uint32_t length);

bool bytearray_read8(BYTEARRAY *ba, uint8_t *value);
bool bytearray_read16(BYTEARRAY *ba, uint16_t *value);
bool bytearray_read32(BYTEARRAY *ba, uint32_t *value);
bool bytearray_read64(BYTEARRAY *ba, uint64_t *value);
bool bytearray_read64d(BYTEARRAY *ba, double *value);
bool bytearray_readbuffer(BYTEARRAY *ba, void *buff, uint32_t length);

bool bytearray_readstring8(BYTEARRAY *ba, uint8_t *str, uint8_t length,
                           uint8_t *count);
bool bytearray_readstring16(BYTEARRAY *ba, uint8_t *str, uint16_t length,
                            uint16_t *count);
bool bytearray_readstring32(BYTEARRAY *ba, uint8_t *str, uint32_t length,
                            uint32_t *count);

#endif
