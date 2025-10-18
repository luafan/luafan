#!/bin/bash

# LuaFan Unified Test Runner
# This script provides comprehensive testing: C tests, Lua tests, integration tests,
# performance benchmarks, and test coverage reporting

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
PURPLE='\033[0;35m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Test result tracking
declare -A TEST_RESULTS
TEST_RESULTS[c_tests]="skip"
TEST_RESULTS[lua_tests]="skip"
TEST_RESULTS[integration_tests]="skip"
TEST_RESULTS[performance_tests]="skip"
TEST_RESULTS[coverage_tests]="skip"

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║        LuaFan Unified Test Suite       ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo

# Parse command line arguments
BUILD_TESTS=true
RUN_C_TESTS=true
RUN_LUA_TESTS=true
RUN_INTEGRATION_TESTS=false
RUN_PERFORMANCE_TESTS=false
RUN_COVERAGE_TESTS=false
VERBOSE=false
CLEAN_BUILD=false
OUTPUT_FILE=""
QUICK_PERFORMANCE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --no-build)
            BUILD_TESTS=false
            shift
            ;;
        --c-only)
            RUN_LUA_TESTS=false
            RUN_INTEGRATION_TESTS=false
            RUN_PERFORMANCE_TESTS=false
            RUN_COVERAGE_TESTS=false
            shift
            ;;
        --lua-only)
            RUN_C_TESTS=false
            RUN_INTEGRATION_TESTS=false
            RUN_PERFORMANCE_TESTS=false
            RUN_COVERAGE_TESTS=false
            shift
            ;;
        --integration)
            RUN_INTEGRATION_TESTS=true
            shift
            ;;
        --performance)
            RUN_PERFORMANCE_TESTS=true
            shift
            ;;
        --coverage)
            RUN_COVERAGE_TESTS=true
            shift
            ;;
        --all)
            RUN_C_TESTS=true
            RUN_LUA_TESTS=true
            RUN_INTEGRATION_TESTS=true
            RUN_PERFORMANCE_TESTS=true
            RUN_COVERAGE_TESTS=true
            shift
            ;;
        --quick-perf)
            RUN_PERFORMANCE_TESTS=true
            QUICK_PERFORMANCE=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --output=*)
            OUTPUT_FILE="${1#*=}"
            shift
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Test Selection Options:"
            echo "  --c-only        Run only C tests"
            echo "  --lua-only      Run only Lua tests"
            echo "  --integration   Run integration tests"
            echo "  --performance   Run performance benchmarks"
            echo "  --coverage      Run test coverage analysis"
            echo "  --all           Run all test types"
            echo "  --quick-perf    Run quick performance tests (reduced iterations)"
            echo ""
            echo "Build Options:"
            echo "  --no-build      Skip building test executables"
            echo "  --clean         Clean build directory before building"
            echo ""
            echo "Output Options:"
            echo "  --verbose       Enable verbose output"
            echo "  --output=FILE   Save test results to JSON file"
            echo ""
            echo "Other Options:"
            echo "  --help          Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                    # Run basic tests (C + Lua)"
            echo "  $0 --all             # Run complete test suite"
            echo "  $0 --quick-perf      # Quick performance check"
            echo "  $0 --coverage --verbose  # Detailed coverage analysis"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Initialize JSON output if requested
if [ -n "$OUTPUT_FILE" ]; then
    echo "{" > "$OUTPUT_FILE"
    echo "  \"timestamp\": \"$(date -Iseconds)\"," >> "$OUTPUT_FILE"
    echo "  \"test_run_id\": \"$(date +%s)\"," >> "$OUTPUT_FILE"
    echo "  \"results\": {" >> "$OUTPUT_FILE"
fi

# Helper functions
log_test_result() {
    local test_name="$1"
    local status="$2"
    local details="$3"
    local duration="$4"

    TEST_RESULTS[$test_name]="$status"

    if [ -n "$OUTPUT_FILE" ]; then
        # Add comma if not the first result
        if [ "$test_name" != "c_tests" ] && [ "$(grep -c '\".*\":' "$OUTPUT_FILE")" -gt 3 ]; then
            sed -i '$ s/$/,/' "$OUTPUT_FILE"
        fi

        cat >> "$OUTPUT_FILE" << EOF
    "$test_name": {
      "status": "$status",
      "details": "$details",
      "duration_seconds": ${duration:-0}
    }
EOF
    fi
}

