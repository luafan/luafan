# LuaFan Comprehensive Testing Infrastructure

This is the comprehensive testing infrastructure for the LuaFan project, providing automated testing, performance benchmarking, regression detection, and CI/CD integration. It supports C and Lua unit tests, integration tests, performance analysis, and test coverage reporting.

## Directory Structure

```
tests/
├── README.md                           # This document (comprehensive guide)
├── CMakeLists.txt                      # CMake build configuration
├── run_all_tests.sh                    # Unified test runner (enhanced)
├── run_lua_tests.sh                    # Lua test runner script
├── run_performance_tests.sh            # Performance benchmarking runner
├── performance_regression_check.sh     # Performance regression detector
├── c/                                  # C language tests
│   ├── framework/                      # C testing framework
│   │   ├── test_framework.h            # Framework header file
│   │   └── test_framework.c            # Framework implementation
│   └── unit/                          # C unit tests
│       └── test_example.c              # Example test
├── lua/                                # Lua tests
│   ├── framework/                      # Lua testing framework
│   │   └── test_framework.lua          # Framework implementation
│   ├── unit/                          # Lua unit tests
│   ├── modules/                        # LuaFan module tests
│   │   └── test_fan_utils.lua          # fan.utils module test
│   └── test_example.lua                # Example test
├── integration/                        # Integration tests
│   ├── network/                       # Network-related integration tests
│   └── database/                      # Database-related integration tests
├── fixtures/                           # Test data and resources
├── performance_baseline.json           # Performance baseline measurements
├── performance_current.json            # Current performance results
└── performance_regression_report.json  # Regression analysis report
```

## Quick Start

### Basic Testing

```bash
cd tests

# Run basic tests (C + Lua)
./run_all_tests.sh

# Run complete test suite including performance and coverage
./run_all_tests.sh --all

# Quick performance check
./run_all_tests.sh --quick-perf

# Run with detailed output and save results
./run_all_tests.sh --all --verbose --output=results.json
```

### Test Selection Options

```bash
# Individual test types
./run_all_tests.sh --c-only        # Run only C tests
./run_all_tests.sh --lua-only      # Run only Lua tests
./run_all_tests.sh --integration   # Run integration tests
./run_all_tests.sh --performance   # Run performance benchmarks
./run_all_tests.sh --coverage      # Run test coverage analysis

# Combined options
./run_all_tests.sh --performance --coverage  # Performance + coverage
```

### Performance Testing

```bash
# Quick performance test (3 iterations)
./run_performance_tests.sh --quick

# Full performance test (10 iterations)
./run_performance_tests.sh --full

# Check for performance regressions
./performance_regression_check.sh

# Update performance baseline
./performance_regression_check.sh --update-baseline
```

### Build and Output Options

```bash
# Build options
./run_all_tests.sh --clean         # Clean build before running
./run_all_tests.sh --no-build      # Skip build and run directly

# Output options
./run_all_tests.sh --verbose       # Enable verbose output
./run_all_tests.sh --output=FILE   # Save test results to JSON file

# Help
./run_all_tests.sh --help         # Show all available options
```

## Testing Infrastructure Components

### 1. Unified Test Runner (`run_all_tests.sh`)

The main testing script that orchestrates all test types with comprehensive tracking and JSON output.

**Features:**
- All test types in one command
- JSON output for CI integration
- Detailed progress tracking and timing
- Comprehensive error handling and reporting
- Support for parallel and sequential execution

**Test Result Tracking:**
- Real-time status updates for each test category
- Duration measurement for each test phase
- Detailed error reporting with exit codes
- JSON export for CI/CD integration

### 2. Performance Benchmarking (`run_performance_tests.sh`)

Dedicated performance testing with statistical analysis and memory leak detection.

**Features:**
- Multiple iterations for statistical accuracy (3 for quick, 10 for full)
- Timing analysis (min, max, average)
- Memory leak detection with valgrind
- Concurrent stress testing
- JSON output with detailed performance metrics

**Test Categories:**
- ByteArray performance (allocation, manipulation)
- ObjectBuf performance (serialization, deserialization)
- Core framework performance (event loops, coroutines)
- Optimized vs standard implementations

### 3. Performance Regression Detection (`performance_regression_check.sh`)

