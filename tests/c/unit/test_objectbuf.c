#include "test_framework.h"
#include "bytearray.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

/* Forward declarations of FFI functions from objectbuf.c */
void ffi_stream_add_u30(BYTEARRAY *ba, uint32_t u);
void ffi_stream_add_d64(BYTEARRAY *ba, double value);
void ffi_stream_add_string(BYTEARRAY *ba, const char *data, size_t len);
void ffi_stream_add_bytes(BYTEARRAY *ba, const char *data, size_t len);

bool ffi_stream_get_u30(BYTEARRAY *ba, uint32_t *result);
bool ffi_stream_get_d64(BYTEARRAY *ba, double *result);
void ffi_stream_get_string(BYTEARRAY *ba, uint8_t **buff, size_t *buflen);

/* Test constants from objectbuf.c */
#define HAS_NUMBER_MASK 1 << 7
#define HAS_U30_MASK 1 << 6
#define HAS_STRING_MASK 1 << 5
#define HAS_FUNCTION_MASK 1 << 4
#define HAS_TABLE_MASK 1 << 3
#define TRUE_FALSE_MASK 1 << 0

#define MAX_U30 4294967296 // 2^32

/* Test the basic FFI stream functions that objectbuf depends on */
TEST_CASE(test_ffi_stream_u30_operations) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Test adding U30 values
    uint32_t test_values[] = {0, 1, 127, 128, 255, 256, 16383, 16384, 65535, 65536, 0x1FFFFF, 0x3FFFFFFF};
    size_t num_values = sizeof(test_values) / sizeof(test_values[0]);

    for (size_t i = 0; i < num_values; i++) {
        ffi_stream_add_u30(&ba, test_values[i]);
    }

    // Switch to read mode
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Test reading U30 values
    for (size_t i = 0; i < num_values; i++) {
        uint32_t result;
        TEST_ASSERT_TRUE(ffi_stream_get_u30(&ba, &result));
        TEST_ASSERT_EQUAL(test_values[i], result);
    }

    // Test reading beyond available data
    uint32_t result;
    TEST_ASSERT_FALSE(ffi_stream_get_u30(&ba, &result));

    bytearray_dealloc(&ba);
}

/* Test double (d64) operations */
TEST_CASE(test_ffi_stream_d64_operations) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Test various double values
    double test_values[] = {0.0, 1.0, -1.0, 3.14159, -3.14159, 1e10, -1e10, 1e-10, -1e-10};
    size_t num_values = sizeof(test_values) / sizeof(test_values[0]);

    for (size_t i = 0; i < num_values; i++) {
        ffi_stream_add_d64(&ba, test_values[i]);
    }

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    for (size_t i = 0; i < num_values; i++) {
        double result;
        TEST_ASSERT_TRUE(ffi_stream_get_d64(&ba, &result));
        TEST_ASSERT(fabs(result - test_values[i]) < 1e-15, "Double value mismatch");
    }

    double result;
    TEST_ASSERT_FALSE(ffi_stream_get_d64(&ba, &result));

    bytearray_dealloc(&ba);
}

/* Test string operations */
TEST_CASE(test_ffi_stream_string_operations) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 1000));

    // Test various strings
    const char* test_strings[] = {
        "",
        "a",
        "hello",
        "Hello, World!",
        "This is a longer string with special chars: !@#$%^&*()",
        "UTF-8 test: 你好世界"
    };
    size_t num_strings = sizeof(test_strings) / sizeof(test_strings[0]);

    // Add strings
    for (size_t i = 0; i < num_strings; i++) {
        ffi_stream_add_string(&ba, test_strings[i], strlen(test_strings[i]));
    }

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Read and verify strings
    for (size_t i = 0; i < num_strings; i++) {
        uint8_t *buff = NULL;
        size_t buflen = 0;
        ffi_stream_get_string(&ba, &buff, &buflen);

        TEST_ASSERT_NOT_NULL(buff);
        TEST_ASSERT_EQUAL(strlen(test_strings[i]), buflen);
        TEST_ASSERT(memcmp(test_strings[i], buff, buflen) == 0, "String content mismatch");
    }

    bytearray_dealloc(&ba);
}