run_test_section() {
    local section_name="$1"
    local section_color="$2"
    local test_function="$3"

    echo -e "${section_color}$section_name${NC}"
    echo "────────────────────────────────────────"

    local start_time=$(date +%s)
    $test_function
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    return $duration
}

# Build tests if requested
if [ "$BUILD_TESTS" = true ]; then
    echo -e "${YELLOW}Building test framework and test executables...${NC}"

    # Create and enter build directory
    BUILD_DIR="$SCRIPT_DIR/build"

    if [ "$CLEAN_BUILD" = true ] && [ -d "$BUILD_DIR" ]; then
        echo "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Configure with CMake
    if [ "$VERBOSE" = true ]; then
        cmake .. -DCMAKE_BUILD_TYPE=Debug
    else
        cmake .. -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1
    fi

    # Build
    if [ "$VERBOSE" = true ]; then
        make
    else
        make >/dev/null 2>&1
    fi

    cd "$SCRIPT_DIR"
    echo -e "${GREEN}✓ Build completed${NC}"
    echo
fi

# Track overall results
OVERALL_SUCCESS=true

# Define test functions
run_c_tests() {
    local start_time=$(date +%s)
    local success=true
    local details="C unit tests"

    C_TEST_EXECUTABLE="$SCRIPT_DIR/build/run_c_tests"
    if [ -f "$C_TEST_EXECUTABLE" ]; then
        if [ "$VERBOSE" = true ]; then
            "$C_TEST_EXECUTABLE"
            local exit_code=$?
        else
            output=$("$C_TEST_EXECUTABLE" 2>&1)
            local exit_code=$?
            if [ $exit_code -ne 0 ] && [ "$VERBOSE" = true ]; then
                echo "$output"
            fi
        fi

        if [ $exit_code -eq 0 ]; then
            echo -e "${GREEN}✓ C tests passed${NC}"
            details="All C unit tests passed"
        else
            echo -e "${RED}✗ C tests failed${NC}"
            details="C unit tests failed with exit code $exit_code"
            success=false
            OVERALL_SUCCESS=false
        fi
    else
        echo -e "${YELLOW}⚠ No C test executable found${NC}"
        details="No C test executable found"
        success=false
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_test_result "c_tests" "$([[ $success == true ]] && echo 'pass' || echo 'fail')" "$details" "$duration"
}

# Run C tests
if [ "$RUN_C_TESTS" = true ]; then
    run_test_section "Running C unit tests..." "$YELLOW" run_c_tests
    echo
fi

run_lua_tests() {
    local start_time=$(date +%s)
    local success=true
    local details="Lua unit tests"

    LUA_TEST_SCRIPT="$SCRIPT_DIR/run_lua_tests.sh"
    if [ -f "$LUA_TEST_SCRIPT" ]; then
        if [ "$VERBOSE" = true ]; then
            "$LUA_TEST_SCRIPT"
            local exit_code=$?
        else
            output=$("$LUA_TEST_SCRIPT" 2>&1)
            local exit_code=$?
            if [ $exit_code -ne 0 ]; then
                echo "$output"
            fi
        fi

        if [ $exit_code -eq 0 ]; then
            echo -e "${GREEN}✓ Lua tests passed${NC}"
            details="All Lua unit tests passed"
        else
            echo -e "${RED}✗ Lua tests failed${NC}"
            details="Lua unit tests failed with exit code $exit_code"
            success=false
            OVERALL_SUCCESS=false
        fi
    else
        echo -e "${YELLOW}⚠ No Lua test script found${NC}"
        details="No Lua test script found"
        success=false
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_test_result "lua_tests" "$([[ $success == true ]] && echo 'pass' || echo 'fail')" "$details" "$duration"
}

run_integration_tests() {
    local start_time=$(date +%s)
    local success=true
    local details="Integration tests"

    INTEGRATION_TEST_SCRIPT="$SCRIPT_DIR/run_integration_tests.sh"
    if [ -f "$INTEGRATION_TEST_SCRIPT" ]; then
        if [ "$VERBOSE" = true ]; then
            "$INTEGRATION_TEST_SCRIPT"
            local exit_code=$?
        else
            output=$("$INTEGRATION_TEST_SCRIPT" 2>&1)
            local exit_code=$?
            if [ $exit_code -ne 0 ]; then
                echo "$output"
            fi
        fi

        if [ $exit_code -eq 0 ]; then
            echo -e "${GREEN}✓ Integration tests passed${NC}"
            details="All integration tests passed"
        else
            echo -e "${RED}✗ Integration tests failed${NC}"
            details="Integration tests failed with exit code $exit_code"
            success=false
            OVERALL_SUCCESS=false
        fi
    else
        echo -e "${YELLOW}⚠ No integration test script found${NC}"
        details="No integration test script found"
        success=false
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_test_result "integration_tests" "$([[ $success == true ]] && echo 'pass' || echo 'fail')" "$details" "$duration"
}

