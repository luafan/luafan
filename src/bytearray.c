#include "bytearray.h"
#include <memory.h>
#include <stdlib.h>
#include <string.h>

// Aggressive optimization settings for objectbuf workloads
#define MIN_CAPACITY 128        // Larger initial capacity to reduce reallocations
#define GROWTH_FACTOR 1.6f      // Optimal growth factor based on research
#define MAX_PREALLOC 4096       // Larger pre-allocation for objectbuf
#define CACHE_LINE_SIZE 64      // For memory alignment optimization

// Memory pool for small allocations to reduce malloc overhead
#define POOL_SIZE 512
#define POOL_BLOCK_SIZE 2048
static struct {
    uint8_t blocks[POOL_SIZE][POOL_BLOCK_SIZE];
    bool used[POOL_SIZE];
    int next_free;
} memory_pool = {.next_free = 0};

// Fast memory pool allocation for small buffers
static inline void* pool_alloc(size_t size) {
    if (size <= POOL_BLOCK_SIZE) {
        for (int i = memory_pool.next_free; i < POOL_SIZE; i++) {
            if (!memory_pool.used[i]) {
                memory_pool.used[i] = true;
                memory_pool.next_free = (i + 1) % POOL_SIZE;
                return memory_pool.blocks[i];
            }
        }
        // Wrap around if needed
        for (int i = 0; i < memory_pool.next_free; i++) {
            if (!memory_pool.used[i]) {
                memory_pool.used[i] = true;
                memory_pool.next_free = (i + 1) % POOL_SIZE;
                return memory_pool.blocks[i];
            }
        }
    }
    return malloc(size);
}

static inline void pool_free(void* ptr) {
    // Check if pointer is in our pool
    if (ptr >= (void*)memory_pool.blocks && ptr < (void*)(memory_pool.blocks + POOL_SIZE)) {
        int index = ((uint8_t*)ptr - (uint8_t*)memory_pool.blocks) / POOL_BLOCK_SIZE;
        if (index >= 0 && index < POOL_SIZE) {
            memory_pool.used[index] = false;
            if (index < memory_pool.next_free) {
                memory_pool.next_free = index;
            }
            return;
        }
    }
    free(ptr);
}

