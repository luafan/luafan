#include "test_framework.h"
#include "bytearray.h"
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

/* Performance measurement utilities */
static double get_time_microseconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000.0 + tv.tv_usec;
}

#define BENCHMARK_ITERATIONS 100000
#define LARGE_BUFFER_SIZE 10240
#define SMALL_BUFFER_SIZE 64

/* Benchmark current implementation */
TEST_CASE(benchmark_current_implementation) {
    printf("\n=== PERFORMANCE BASELINE (Current Implementation) ===\n");

    // Test 1: Small writes performance
    {
        BYTEARRAY ba;
        bytearray_alloc(&ba, SMALL_BUFFER_SIZE);

        double start = get_time_microseconds();
        for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
            bytearray_write8(&ba, (uint8_t)i);
            if (ba.offset >= SMALL_BUFFER_SIZE - 1) {
                bytearray_empty(&ba);
            }
        }
        double end = get_time_microseconds();

        double time_us = end - start;
        double ops_per_sec = BENCHMARK_ITERATIONS / (time_us / 1000000.0);
        printf("Small writes (uint8):  %.2f us total, %.0f ops/sec\n", time_us, ops_per_sec);

        bytearray_dealloc(&ba);
    }

    // Test 2: Large buffer writes
    {
        BYTEARRAY ba;
        bytearray_alloc(&ba, LARGE_BUFFER_SIZE);
        uint8_t large_buffer[1024];
        memset(large_buffer, 0xAA, 1024);

        double start = get_time_microseconds();
        for (int i = 0; i < BENCHMARK_ITERATIONS / 100; i++) {
            bytearray_writebuffer(&ba, large_buffer, 1024);
            if (ba.offset >= LARGE_BUFFER_SIZE - 1024) {
                bytearray_empty(&ba);
            }
        }
        double end = get_time_microseconds();

        double time_us = end - start;
        double ops_per_sec = (BENCHMARK_ITERATIONS / 100) / (time_us / 1000000.0);
        double mb_per_sec = (ops_per_sec * 1024) / (1024 * 1024);
        printf("Large writes (1KB):    %.2f us total, %.0f ops/sec, %.2f MB/sec\n",
               time_us, ops_per_sec, mb_per_sec);

        bytearray_dealloc(&ba);
    }

    // Test 3: Capacity expansion performance
    {
        BYTEARRAY ba;
        bytearray_alloc(&ba, 64); // Start small

        double start = get_time_microseconds();
        for (int i = 0; i < BENCHMARK_ITERATIONS / 10; i++) {
            bytearray_write32(&ba, (uint32_t)i);
        }
        double end = get_time_microseconds();

        double time_us = end - start;
        double ops_per_sec = (BENCHMARK_ITERATIONS / 10) / (time_us / 1000000.0);
        printf("Expansion writes:      %.2f us total, %.0f ops/sec\n", time_us, ops_per_sec);
        printf("Final buffer size:     %u bytes\n", ba.buflen);

        bytearray_dealloc(&ba);
    }

    // Test 4: Read performance
    {
        BYTEARRAY ba;
        bytearray_alloc(&ba, LARGE_BUFFER_SIZE);

        // Fill with data
        for (int i = 0; i < BENCHMARK_ITERATIONS / 100; i++) {
            bytearray_write32(&ba, (uint32_t)i);
        }
        bytearray_read_ready(&ba);

        double start = get_time_microseconds();
        uint32_t value;
        for (int i = 0; i < BENCHMARK_ITERATIONS / 100; i++) {
            bytearray_read32(&ba, &value);
        }
        double end = get_time_microseconds();

        double time_us = end - start;
        double ops_per_sec = (BENCHMARK_ITERATIONS / 100) / (time_us / 1000000.0);
        printf("Read operations:       %.2f us total, %.0f ops/sec\n", time_us, ops_per_sec);

        bytearray_dealloc(&ba);
    }

    printf("=====================================================\n\n");
}

/* Test suite for performance benchmarks */
TEST_SUITE_BEGIN(bytearray_performance)
    TEST_SUITE_ADD(benchmark_current_implementation)
TEST_SUITE_END(bytearray_performance)

TEST_SUITE_ADD_NAME(benchmark_current_implementation)

TEST_SUITE_FINISH(bytearray_performance)

/* Test suite setup/teardown functions */
void bytearray_performance_setup(void) {
    printf("Setting up bytearray performance test suite...\n");
}

void bytearray_performance_teardown(void) {
    printf("Tearing down bytearray performance test suite...\n");
}