run_performance_tests() {
    local start_time=$(date +%s)
    local success=true
    local details="Performance benchmarks"

    PERF_TEST_SCRIPT="$SCRIPT_DIR/run_performance_tests.sh"
    if [ -f "$PERF_TEST_SCRIPT" ]; then
        local perf_args=""
        [ "$QUICK_PERFORMANCE" = true ] && perf_args="--quick"
        [ -n "$OUTPUT_FILE" ] && perf_args="$perf_args --output=${OUTPUT_FILE%.json}_performance.json"

        if [ "$VERBOSE" = true ]; then
            "$PERF_TEST_SCRIPT" $perf_args
            local exit_code=$?
        else
            output=$("$PERF_TEST_SCRIPT" $perf_args 2>&1)
            local exit_code=$?
            # Always show performance results summary
            echo "$output" | grep -E "(Results:|avg=|Tests run:|Performance testing completed)"
        fi

        if [ $exit_code -eq 0 ]; then
            echo -e "${GREEN}✓ Performance tests completed${NC}"
            details="Performance benchmarks completed successfully"
        else
            echo -e "${RED}✗ Performance tests failed${NC}"
            details="Performance tests failed with exit code $exit_code"
            success=false
            OVERALL_SUCCESS=false
        fi
    else
        echo -e "${YELLOW}⚠ No performance test script found${NC}"
        details="No performance test script found"
        success=false
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_test_result "performance_tests" "$([[ $success == true ]] && echo 'pass' || echo 'fail')" "$details" "$duration"
}

