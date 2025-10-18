#!/bin/bash

# LuaFan Performance Test Runner
# Runs performance benchmarks and tracks regression

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_FILE=""
QUICK_MODE=false
FULL_MODE=false

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║        LuaFan Performance Tests        ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --full)
            FULL_MODE=true
            shift
            ;;
        --output=*)
            OUTPUT_FILE="${1#*=}"
            shift
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo "Options:"
            echo "  --quick           Run quick performance tests (reduced iterations)"
            echo "  --full            Run full performance test suite"
            echo "  --output=FILE     Output results to JSON file"
            echo "  --help            Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Default to quick mode if neither specified
if [ "$QUICK_MODE" = false ] && [ "$FULL_MODE" = false ]; then
    QUICK_MODE=true
fi

# JSON output initialization
if [ -n "$OUTPUT_FILE" ]; then
    echo "{" > "$OUTPUT_FILE"
    echo "  \"timestamp\": \"$(date -Iseconds)\"," >> "$OUTPUT_FILE"
    echo "  \"mode\": \"$([ "$FULL_MODE" = true ] && echo "full" || echo "quick")\"," >> "$OUTPUT_FILE"
    echo "  \"tests\": [" >> "$OUTPUT_FILE"
fi

test_count=0

# Function to run a performance test and record results
run_perf_test() {
    local test_name="$1"
    local test_executable="$2"
    local iterations="$3"
    local description="$4"

    echo -e "${YELLOW}Running $test_name...${NC}"

    if [ ! -f "$test_executable" ]; then
        echo -e "${YELLOW}⚠ $test_executable not found, skipping${NC}"
        return
    fi

    # Run the test multiple times and capture timing
    local total_time=0
    local min_time=999999
    local max_time=0
    local successful_runs=0

    for ((i=1; i<=iterations; i++)); do
        echo -n "  Run $i/$iterations... "

        # Time the execution
        local start_time=$(date +%s%N)
        if timeout 30s "$test_executable" >/dev/null 2>&1; then
            local end_time=$(date +%s%N)
            local run_time=$(( (end_time - start_time) / 1000000 )) # Convert to milliseconds

            total_time=$((total_time + run_time))
            successful_runs=$((successful_runs + 1))

            if [ $run_time -lt $min_time ]; then
                min_time=$run_time
            fi
            if [ $run_time -gt $max_time ]; then
                max_time=$run_time
            fi

            echo -e "${GREEN}${run_time}ms${NC}"
        else
            echo -e "${RED}FAILED${NC}"
        fi
    done

    if [ $successful_runs -gt 0 ]; then
        local avg_time=$((total_time / successful_runs))
        echo -e "  Results: ${GREEN}avg=${avg_time}ms${NC}, min=${min_time}ms, max=${max_time}ms (${successful_runs}/${iterations} runs)"

        # Record to JSON if requested
        if [ -n "$OUTPUT_FILE" ]; then
            [ $test_count -gt 0 ] && echo "," >> "$OUTPUT_FILE"
            cat >> "$OUTPUT_FILE" << EOF
    {
      "name": "$test_name",
      "description": "$description",
      "iterations": $iterations,
      "successful_runs": $successful_runs,
      "avg_time_ms": $avg_time,
      "min_time_ms": $min_time,
      "max_time_ms": $max_time
    }
EOF
            test_count=$((test_count + 1))
        fi
    else
        echo -e "  ${RED}All runs failed!${NC}"
    fi
    echo
}

# Determine iteration counts based on mode
if [ "$QUICK_MODE" = true ]; then
    ITERATIONS=3
    echo -e "${YELLOW}Running in quick mode (${ITERATIONS} iterations per test)${NC}"
else
    ITERATIONS=10
    echo -e "${YELLOW}Running in full mode (${ITERATIONS} iterations per test)${NC}"
fi
echo

