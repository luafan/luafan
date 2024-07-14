
#include "bytearray.h"

#include <memory.h>
#include <stdlib.h>

inline bool ensure_capacity(BYTEARRAY *ba, size_t required_size) {
    while (ba->total - ba->offset < required_size) {
        if (!ba->wrapbuffer) {
            ba->buflen *= 2;
            ba->total = ba->buflen;
            void *temp = realloc(ba->buffer, ba->buflen);
            if (!temp) {
                return false;
            }
            ba->buffer = temp;
        } else {
            return false;
        }
    }
    return true;
}

inline bool write_value(BYTEARRAY *ba, const void *value, size_t size) {
    if (!ensure_capacity(ba, size)) {
        return false;
    }
    memcpy(ba->buffer + ba->offset, value, size);
    ba->offset += size;
    return true;
}

inline bool read_value(BYTEARRAY *ba, void *value, size_t size) {
    if (ba->total - ba->offset < size) {
        return false;
    }
    memcpy(value, ba->buffer + ba->offset, size);
    ba->offset += size;
    return true;
}

bool bytearray_alloc(BYTEARRAY *ba, uint32_t length) {
    if (length == 0) {
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

bool bytearray_dealloc(BYTEARRAY *ba) {
    if (!ba->wrapbuffer) {
        free(ba->buffer);
        ba->buffer = NULL;
    }

    ba->offset = 0;
    ba->total = 0;
    return true;
}

bool bytearray_wrap_buffer(BYTEARRAY *ba, uint8_t *buff, uint32_t length) {
    ba->buffer = buff;
    ba->total = length;
    ba->offset = 0;
    ba->buflen = length;
    ba->reading = true;
    ba->wrapbuffer = true;

    return true;
}

bool bytearray_read_ready(BYTEARRAY *ba) {
    if (ba == NULL || ba->buffer == NULL || ba->reading) {
        return false;
    }
    ba->total = ba->offset;
    ba->offset = 0;
    ba->reading = true;
    return true;
}

size_t bytearray_read_available(BYTEARRAY *ba) {
    if (ba == NULL || ba->buffer == NULL || !ba->reading) {
        return 0;
    }
    return ba->total - ba->offset;
}

bool bytearray_mark(BYTEARRAY *ba) {
    if (ba == NULL || ba->buffer == NULL || !ba->reading) {
        return 0;
    }

    ba->mark = ba->offset;
    return 1;
}

bool bytearray_reset(BYTEARRAY *ba) {
    if (ba == NULL || ba->buffer == NULL || !ba->reading) {
        return 0;
    }

    ba->offset = ba->mark;
    return 1;
}

bool bytearray_empty(BYTEARRAY *ba) {
    if (ba != NULL) {
        ba->offset = 0;
        ba->total = 0;
        return true;
    }
    return false;
}

bool bytearray_write_ready(BYTEARRAY *ba) {
    if (ba == NULL || !ba->reading || ba->buffer == NULL) {
        return false;
    }
    if (ba->offset > 0) {
        size_t unreadleft = ba->total - ba->offset;
        uint8_t *buf = ba->buffer;

        memmove(buf, buf + ba->offset, unreadleft);
        ba->offset = unreadleft;
    } else {
        ba->offset = ba->total;
    }

    ba->mark = 0;
    ba->total = ba->buflen;
    ba->reading = false;

    return true;
}

bool bytearray_writebuffer(BYTEARRAY *ba, const void *buff, const size_t length) {
    if (!ensure_capacity(ba, length)) {
        return false;
    }
    memcpy(ba->buffer + ba->offset, buff, length);
    ba->offset += length;
    return true;
}

bool bytearray_readbuffer(BYTEARRAY *ba, void *buff, uint32_t length) {
    if (ba->total - ba->offset < length) {
        return false;
    }
    if (buff) {
        memcpy(buff, ba->buffer + ba->offset, length);
    }
    ba->offset += length;

    return true;
}

bool bytearray_write8(BYTEARRAY *ba, const uint8_t value) {
    return write_value(ba, &value, sizeof(uint8_t));
}

bool bytearray_write16(BYTEARRAY *ba, const uint16_t value) {
    return write_value(ba, &value, sizeof(uint16_t));
}

bool bytearray_write32(BYTEARRAY *ba, const uint32_t value) {
    return write_value(ba, &value, sizeof(uint32_t));
}

bool bytearray_write64(BYTEARRAY *ba, const uint64_t value) {
    return write_value(ba, &value, sizeof(uint64_t));
}

bool bytearray_write64d(BYTEARRAY *ba, const double value) {
    return write_value(ba, &value, sizeof(double));
}

bool bytearray_read8(BYTEARRAY *ba, uint8_t *value) {
    return read_value(ba, value, sizeof(uint8_t));
}

bool bytearray_read16(BYTEARRAY *ba, uint16_t *value) {
    return read_value(ba, value, sizeof(uint16_t));
}

bool bytearray_read32(BYTEARRAY *ba, uint32_t *value) {
    return read_value(ba, value, sizeof(uint32_t));
}

bool bytearray_read64(BYTEARRAY *ba, uint64_t *value) {
    return read_value(ba, value, sizeof(uint64_t));
}

bool bytearray_read64d(BYTEARRAY *ba, double *value) {
    return read_value(ba, value, sizeof(double));
}