run_coverage_tests() {
    local start_time=$(date +%s)
    local success=true
    local details="Test coverage analysis"

    # Check if coverage tools are available
    if ! command -v gcov &> /dev/null; then
        echo -e "${YELLOW}⚠ gcov not available, skipping coverage analysis${NC}"
        details="gcov not available"
        success=false
    else
        echo -e "${CYAN}Generating test coverage report...${NC}"

        # Look for coverage files in build directory
        COVERAGE_DIR="$SCRIPT_DIR/build"
        if [ -d "$COVERAGE_DIR" ]; then
            cd "$COVERAGE_DIR"

            # Generate coverage report
            coverage_files=$(find . -name "*.gcno" | wc -l)
            if [ $coverage_files -gt 0 ]; then
                gcov CMakeFiles/run_c_tests.dir/c/unit/*.gcno > /dev/null 2>&1
                coverage_reports=$(ls -1 *.gcov 2>/dev/null | wc -l)

                if [ $coverage_reports -gt 0 ]; then
                    echo -e "${GREEN}✓ Generated $coverage_reports coverage reports${NC}"
                    if [ "$VERBOSE" = true ]; then
                        echo "Coverage files:"
                        ls -la *.gcov | head -5
                        [ $coverage_reports -gt 5 ] && echo "... and $((coverage_reports - 5)) more files"
                    fi
                    details="Generated $coverage_reports coverage reports"
                else
                    echo -e "${YELLOW}⚠ No coverage reports generated${NC}"
                    details="No coverage reports generated"
                    success=false
                fi
            else
                echo -e "${YELLOW}⚠ No coverage data files found${NC}"
                details="No coverage data files found"
                success=false
            fi

            cd "$SCRIPT_DIR"
        else
            echo -e "${YELLOW}⚠ No build directory found for coverage analysis${NC}"
            details="No build directory found"
            success=false
        fi
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_test_result "coverage_tests" "$([[ $success == true ]] && echo 'pass' || echo 'fail')" "$details" "$duration"
}

# Run Lua tests
if [ "$RUN_LUA_TESTS" = true ]; then
    run_test_section "Running Lua unit tests..." "$YELLOW" run_lua_tests
    echo
fi

# Run integration tests
if [ "$RUN_INTEGRATION_TESTS" = true ]; then
    run_test_section "Running integration tests..." "$CYAN" run_integration_tests
    echo
fi

# Run performance tests
if [ "$RUN_PERFORMANCE_TESTS" = true ]; then
    run_test_section "Running performance benchmarks..." "$PURPLE" run_performance_tests
    echo
fi

# Run coverage analysis
if [ "$RUN_COVERAGE_TESTS" = true ]; then
    run_test_section "Running test coverage analysis..." "$CYAN" run_coverage_tests
    echo
fi

# Complete JSON output
if [ -n "$OUTPUT_FILE" ]; then
    echo "" >> "$OUTPUT_FILE"
    echo "  }," >> "$OUTPUT_FILE"

    # Add summary section
    echo "  \"summary\": {" >> "$OUTPUT_FILE"

    # Count results
    local total_tests=0
    local passed_tests=0
    local failed_tests=0
    local skipped_tests=0

    for test in "${!TEST_RESULTS[@]}"; do
        case "${TEST_RESULTS[$test]}" in
            "pass")
                ((passed_tests++))
                ((total_tests++))
                ;;
            "fail")
                ((failed_tests++))
                ((total_tests++))
                ;;
            "skip")
                ((skipped_tests++))
                ;;
        esac
    done

    echo "    \"total_tests\": $total_tests," >> "$OUTPUT_FILE"
    echo "    \"passed_tests\": $passed_tests," >> "$OUTPUT_FILE"
    echo "    \"failed_tests\": $failed_tests," >> "$OUTPUT_FILE"
    echo "    \"skipped_tests\": $skipped_tests," >> "$OUTPUT_FILE"
    echo "    \"success_rate\": $(( total_tests > 0 ? (passed_tests * 100) / total_tests : 0 ))," >> "$OUTPUT_FILE"
    echo "    \"overall_result\": \"$([[ $OVERALL_SUCCESS == true ]] && echo 'PASS' || echo 'FAIL')\"" >> "$OUTPUT_FILE"
    echo "  }" >> "$OUTPUT_FILE"
    echo "}" >> "$OUTPUT_FILE"

    echo -e "${GREEN}✓ Test results saved to $OUTPUT_FILE${NC}"
fi

# Final summary
echo -e "${BLUE}═══════════════════════════════════════${NC}"
echo -e "${BLUE}            Test Results Summary         ${NC}"
echo -e "${BLUE}═══════════════════════════════════════${NC}"

# Display results for each test type
for test_type in c_tests lua_tests integration_tests performance_tests coverage_tests; do
    case "${TEST_RESULTS[$test_type]}" in
        "pass")
            echo -e "${GREEN}✓ $(echo $test_type | tr '_' ' ' | sed 's/\b\w/\U&/g'): PASSED${NC}"
            ;;
        "fail")
            echo -e "${RED}✗ $(echo $test_type | tr '_' ' ' | sed 's/\b\w/\U&/g'): FAILED${NC}"
            ;;
        "skip")
            echo -e "${YELLOW}⊝ $(echo $test_type | tr '_' ' ' | sed 's/\b\w/\U&/g'): SKIPPED${NC}"
            ;;
    esac
done

echo -e "${BLUE}═══════════════════════════════════════${NC}"

if [ "$OVERALL_SUCCESS" = true ]; then
    echo -e "${GREEN}🎉 ALL EXECUTED TESTS PASSED! 🎉${NC}"
    echo ""
    echo -e "Summary:"
    echo -e "• Tests run: $(( $(echo "${TEST_RESULTS[@]}" | tr ' ' '\n' | grep -c "pass\|fail") ))"
    echo -e "• Passed: $(echo "${TEST_RESULTS[@]}" | tr ' ' '\n' | grep -c "pass")"
    echo -e "• Failed: $(echo "${TEST_RESULTS[@]}" | tr ' ' '\n' | grep -c "fail")"
    echo -e "• Skipped: $(echo "${TEST_RESULTS[@]}" | tr ' ' '\n' | grep -c "skip")"
    [ -n "$OUTPUT_FILE" ] && echo -e "• Results saved to: $OUTPUT_FILE"
    exit 0
else
    echo -e "${RED}❌ SOME TESTS FAILED ❌${NC}"
    echo ""
    echo -e "Please check the test output above for details."
    echo -e "Use --verbose flag for more detailed error information."
    [ -n "$OUTPUT_FILE" ] && echo -e "Detailed results saved to: $OUTPUT_FILE"
    exit 1
fi