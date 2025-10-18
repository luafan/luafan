#include "test_framework.h"
#include "bytearray.h"
#include <string.h>
#include <stdint.h>

/* Test bytearray allocation and deallocation */
TEST_CASE(test_bytearray_alloc_dealloc) {
    BYTEARRAY ba;

    // Test normal allocation
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));
    TEST_ASSERT_NOT_NULL(ba.buffer);
    TEST_ASSERT_EQUAL(100, ba.buflen);
    TEST_ASSERT_EQUAL(100, ba.total);
    TEST_ASSERT_EQUAL(0, ba.offset);
    TEST_ASSERT_FALSE(ba.reading);
    TEST_ASSERT_FALSE(ba.wrapbuffer);

    // Test deallocation
    TEST_ASSERT_TRUE(bytearray_dealloc(&ba));
    TEST_ASSERT_NULL(ba.buffer);
    TEST_ASSERT_EQUAL(0, ba.offset);
    TEST_ASSERT_EQUAL(0, ba.total);

    // Test zero-length allocation (should default to 128 in optimized version)
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 0));
    TEST_ASSERT_NOT_NULL(ba.buffer);
    TEST_ASSERT_EQUAL(128, ba.buflen);
    TEST_ASSERT_EQUAL(128, ba.total);
    bytearray_dealloc(&ba);
}

/* Test buffer wrapping functionality */
TEST_CASE(test_bytearray_wrap_buffer) {
    BYTEARRAY ba;
    uint8_t external_buffer[256];

    // Initialize external buffer with test pattern
    for (int i = 0; i < 256; i++) {
        external_buffer[i] = (uint8_t)(i & 0xFF);
    }

    // Test buffer wrapping
    TEST_ASSERT_TRUE(bytearray_wrap_buffer(&ba, external_buffer, 256));
    TEST_ASSERT_EQUAL(external_buffer, ba.buffer);
    TEST_ASSERT_EQUAL(256, ba.total);
    TEST_ASSERT_EQUAL(256, ba.buflen);
    TEST_ASSERT_EQUAL(0, ba.offset);
    TEST_ASSERT_TRUE(ba.reading);
    TEST_ASSERT_TRUE(ba.wrapbuffer);

    // Test reading from wrapped buffer
    uint8_t value;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(0, value);
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(1, value);

    // Deallocation should not free external buffer
    bytearray_dealloc(&ba);
    TEST_ASSERT_EQUAL(0, external_buffer[0]); // Buffer should still be intact
}

/* Test read/write state transitions */
TEST_CASE(test_bytearray_state_transitions) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Initial state should be write mode
    TEST_ASSERT_FALSE(ba.reading);
    // bytearray_write_ready should only be called from reading mode, not initial write mode

    // Write some data
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 0x42));
    TEST_ASSERT_TRUE(bytearray_write16(&ba, 0x1234));
    TEST_ASSERT_EQUAL(3, ba.offset); // 1 + 2 bytes written

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    TEST_ASSERT_TRUE(ba.reading);
    TEST_ASSERT_EQUAL(0, ba.offset); // Reset for reading
    TEST_ASSERT_EQUAL(3, ba.total);   // Total available to read

    // Read back the data
    uint8_t val8;
    uint16_t val16;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &val8));
    TEST_ASSERT_EQUAL(0x42, val8);
    TEST_ASSERT_TRUE(bytearray_read16(&ba, &val16));
    TEST_ASSERT_EQUAL(0x1234, val16);

    // Switch back to write mode
    TEST_ASSERT_TRUE(bytearray_write_ready(&ba));
    TEST_ASSERT_FALSE(ba.reading);

    bytearray_dealloc(&ba);
}