/* Test string operations with insufficient data */
TEST_CASE(test_ffi_stream_string_insufficient_data) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Add a string that requires more data than available
    const char* test_str = "hello";
    ffi_stream_add_string(&ba, test_str, strlen(test_str));

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Manually corrupt the stream by reducing total
    ba.total = 2; // Less than required for the string

    uint8_t *buff = NULL;
    size_t buflen = 0;
    ffi_stream_get_string(&ba, &buff, &buflen);

    // Should return NULL buff and required length
    TEST_ASSERT_NULL(buff);
    TEST_ASSERT_TRUE(buflen > 0); // Should indicate required length

    bytearray_dealloc(&ba);
}

/* Test bytes operations */
TEST_CASE(test_ffi_stream_bytes_operations) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 1000));

    // Create test data
    uint8_t test_data[256];
    for (int i = 0; i < 256; i++) {
        test_data[i] = (uint8_t)i;
    }

    // Add bytes
    ffi_stream_add_bytes(&ba, (const char*)test_data, 256);

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    TEST_ASSERT_EQUAL(256, bytearray_read_available(&ba));

    bytearray_dealloc(&ba);
}

/* Test mask constants */
TEST_CASE(test_objectbuf_mask_constants) {
    // Verify mask values are as expected
    TEST_ASSERT_EQUAL(128, HAS_NUMBER_MASK);   // 1 << 7
    TEST_ASSERT_EQUAL(64, HAS_U30_MASK);       // 1 << 6
    TEST_ASSERT_EQUAL(32, HAS_STRING_MASK);    // 1 << 5
    TEST_ASSERT_EQUAL(16, HAS_FUNCTION_MASK);  // 1 << 4
    TEST_ASSERT_EQUAL(8, HAS_TABLE_MASK);      // 1 << 3
    TEST_ASSERT_EQUAL(1, TRUE_FALSE_MASK);     // 1 << 0

    // Test mask combinations
    uint8_t combined = HAS_NUMBER_MASK | HAS_STRING_MASK;
    TEST_ASSERT_EQUAL(160, combined); // 128 + 32

    TEST_ASSERT_TRUE((combined & HAS_NUMBER_MASK) != 0);
    TEST_ASSERT_TRUE((combined & HAS_STRING_MASK) != 0);
    TEST_ASSERT_FALSE((combined & HAS_U30_MASK) != 0);
}

/* Test U30 encoding edge cases */
TEST_CASE(test_u30_encoding_edge_cases) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 100));

    // Test boundary values for U30 encoding
    uint32_t edge_cases[] = {
        0,          // Minimum
        127,        // 7-bit boundary
        128,        // First 2-byte value
        16383,      // 14-bit boundary
        16384,      // First 3-byte value
        2097151,    // 21-bit boundary
        2097152,    // First 4-byte value
        268435455,  // 28-bit boundary
        268435456   // First 5-byte value
    };

    size_t num_cases = sizeof(edge_cases) / sizeof(edge_cases[0]);

    for (size_t i = 0; i < num_cases; i++) {
        ffi_stream_add_u30(&ba, edge_cases[i]);
    }

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    for (size_t i = 0; i < num_cases; i++) {
        uint32_t result;
        TEST_ASSERT_TRUE(ffi_stream_get_u30(&ba, &result));
        TEST_ASSERT_EQUAL(edge_cases[i], result);
    }

    bytearray_dealloc(&ba);
}

