#include "test_framework.h"
#include <sys/time.h>

/* Global test state */
test_results_t g_test_results = {0};
int g_current_test_failed = 0;

/* Memory tracking for tests */
typedef struct memory_block {
    void* ptr;
    size_t size;
    struct memory_block* next;
} memory_block_t;

static memory_block_t* g_memory_blocks = NULL;
static int g_memory_leak_count = 0;

/* Helper function to get current time in seconds */
static double get_current_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* Initialize test framework */
void test_framework_init(void) {
    memset(&g_test_results, 0, sizeof(test_results_t));
    g_current_test_failed = 0;
    g_memory_blocks = NULL;
    g_memory_leak_count = 0;
}

/* Cleanup test framework */
void test_framework_cleanup(void) {
    /* Free any remaining memory blocks */
    memory_block_t* current = g_memory_blocks;
    while (current) {
        memory_block_t* next = current->next;
        free(current->ptr);
        free(current);
        current = next;
        g_memory_leak_count++;
    }
    g_memory_blocks = NULL;
}

/* Test memory allocation with tracking */
void* test_malloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) {
        memory_block_t* block = malloc(sizeof(memory_block_t));
        if (block) {
            block->ptr = ptr;
            block->size = size;
            block->next = g_memory_blocks;
            g_memory_blocks = block;
        }
    }
    return ptr;
}

/* Test memory deallocation with tracking */
void test_free(void* ptr) {
    if (!ptr) return;

    memory_block_t** current = &g_memory_blocks;
    while (*current) {
        if ((*current)->ptr == ptr) {
            memory_block_t* to_remove = *current;
            *current = (*current)->next;
            free(to_remove->ptr);
            free(to_remove);
            return;
        }
        current = &(*current)->next;
    }

    /* If we get here, the pointer wasn't tracked */
    free(ptr);
}

/* Reset memory tracking */
void test_memory_reset(void) {
    memory_block_t* current = g_memory_blocks;
    while (current) {
        memory_block_t* next = current->next;
        free(current->ptr);
        free(current);
        current = next;
    }
    g_memory_blocks = NULL;
    g_memory_leak_count = 0;
}

/* Get memory leak count */
int test_memory_get_leak_count(void) {
    int count = 0;
    memory_block_t* current = g_memory_blocks;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* Run a single test suite */
int run_test_suite(test_suite_t* suite) {
    if (!suite) return -1;

    printf("\n=== Running Test Suite: %s ===\n", suite->name);

    int suite_passed = 0;
    int suite_failed = 0;
    int suite_skipped = 0;
    double suite_start_time = get_current_time();

    for (int i = 0; i < suite->test_count; i++) {
        printf("Running test: %s ... ", suite->test_names[i]);
        fflush(stdout);

        /* Reset test state */
        g_current_test_failed = 0;
        test_memory_reset();

        /* Run setup if provided */
        if (suite->setup) {
            suite->setup();
        }

        double test_start_time = get_current_time();

        /* Run the test */
        suite->tests[i]();

        double test_end_time = get_current_time();
        double test_duration = test_end_time - test_start_time;

        /* Run teardown if provided */
        if (suite->teardown) {
            suite->teardown();
        }

        /* Check for memory leaks */
        int leak_count = test_memory_get_leak_count();
        if (leak_count > 0) {
            printf("FAIL (Memory leak: %d blocks)\n", leak_count);
            g_current_test_failed = 1;
        }

        /* Report test result */
        if (g_current_test_failed) {
            printf("FAIL (%.3fs)\n", test_duration);
            suite_failed++;
        } else {
            printf("PASS (%.3fs)\n", test_duration);
            suite_passed++;
        }

        g_test_results.total_tests++;
    }

    double suite_end_time = get_current_time();
    double suite_duration = suite_end_time - suite_start_time;

    printf("\nSuite '%s' completed: %d passed, %d failed, %d skipped (%.3fs)\n",
           suite->name, suite_passed, suite_failed, suite_skipped, suite_duration);

    g_test_results.passed_tests += suite_passed;
    g_test_results.failed_tests += suite_failed;
    g_test_results.skipped_tests += suite_skipped;
    g_test_results.total_time += suite_duration;

    return suite_failed;
}

/* Run all test suites */
int run_all_tests(test_suite_t* suites[], int suite_count) {
    printf("Starting test run with %d suites...\n", suite_count);

    test_framework_init();

    double start_time = get_current_time();
    int total_failures = 0;

    for (int i = 0; i < suite_count; i++) {
        int failures = run_test_suite(suites[i]);
        if (failures > 0) {
            total_failures += failures;
        }
    }

    double end_time = get_current_time();
    g_test_results.total_time = end_time - start_time;

    print_test_results();
    test_framework_cleanup();

    return total_failures;
}

/* Print final test results */
void print_test_results(void) {
    printf("\n============================================================\n");
    printf("TEST RESULTS SUMMARY\n");
    printf("============================================================\n");
    printf("Total Tests:   %d\n", g_test_results.total_tests);
    printf("Passed:        %d\n", g_test_results.passed_tests);
    printf("Failed:        %d\n", g_test_results.failed_tests);
    printf("Skipped:       %d\n", g_test_results.skipped_tests);
    printf("Success Rate:  %.1f%%\n",
           g_test_results.total_tests > 0 ?
           (100.0 * g_test_results.passed_tests / g_test_results.total_tests) : 0.0);
    printf("Total Time:    %.3f seconds\n", g_test_results.total_time);
    printf("============================================================\n");

    if (g_test_results.failed_tests > 0) {
        printf("RESULT: FAILED\n");
    } else {
        printf("RESULT: PASSED\n");
    }
}