# ByteArray performance tests
echo -e "${BLUE}ByteArray Performance Tests${NC}"
echo "────────────────────────────────────────"
run_perf_test "ByteArray Allocation" "$SCRIPT_DIR/bytearray_performance_test" $ITERATIONS "Memory allocation and basic operations"

# ObjectBuf performance tests
echo -e "${BLUE}ObjectBuf Performance Tests${NC}"
echo "────────────────────────────────────────"
run_perf_test "ObjectBuf Serialization" "$SCRIPT_DIR/objectbuf_performance_test" $ITERATIONS "Object serialization and deserialization"

# General performance tests
echo -e "${BLUE}General Performance Tests${NC}"
echo "────────────────────────────────────────"
run_perf_test "Core Performance" "$SCRIPT_DIR/performance_test" $ITERATIONS "Core framework performance"
run_perf_test "Simple Performance" "$SCRIPT_DIR/simple_perf_test" $ITERATIONS "Simple operations benchmark"

# Check for optimized versions
echo -e "${BLUE}Optimized Performance Tests${NC}"
echo "────────────────────────────────────────"

# Look for any optimized test executables
for optimized_test in "$SCRIPT_DIR"/*_optimized "$SCRIPT_DIR"/*_final "$SCRIPT_DIR"/*_cleaned; do
    if [ -f "$optimized_test" ]; then
        test_basename=$(basename "$optimized_test")
        run_perf_test "Optimized: $test_basename" "$optimized_test" $ITERATIONS "Optimized performance test"
    fi
done

# If in full mode, run stress tests
if [ "$FULL_MODE" = true ]; then
    echo -e "${BLUE}Stress Tests (Full Mode Only)${NC}"
    echo "────────────────────────────────────────"

    # Memory stress test (if available)
    if command -v valgrind >/dev/null 2>&1; then
        echo -e "${YELLOW}Running memory leak detection...${NC}"
        if [ -f "$SCRIPT_DIR/build/run_c_tests" ]; then
            echo -n "  Memory leak test... "
            if timeout 60s valgrind --leak-check=full --error-exitcode=1 "$SCRIPT_DIR/build/run_c_tests" >/dev/null 2>&1; then
                echo -e "${GREEN}PASSED (no leaks)${NC}"
            else
                echo -e "${RED}FAILED (leaks detected)${NC}"
            fi
        fi
        echo
    fi

    # Concurrent stress test
    if [ -f "$SCRIPT_DIR/performance_test" ]; then
        echo -e "${YELLOW}Running concurrent stress test...${NC}"
        echo -n "  Concurrent execution test... "

        # Run multiple instances in parallel
        for i in {1..4}; do
            timeout 15s "$SCRIPT_DIR/performance_test" >/dev/null 2>&1 &
        done

        wait
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}PASSED${NC}"
        else
            echo -e "${RED}FAILED${NC}"
        fi
        echo
    fi
fi

# Finalize JSON output
if [ -n "$OUTPUT_FILE" ]; then
    echo "" >> "$OUTPUT_FILE"
    echo "  ]," >> "$OUTPUT_FILE"
    echo "  \"summary\": {" >> "$OUTPUT_FILE"
    echo "    \"total_tests\": $test_count," >> "$OUTPUT_FILE"
    echo "    \"mode\": \"$([ "$FULL_MODE" = true ] && echo "full" || echo "quick")\"" >> "$OUTPUT_FILE"
    echo "  }" >> "$OUTPUT_FILE"
    echo "}" >> "$OUTPUT_FILE"

    echo -e "${GREEN}✓ Results saved to $OUTPUT_FILE${NC}"
fi

# Final summary
echo -e "${BLUE}═══════════════════════════════════════${NC}"
echo -e "${GREEN}🎉 Performance testing completed! 🎉${NC}"
echo -e "Mode: $([ "$FULL_MODE" = true ] && echo "Full" || echo "Quick")"
echo -e "Tests run: $test_count"

if [ -n "$OUTPUT_FILE" ]; then
    echo -e "Results: $OUTPUT_FILE"
fi

exit 0