/* Test all numeric read/write operations */
TEST_CASE(test_bytearray_numeric_operations) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 1000));

    // Test data
    uint8_t test_u8 = 0xAB;
    uint16_t test_u16 = 0x1234;
    uint32_t test_u32 = 0x12345678;
    uint64_t test_u64 = 0x123456789ABCDEF0ULL;
    double test_double = 3.14159265359;

    // Write all values
    TEST_ASSERT_TRUE(bytearray_write8(&ba, test_u8));
    TEST_ASSERT_TRUE(bytearray_write16(&ba, test_u16));
    TEST_ASSERT_TRUE(bytearray_write32(&ba, test_u32));
    TEST_ASSERT_TRUE(bytearray_write64(&ba, test_u64));
    TEST_ASSERT_TRUE(bytearray_write64d(&ba, test_double));

    // Check total bytes written
    size_t expected_size = sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) +
                          sizeof(uint64_t) + sizeof(double);
    TEST_ASSERT_EQUAL(expected_size, ba.offset);

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Read and verify all values
    uint8_t read_u8;
    uint16_t read_u16;
    uint32_t read_u32;
    uint64_t read_u64;
    double read_double;

    TEST_ASSERT_TRUE(bytearray_read8(&ba, &read_u8));
    TEST_ASSERT_EQUAL(test_u8, read_u8);

    TEST_ASSERT_TRUE(bytearray_read16(&ba, &read_u16));
    TEST_ASSERT_EQUAL(test_u16, read_u16);

    TEST_ASSERT_TRUE(bytearray_read32(&ba, &read_u32));
    TEST_ASSERT_EQUAL(test_u32, read_u32);

    TEST_ASSERT_TRUE(bytearray_read64(&ba, &read_u64));
    TEST_ASSERT_EQUAL(test_u64, read_u64);

    TEST_ASSERT_TRUE(bytearray_read64d(&ba, &read_double));
    TEST_ASSERT(read_double > 3.14159 && read_double < 3.14160, "Double value mismatch");

    bytearray_dealloc(&ba);
}

/* Test buffer operations with larger data */
TEST_CASE(test_bytearray_buffer_operations) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 500));

    // Create test buffer
    uint8_t write_buffer[200];
    uint8_t read_buffer[200];

    // Fill write buffer with pattern
    for (int i = 0; i < 200; i++) {
        write_buffer[i] = (uint8_t)(i % 256);
    }

    // Write buffer data
    TEST_ASSERT_TRUE(bytearray_writebuffer(&ba, write_buffer, 200));
    TEST_ASSERT_EQUAL(200, ba.offset);

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Read buffer data
    memset(read_buffer, 0, 200);
    TEST_ASSERT_TRUE(bytearray_readbuffer(&ba, read_buffer, 200));

    // Verify data integrity
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_EQUAL(write_buffer[i], read_buffer[i]);
    }

    bytearray_dealloc(&ba);
}

/* Test mark and reset functionality */
TEST_CASE(test_bytearray_mark_reset) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Write test data
    for (uint8_t i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(bytearray_write8(&ba, i));
    }

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Read some data
    uint8_t value;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(0, value);
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(1, value);
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(2, value);

    // Mark current position
    TEST_ASSERT_TRUE(bytearray_mark(&ba));
    TEST_ASSERT_EQUAL(3, ba.mark);

    // Read more data
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(3, value);
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(4, value);

    // Reset to marked position
    TEST_ASSERT_TRUE(bytearray_reset(&ba));
    TEST_ASSERT_EQUAL(3, ba.offset);

    // Read should continue from marked position
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(3, value);

    bytearray_dealloc(&ba);
}

/* Test capacity expansion */
TEST_CASE(test_bytearray_capacity_expansion) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 10)); // Small initial size

    // Write data that should trigger capacity expansion
    uint8_t large_buffer[100];
    memset(large_buffer, 0xCC, 100);

    TEST_ASSERT_TRUE(bytearray_writebuffer(&ba, large_buffer, 100));
    TEST_ASSERT_TRUE(ba.buflen >= 100); // Buffer should have expanded
    TEST_ASSERT_EQUAL(100, ba.offset);

    // Verify data integrity after expansion
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    uint8_t read_buffer[100];
    TEST_ASSERT_TRUE(bytearray_readbuffer(&ba, read_buffer, 100));

    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(0xCC, read_buffer[i]);
    }

    bytearray_dealloc(&ba);
}

/* Test read availability and empty functionality */
TEST_CASE(test_bytearray_availability) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Write some data
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_TRUE(bytearray_write8(&ba, (uint8_t)i));
    }

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    TEST_ASSERT_EQUAL(20, bytearray_read_available(&ba));

    // Read some data
    uint8_t value;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    }

    // Check remaining availability
    TEST_ASSERT_EQUAL(15, bytearray_read_available(&ba));

    // Test empty functionality
    TEST_ASSERT_TRUE(bytearray_empty(&ba));
    TEST_ASSERT_EQUAL(0, ba.offset);
    TEST_ASSERT_EQUAL(0, ba.total);

    bytearray_dealloc(&ba);
}