// Branch prediction hints for better performance
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// Optimized capacity management with power-of-2 sizes for better memory allocator performance
static inline size_t next_power_of_2(size_t n) {
    if (n <= MIN_CAPACITY) return MIN_CAPACITY;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

// Fast path macros for common operations (inlined)
#define FAST_BOUNDS_CHECK(ba, size) \
    __builtin_expect((ba->total - ba->offset >= size), 1)

#define FAST_WRITE_BOUNDS_CHECK(ba, size) \
    __builtin_expect((ba->total - ba->offset >= size) || (!ba->wrapbuffer), 1)

// Ultra-fast capacity expansion with optimized realloc strategy
static inline bool ensure_capacity_optimized(BYTEARRAY *ba, size_t required_size) {
    size_t available = ba->total - ba->offset;
    if (LIKELY(available >= required_size)) {
        return true; // Fast path - no allocation needed
    }

    if (UNLIKELY(ba->wrapbuffer)) {
        return false; // Cannot expand wrapped buffer
    }

    size_t needed = ba->offset + required_size;
    size_t new_size;

    // Optimized size calculation - use power-of-2 for small sizes
    if (needed <= MAX_PREALLOC) {
        new_size = next_power_of_2(needed);
    } else {
        new_size = needed + (needed >> 2); // Add 25% for future growth
        // Align to cache line boundary for better performance
        new_size = (new_size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    }

    // For simplicity, disable pool allocation in capacity expansion
    // This avoids mixing pool and malloc pointers which causes realloc issues
    uint8_t *new_buffer = realloc(ba->buffer, new_size);

    if (UNLIKELY(!new_buffer)) {
        return false;
    }

    ba->buffer = new_buffer;
    ba->buflen = new_size;
    ba->total = new_size;
    return true;
}

// Ultra-optimized write operations with minimal overhead
static inline bool write_value_optimized(BYTEARRAY *ba, const void *value, size_t size) {
    // Ultra-fast path for most common case
    if (LIKELY(ba->total - ba->offset >= size)) {
        // Use compiler builtin for better optimization
        __builtin_memcpy(ba->buffer + ba->offset, value, size);
        ba->offset += size;
        return true;
    }

    // Slow path - need capacity expansion
    if (UNLIKELY(!ensure_capacity_optimized(ba, size))) {
        return false;
    }
    __builtin_memcpy(ba->buffer + ba->offset, value, size);
    ba->offset += size;
    return true;
}

// Ultra-optimized read operations
static inline bool read_value_optimized(BYTEARRAY *ba, void *value, size_t size) {
    if (LIKELY(ba->total - ba->offset >= size)) {
        __builtin_memcpy(value, ba->buffer + ba->offset, size);
        ba->offset += size;
        return true;
    }
    return false;
}

// Optimized individual write functions
bool bytearray_write8_optimized(BYTEARRAY *ba, const uint8_t value) {
    // Ultra-fast path for single byte
    if (__builtin_expect(ba->offset < ba->total, 1)) {
        ba->buffer[ba->offset++] = value;
        return true;
    }
    return write_value_optimized(ba, &value, 1);
}

bool bytearray_write16_optimized(BYTEARRAY *ba, const uint16_t value) {
    return write_value_optimized(ba, &value, sizeof(uint16_t));
}

bool bytearray_write32_optimized(BYTEARRAY *ba, const uint32_t value) {
    return write_value_optimized(ba, &value, sizeof(uint32_t));
}

bool bytearray_write64_optimized(BYTEARRAY *ba, const uint64_t value) {
    return write_value_optimized(ba, &value, sizeof(uint64_t));
}

bool bytearray_write64d_optimized(BYTEARRAY *ba, const double value) {
    return write_value_optimized(ba, &value, sizeof(double));
}

// Optimized read functions
bool bytearray_read8_optimized(BYTEARRAY *ba, uint8_t *value) {
    if (__builtin_expect(ba->offset < ba->total, 1)) {
        *value = ba->buffer[ba->offset++];
        return true;
    }
    return false;
}

bool bytearray_read16_optimized(BYTEARRAY *ba, uint16_t *value) {
    return read_value_optimized(ba, value, sizeof(uint16_t));
}

bool bytearray_read32_optimized(BYTEARRAY *ba, uint32_t *value) {
    return read_value_optimized(ba, value, sizeof(uint32_t));
}

bool bytearray_read64_optimized(BYTEARRAY *ba, uint64_t *value) {
    return read_value_optimized(ba, value, sizeof(uint64_t));
}

bool bytearray_read64d_optimized(BYTEARRAY *ba, double *value) {
    return read_value_optimized(ba, value, sizeof(double));
}

// Optimized buffer operations
bool bytearray_writebuffer_optimized(BYTEARRAY *ba, const void *buff, const size_t length) {
    return write_value_optimized(ba, buff, length);
}

bool bytearray_readbuffer_optimized(BYTEARRAY *ba, void *buff, uint32_t length) {
    if (__builtin_expect(FAST_BOUNDS_CHECK(ba, length), 1)) {
        if (buff) {
            memcpy(buff, ba->buffer + ba->offset, length);
        }
        ba->offset += length;
        return true;
    }
    return false;
}

// Re-implement original functions using optimized versions
bool ensure_capacity(BYTEARRAY *ba, size_t required_size) {
    return ensure_capacity_optimized(ba, required_size);
}

bool write_value(BYTEARRAY *ba, const void *value, size_t size) {
    return write_value_optimized(ba, value, size);
}

bool read_value(BYTEARRAY *ba, void *value, size_t size) {
    return read_value_optimized(ba, value, size);
}

bool bytearray_alloc(BYTEARRAY *ba, uint32_t length) {
    if (length == 0) {
        length = MIN_CAPACITY;
    }

    // Use regular malloc to avoid mixing with realloc
    ba->buffer = malloc(length);

    if (UNLIKELY(!ba->buffer)) {
        return false;
    }

    ba->offset = 0;
    ba->total = length;
    ba->buflen = length;
    ba->wrapbuffer = false;
    ba->reading = false;
    ba->mark = 0;

    return true;
}

bool bytearray_dealloc(BYTEARRAY *ba) {
    if (!ba->wrapbuffer && ba->buffer) {
        free(ba->buffer);
        ba->buffer = NULL;
    }

    ba->offset = 0;
    ba->total = 0;
    ba->buflen = 0;
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
    return bytearray_writebuffer_optimized(ba, buff, length);
}

bool bytearray_readbuffer(BYTEARRAY *ba, void *buff, uint32_t length) {
    return bytearray_readbuffer_optimized(ba, buff, length);
}

bool bytearray_write8(BYTEARRAY *ba, const uint8_t value) {
    return bytearray_write8_optimized(ba, value);
}

bool bytearray_write16(BYTEARRAY *ba, const uint16_t value) {
    return bytearray_write16_optimized(ba, value);
}

bool bytearray_write32(BYTEARRAY *ba, const uint32_t value) {
    return bytearray_write32_optimized(ba, value);
}

bool bytearray_write64(BYTEARRAY *ba, const uint64_t value) {
    return bytearray_write64_optimized(ba, value);
}

bool bytearray_write64d(BYTEARRAY *ba, const double value) {
    return bytearray_write64d_optimized(ba, value);
}

bool bytearray_read8(BYTEARRAY *ba, uint8_t *value) {
    return bytearray_read8_optimized(ba, value);
}

bool bytearray_read16(BYTEARRAY *ba, uint16_t *value) {
    return bytearray_read16_optimized(ba, value);
}

bool bytearray_read32(BYTEARRAY *ba, uint32_t *value) {
    return bytearray_read32_optimized(ba, value);
}

bool bytearray_read64(BYTEARRAY *ba, uint64_t *value) {
    return bytearray_read64_optimized(ba, value);
}

bool bytearray_read64d(BYTEARRAY *ba, double *value) {
    return bytearray_read64d_optimized(ba, value);
}