/* Test number classification logic */
TEST_CASE(test_number_classification) {
    // Test the logic used in packer_number for determining U30 vs double storage

    // Values that should be stored as U30 (integers within range)
    double u30_values[] = {0.0, 1.0, 100.0, 1000.0, 65535.0};
    size_t num_u30 = sizeof(u30_values) / sizeof(u30_values[0]);

    for (size_t i = 0; i < num_u30; i++) {
        double value = u30_values[i];
        // Test the classification logic: floor(value) == value && value < MAX_U30 && value >= 0
        TEST_ASSERT_TRUE(floor(value) == value);
        TEST_ASSERT_TRUE(value < MAX_U30);
        TEST_ASSERT_TRUE(value >= 0);
    }

    // Values that should be stored as double
    double double_values[] = {-1.0, 3.14159, MAX_U30, MAX_U30 + 1};
    size_t num_doubles = sizeof(double_values) / sizeof(double_values[0]);

    for (size_t i = 0; i < num_doubles; i++) {
        double value = double_values[i];
        // At least one condition should fail
        bool is_integer = (floor(value) == value);
        bool in_range = (value < MAX_U30);
        bool non_negative = (value >= 0);

        TEST_ASSERT_FALSE(is_integer && in_range && non_negative);
    }
}

/* Test empty and boundary conditions */
TEST_CASE(test_objectbuf_boundary_conditions) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 10));

    // Test empty string
    ffi_stream_add_string(&ba, "", 0);
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    uint8_t *buff = NULL;
    size_t buflen = 0;
    ffi_stream_get_string(&ba, &buff, &buflen);

    TEST_ASSERT_NOT_NULL(buff);
    TEST_ASSERT_EQUAL(0, buflen);

    // Reset for next test
    TEST_ASSERT_TRUE(bytearray_write_ready(&ba));
    TEST_ASSERT_TRUE(bytearray_empty(&ba));

    // Test zero value
    ffi_stream_add_u30(&ba, 0);
    ffi_stream_add_d64(&ba, 0.0);

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    uint32_t u30_result;
    double d64_result;

    TEST_ASSERT_TRUE(ffi_stream_get_u30(&ba, &u30_result));
    TEST_ASSERT_EQUAL(0, u30_result);

    TEST_ASSERT_TRUE(ffi_stream_get_d64(&ba, &d64_result));
    TEST_ASSERT(fabs(d64_result - 0.0) < 1e-15, "Double zero mismatch");

    bytearray_dealloc(&ba);
}

/* Test large data handling */
TEST_CASE(test_objectbuf_large_data) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 10000));

    // Test large string
    char large_string[5000];
    memset(large_string, 'A', 4999);
    large_string[4999] = '\0';

    ffi_stream_add_string(&ba, large_string, 4999);
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    uint8_t *buff = NULL;
    size_t buflen = 0;
    ffi_stream_get_string(&ba, &buff, &buflen);

    TEST_ASSERT_NOT_NULL(buff);
    TEST_ASSERT_EQUAL(4999, buflen);
    TEST_ASSERT(memcmp(large_string, buff, buflen) == 0, "Large string mismatch");

    bytearray_dealloc(&ba);
}

/* Test error conditions and robustness */
TEST_CASE(test_objectbuf_error_conditions) {
    BYTEARRAY ba;

    // Test with uninitialized bytearray
    memset(&ba, 0, sizeof(BYTEARRAY));

    uint32_t result_u30;
    double result_d64;
    uint8_t *buff = NULL;
    size_t buflen = 0;

    // These should fail gracefully
    TEST_ASSERT_FALSE(ffi_stream_get_u30(&ba, &result_u30));
    TEST_ASSERT_FALSE(ffi_stream_get_d64(&ba, &result_d64));

    ffi_stream_get_string(&ba, &buff, &buflen);
    TEST_ASSERT_NULL(buff);

    // Test with properly initialized but empty buffer
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 10));
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    TEST_ASSERT_FALSE(ffi_stream_get_u30(&ba, &result_u30));
    TEST_ASSERT_FALSE(ffi_stream_get_d64(&ba, &result_d64));

    ffi_stream_get_string(&ba, &buff, &buflen);
    TEST_ASSERT_NULL(buff);
    TEST_ASSERT_TRUE(buflen > 0); // Should indicate required length

    bytearray_dealloc(&ba);
}