Automated detection of performance regressions with configurable thresholds.

**Features:**
- Baseline performance tracking
- Configurable regression thresholds (10% warning, 25% failure)
- Detailed comparison reports with percentage changes
- JSON output for automated analysis
- Support for baseline updates after optimizations

**Workflow:**
1. Run current performance tests
2. Compare against stored baseline
3. Generate detailed regression report
4. Return appropriate exit codes for CI

### 4. Test Coverage Analysis

Integrated code coverage reporting using gcov for C components.

**Features:**
- Automatic coverage data generation during C test runs
- Coverage report generation with file-by-file analysis
- Integration with unified test runner
- Verbose output showing coverage statistics

### 5. CI/CD Integration (GitHub Actions)

Complete CI/CD pipeline with multi-platform testing and automated regression detection.

**Test Matrix:**
- Ubuntu 22.04 (Lua 5.3, 5.4)
- Alpine Linux (minimal environment)
- macOS 13 (Apple Silicon compatibility)

**Pipeline Stages:**
1. **Build Testing**: CMake build verification
2. **Unit Testing**: C and Lua unit tests
3. **Performance Monitoring**: Automated benchmarking with regression detection
4. **Security Scanning**: Static analysis with cppcheck and super-linter
5. **Integration Testing**: Database and network integration
6. **Coverage Reporting**: Test coverage analysis and artifact storage

## Test Types

### Unit Tests
- **C Unit Tests**: Core C module testing (tcpd, udpd, httpd, etc.)
- **Lua Unit Tests**: Lua module testing with event loop integration
- Memory safety validation and error handling verification

### Integration Tests
- End-to-end functionality testing
- Network connectivity and protocol testing
- Database integration (when available)
- Cross-module interaction validation

### Performance Tests
- Execution time benchmarking with statistical analysis
- Memory allocation performance testing
- Network throughput and latency measurement
- Concurrent operation stress testing
- Regression detection against historical baselines

### Coverage Analysis
- C code coverage using gcov
- Line and branch coverage reporting
- Coverage trend tracking over time
- Integration with CI for coverage enforcement

## Output Formats

### JSON Test Results

The unified test runner produces comprehensive JSON output:

```json
{
  "timestamp": "2024-10-18T08:30:00Z",
  "test_run_id": "1729242600",
  "results": {
    "c_tests": {
      "status": "pass",
      "details": "All C unit tests passed",
      "duration_seconds": 5
    },
    "performance_tests": {
      "status": "pass",
      "details": "Performance benchmarks completed successfully",
      "duration_seconds": 15
    }
  },
  "summary": {
    "total_tests": 5,
    "passed_tests": 5,
    "failed_tests": 0,
    "success_rate": 100,
    "overall_result": "PASS"
  }
}
```

### Performance Regression Reports

Detailed regression analysis with percentage changes:

```json
{
  "timestamp": "2024-10-18T08:30:00Z",
  "comparisons": [
    {
      "test_name": "ByteArray Allocation",
      "baseline_time_ms": 245,
      "current_time_ms": 251,
      "percentage_change": 2.4,
      "status": "PASS"
    }
  ],
  "summary": {
    "total_comparisons": 4,
    "warnings": 0,
    "failures": 0,
    "overall_status": "PASS"
  }
}
```

## Best Practices

### For Developers

1. **Pre-commit testing:**
   ```bash
   ./run_all_tests.sh --quick-perf
   ```

2. **After optimizations:**
   ```bash
   ./performance_regression_check.sh --update-baseline
   ```

3. **Debugging failures:**
   ```bash
   ./run_all_tests.sh --verbose --coverage
   ```

### For CI/CD

1. **Pull request validation:**
   ```bash
   ./run_all_tests.sh --all --output=pr_results.json
   ```

2. **Master branch validation:**
   ```bash
   ./run_all_tests.sh --all --verbose
   ./performance_regression_check.sh
   ```

3. **Release testing:**
   ```bash
   ./run_all_tests.sh --all --verbose --clean
   ./run_performance_tests.sh --full
   ```

## C Testing Framework

### Basic Usage