/* Test edge cases and error conditions */
TEST_CASE(test_bytearray_edge_cases) {
    BYTEARRAY ba;
    uint8_t value;

    // Test operations on uninitialized bytearray
    memset(&ba, 0, sizeof(BYTEARRAY));
    TEST_ASSERT_FALSE(bytearray_read_ready(&ba));
    TEST_ASSERT_EQUAL(0, bytearray_read_available(&ba));
    TEST_ASSERT_FALSE(bytearray_mark(&ba));
    TEST_ASSERT_FALSE(bytearray_reset(&ba));

    // Initialize properly
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 10));

    // Test reading beyond available data
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    TEST_ASSERT_FALSE(bytearray_read8(&ba, &value)); // No data to read

    // Switch back to write mode and add some data
    TEST_ASSERT_TRUE(bytearray_write_ready(&ba));
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 0x42));

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Try to read more than available
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &value));
    TEST_ASSERT_EQUAL(0x42, value);
    TEST_ASSERT_FALSE(bytearray_read8(&ba, &value)); // Should fail

    // Test NULL pointer handling in readbuffer
    TEST_ASSERT_TRUE(bytearray_write_ready(&ba));
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 0x55));
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    TEST_ASSERT_TRUE(bytearray_readbuffer(&ba, NULL, 1)); // Should succeed but not copy data

    bytearray_dealloc(&ba);
}

/* Test wrapped buffer constraints */
TEST_CASE(test_bytearray_wrapped_buffer_constraints) {
    BYTEARRAY ba;
    uint8_t external_buffer[50];
    uint8_t large_data[100];

    memset(large_data, 0xFF, 100);

    // Wrap a small buffer and add some initial data
    TEST_ASSERT_TRUE(bytearray_wrap_buffer(&ba, external_buffer, 50));

    // Read some data first to test write_ready behavior
    uint8_t dummy;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &dummy)); // Read first byte

    // Switch to write mode - this should work since we're in reading mode
    TEST_ASSERT_TRUE(bytearray_write_ready(&ba));

    // Try to write more data than remaining buffer can hold
    // After write_ready, offset starts at total-1 (49), and we have 1 remaining byte
    TEST_ASSERT_FALSE(bytearray_writebuffer(&ba, large_data, 50));

    // Writing a small amount should work
    TEST_ASSERT_TRUE(bytearray_writebuffer(&ba, large_data, 1));

    // No deallocation needed for wrapped buffer
    bytearray_dealloc(&ba);
}

/* Set up test suite */
TEST_SUITE_BEGIN(bytearray)
    TEST_SUITE_ADD(test_bytearray_alloc_dealloc)
    TEST_SUITE_ADD(test_bytearray_wrap_buffer)
    TEST_SUITE_ADD(test_bytearray_state_transitions)
    TEST_SUITE_ADD(test_bytearray_numeric_operations)
    TEST_SUITE_ADD(test_bytearray_buffer_operations)
    TEST_SUITE_ADD(test_bytearray_mark_reset)
    TEST_SUITE_ADD(test_bytearray_capacity_expansion)
    TEST_SUITE_ADD(test_bytearray_availability)
    TEST_SUITE_ADD(test_bytearray_edge_cases)
    TEST_SUITE_ADD(test_bytearray_wrapped_buffer_constraints)
TEST_SUITE_END(bytearray)

TEST_SUITE_ADD_NAME(test_bytearray_alloc_dealloc)
TEST_SUITE_ADD_NAME(test_bytearray_wrap_buffer)
TEST_SUITE_ADD_NAME(test_bytearray_state_transitions)
TEST_SUITE_ADD_NAME(test_bytearray_numeric_operations)
TEST_SUITE_ADD_NAME(test_bytearray_buffer_operations)
TEST_SUITE_ADD_NAME(test_bytearray_mark_reset)
TEST_SUITE_ADD_NAME(test_bytearray_capacity_expansion)
TEST_SUITE_ADD_NAME(test_bytearray_availability)
TEST_SUITE_ADD_NAME(test_bytearray_edge_cases)
TEST_SUITE_ADD_NAME(test_bytearray_wrapped_buffer_constraints)

TEST_SUITE_FINISH(bytearray)

/* Test suite setup/teardown functions */
void bytearray_setup(void) {
    printf("Setting up bytearray test suite...\n");
}

void bytearray_teardown(void) {
    printf("Tearing down bytearray test suite...\n");
}

/* Test suite is now run from test_main.c */