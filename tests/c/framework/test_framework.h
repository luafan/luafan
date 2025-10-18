#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* Test result structure */
typedef struct {
    int total_tests;
    int passed_tests;
    int failed_tests;
    int skipped_tests;
    double total_time;
} test_results_t;

/* Test function signature */
typedef void (*test_func_t)(void);

/* Test suite structure */
typedef struct {
    const char* name;
    test_func_t* tests;
    const char** test_names;
    int test_count;
    void (*setup)(void);
    void (*teardown)(void);
} test_suite_t;

/* Global test state */
extern test_results_t g_test_results;
extern int g_current_test_failed;

/* Macros for test assertions */
#define TEST_ASSERT(condition, message, ...) \
    do { \
        if (!(condition)) { \
            printf("FAIL: %s:%d - " message "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
            g_current_test_failed = 1; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    TEST_ASSERT((expected) == (actual), "Expected %d, got %d", (expected), (actual))

#define TEST_ASSERT_NOT_EQUAL(expected, actual) \
    TEST_ASSERT((expected) != (actual), "Expected not %d, but got %d", (expected), (actual))

#define TEST_ASSERT_STRING_EQUAL(expected, actual) \
    TEST_ASSERT(strcmp((expected), (actual)) == 0, "Expected '%s', got '%s'", (expected), (actual))

#define TEST_ASSERT_NULL(ptr) \
    TEST_ASSERT((ptr) == NULL, "Expected NULL, got %p", (ptr))

#define TEST_ASSERT_NOT_NULL(ptr) \
    TEST_ASSERT((ptr) != NULL, "Expected non-NULL, got NULL")

#define TEST_ASSERT_TRUE(condition) \
    TEST_ASSERT((condition), "Expected true, got false")

#define TEST_ASSERT_FALSE(condition) \
    TEST_ASSERT(!(condition), "Expected false, got true")

/* Test registration macros */
#define TEST_CASE(name) void test_##name(void)
#define TEST_SUITE_BEGIN(suite_name) \
    static test_func_t suite_name##_tests[] = {

#define TEST_SUITE_ADD(test_name) test_##test_name,

#define TEST_SUITE_END(suite_name) \
    }; \
    static const char* suite_name##_test_names[] = {

#define TEST_SUITE_ADD_NAME(test_name) #test_name,

#define TEST_SUITE_FINISH(suite_name) \
    }; \
    test_suite_t suite_name##_suite = { \
        .name = #suite_name, \
        .tests = suite_name##_tests, \
        .test_names = suite_name##_test_names, \
        .test_count = sizeof(suite_name##_tests) / sizeof(test_func_t), \
        .setup = NULL, \
        .teardown = NULL \
    };

/* Function declarations */
void test_framework_init(void);
void test_framework_cleanup(void);
int run_test_suite(test_suite_t* suite);
int run_all_tests(test_suite_t* suites[], int suite_count);
void print_test_results(void);

/* Memory tracking for tests */
void* test_malloc(size_t size);
void test_free(void* ptr);
void test_memory_reset(void);
int test_memory_get_leak_count(void);

#endif /* TEST_FRAMEWORK_H */