```c
#include "test_framework.h"

// Define test case
TEST_CASE(my_test) {
    TEST_ASSERT_EQUAL(42, 42);
    TEST_ASSERT_TRUE(1 > 0);
    TEST_ASSERT_NOT_NULL("test");
}

// Create test suite
TEST_SUITE_BEGIN(my_suite)
    TEST_SUITE_ADD(my_test)
TEST_SUITE_END(my_suite)

TEST_SUITE_ADD_NAME(my_test)
TEST_SUITE_FINISH(my_suite)

// Main function
int main(void) {
    test_suite_t* suites[] = { &my_suite_suite };
    return run_all_tests(suites, 1);
}
```

### Available Assertions

- `TEST_ASSERT(condition, message, ...)` - General assertion
- `TEST_ASSERT_EQUAL(expected, actual)` - Equality assertion
- `TEST_ASSERT_STRING_EQUAL(expected, actual)` - String equality
- `TEST_ASSERT_NULL(ptr)` - Null pointer assertion
- `TEST_ASSERT_NOT_NULL(ptr)` - Non-null pointer assertion
- `TEST_ASSERT_TRUE(condition)` - True value assertion
- `TEST_ASSERT_FALSE(condition)` - False value assertion

### Memory Tracking

The framework provides memory leak detection:

```c
void* ptr = test_malloc(100);  // Tracked allocation
test_free(ptr);                // Tracked deallocation
```

## Lua Testing Framework

### Basic Usage

```lua
local TestFramework = require('test_framework')

-- Create test suite
local suite = TestFramework.create_suite("My Tests")

-- Add test
suite:test("my_test", function()
    TestFramework.assert_equal(42, 42)
    TestFramework.assert_true(true)
    TestFramework.assert_not_nil("test")
end)

-- Set setUp/tearDown
suite:set_setup(function()
    print("Setup")
end)

suite:set_teardown(function()
    print("Teardown")
end)

-- Run test
local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
```

### Available Assertions

- `assert_equal(actual, expected, message)` - Equality assertion
- `assert_not_equal(actual, expected, message)` - Inequality assertion
- `assert_true(value, message)` - True value assertion
- `assert_false(value, message)` - False value assertion
- `assert_nil(value, message)` - nil assertion
- `assert_not_nil(value, message)` - Non-nil assertion
- `assert_type(value, expected_type, message)` - Type assertion
- `assert_match(string, pattern, message)` - Pattern matching assertion
- `assert_error(func, expected_error, message)` - Error assertion

### Hook Functions

```lua
suite:set_setup(func)           -- Run once before suite
suite:set_teardown(func)        -- Run once after suite
suite:set_before_each(func)     -- Run before each test
suite:set_after_each(func)      -- Run after each test
```

## Writing New Tests

### C Language Tests

1. Create `test_*.c` files in the `c/unit/` directory
2. Include `test_framework.h`
3. Define test cases and suites
4. Update `CMakeLists.txt` if special configuration is needed

### Lua Tests

1. Create `test_*.lua` files in `lua/` or its subdirectories
2. Import the testing framework: `local TestFramework = require('test_framework')`
3. Create test suites and add tests
4. Scripts should exit with appropriate exit codes

### Module Tests

For LuaFan module tests:

1. Create test files in the `lua/modules/` directory
2. Handle module unavailability gracefully
3. Test the module's public API
4. Test boundary conditions and error handling

## Integration Tests

Integration tests are located in the `integration/` directory for testing:

- Network functionality (TCP/UDP/HTTP)
- Database connections
- Event loop integration
- Coroutine interactions

## Test Data

Fixed data and resource files needed for tests are placed in the `fixtures/` directory.

## Continuous Integration

This testing framework is designed to be easily integrated into CI/CD pipelines:

```bash
# Run in CI environment
cd tests
./run_all_tests.sh --clean --verbose
```

Scripts return appropriate exit codes:
- 0: All tests passed
- 1: Some tests failed

## Contributing Guidelines

1. All new features should have corresponding tests
2. Tests should be independent and repeatable
3. Use descriptive test names
4. Test boundary conditions and error cases
5. Keep tests fast to execute

## Debugging Tests

Use the `--verbose` option for detailed output:

```bash
./run_all_tests.sh --verbose
```

For C tests, you can use gdb:

```bash
cd tests/build
gdb ./run_c_tests
```

For Lua tests, run individual test files directly:

```bash
cd tests
lua lua/test_example.lua
```