/* Test data integrity across multiple operations */
TEST_CASE(test_objectbuf_data_integrity) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 1000));

    // Mix different data types
    ffi_stream_add_u30(&ba, 42);
    ffi_stream_add_d64(&ba, 3.14159);
    ffi_stream_add_string(&ba, "test", 4);
    ffi_stream_add_u30(&ba, 12345);
    ffi_stream_add_d64(&ba, -1.23);
    ffi_stream_add_string(&ba, "another", 7);

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));

    // Read back in same order
    uint32_t u30_1, u30_2;
    double d64_1, d64_2;
    uint8_t *str1_buff, *str2_buff;
    size_t str1_len, str2_len;

    TEST_ASSERT_TRUE(ffi_stream_get_u30(&ba, &u30_1));
    TEST_ASSERT_EQUAL(42, u30_1);

    TEST_ASSERT_TRUE(ffi_stream_get_d64(&ba, &d64_1));
    TEST_ASSERT(fabs(d64_1 - 3.14159) < 1e-5, "First double mismatch");

    ffi_stream_get_string(&ba, &str1_buff, &str1_len);
    TEST_ASSERT_NOT_NULL(str1_buff);
    TEST_ASSERT_EQUAL(4, str1_len);
    TEST_ASSERT(memcmp("test", str1_buff, 4) == 0, "First string mismatch");

    TEST_ASSERT_TRUE(ffi_stream_get_u30(&ba, &u30_2));
    TEST_ASSERT_EQUAL(12345, u30_2);

    TEST_ASSERT_TRUE(ffi_stream_get_d64(&ba, &d64_2));
    TEST_ASSERT(fabs(d64_2 - (-1.23)) < 1e-5, "Second double mismatch");

    ffi_stream_get_string(&ba, &str2_buff, &str2_len);
    TEST_ASSERT_NOT_NULL(str2_buff);
    TEST_ASSERT_EQUAL(7, str2_len);
    TEST_ASSERT(memcmp("another", str2_buff, 7) == 0, "Second string mismatch");

    bytearray_dealloc(&ba);
}

/* Set up test suite */
TEST_SUITE_BEGIN(objectbuf)
    TEST_SUITE_ADD(test_ffi_stream_u30_operations)
    TEST_SUITE_ADD(test_ffi_stream_d64_operations)
    TEST_SUITE_ADD(test_ffi_stream_string_operations)
    TEST_SUITE_ADD(test_ffi_stream_string_insufficient_data)
    TEST_SUITE_ADD(test_ffi_stream_bytes_operations)
    TEST_SUITE_ADD(test_objectbuf_mask_constants)
    TEST_SUITE_ADD(test_u30_encoding_edge_cases)
    TEST_SUITE_ADD(test_number_classification)
    TEST_SUITE_ADD(test_objectbuf_boundary_conditions)
    TEST_SUITE_ADD(test_objectbuf_large_data)
    TEST_SUITE_ADD(test_objectbuf_error_conditions)
    TEST_SUITE_ADD(test_objectbuf_data_integrity)
TEST_SUITE_END(objectbuf)

TEST_SUITE_ADD_NAME(test_ffi_stream_u30_operations)
TEST_SUITE_ADD_NAME(test_ffi_stream_d64_operations)
TEST_SUITE_ADD_NAME(test_ffi_stream_string_operations)
TEST_SUITE_ADD_NAME(test_ffi_stream_string_insufficient_data)
TEST_SUITE_ADD_NAME(test_ffi_stream_bytes_operations)
TEST_SUITE_ADD_NAME(test_objectbuf_mask_constants)
TEST_SUITE_ADD_NAME(test_u30_encoding_edge_cases)
TEST_SUITE_ADD_NAME(test_number_classification)
TEST_SUITE_ADD_NAME(test_objectbuf_boundary_conditions)
TEST_SUITE_ADD_NAME(test_objectbuf_large_data)
TEST_SUITE_ADD_NAME(test_objectbuf_error_conditions)
TEST_SUITE_ADD_NAME(test_objectbuf_data_integrity)

TEST_SUITE_FINISH(objectbuf)

/* Test suite setup/teardown functions */
void objectbuf_setup(void) {
    printf("Setting up objectbuf test suite...\n");
}

void objectbuf_teardown(void) {
    printf("Tearing down objectbuf test suite...\n");
}