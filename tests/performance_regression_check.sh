#!/bin/bash

# LuaFan Performance Regression Testing
# This script compares current performance against baseline measurements
# and identifies potential performance regressions

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
BASELINE_FILE="$SCRIPT_DIR/performance_baseline.json"
CURRENT_RESULTS="$SCRIPT_DIR/performance_current.json"
REPORT_FILE="$SCRIPT_DIR/performance_regression_report.json"

# Regression thresholds (percentage increase that triggers warning/failure)
WARN_THRESHOLD=10  # 10% performance degradation triggers warning
FAIL_THRESHOLD=25  # 25% performance degradation triggers failure

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║     Performance Regression Testing     ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo

# Parse command line arguments
UPDATE_BASELINE=false
VERBOSE=false
QUICK_MODE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --update-baseline)
            UPDATE_BASELINE=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --update-baseline   Update performance baseline with current results"
            echo "  --verbose          Enable verbose output"
            echo "  --quick            Run quick performance tests (faster, less accurate)"
            echo "  --help             Show this help message"
            echo ""
            echo "Thresholds:"
            echo "  Warning: ${WARN_THRESHOLD}% performance degradation"
            echo "  Failure: ${FAIL_THRESHOLD}% performance degradation"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Run current performance tests
echo -e "${YELLOW}Running current performance tests...${NC}"
PERF_ARGS=""
[ "$QUICK_MODE" = true ] && PERF_ARGS="--quick"

if [ -f "$SCRIPT_DIR/run_performance_tests.sh" ]; then
    if [ "$VERBOSE" = true ]; then
        "$SCRIPT_DIR/run_performance_tests.sh" $PERF_ARGS --output="$CURRENT_RESULTS"
    else
        "$SCRIPT_DIR/run_performance_tests.sh" $PERF_ARGS --output="$CURRENT_RESULTS" >/dev/null 2>&1
    fi

    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ Performance tests failed${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Performance tests completed${NC}"
else
    echo -e "${RED}✗ Performance test script not found${NC}"
    exit 1
fi

# Check if baseline exists
if [ ! -f "$BASELINE_FILE" ]; then
    if [ "$UPDATE_BASELINE" = true ]; then
        echo -e "${YELLOW}Creating initial performance baseline...${NC}"
        cp "$CURRENT_RESULTS" "$BASELINE_FILE"
        echo -e "${GREEN}✓ Baseline created at $BASELINE_FILE${NC}"
        exit 0
    else
        echo -e "${YELLOW}⚠ No baseline file found. Use --update-baseline to create one.${NC}"
        echo -e "Current results saved to: $CURRENT_RESULTS"
        exit 0
    fi
fi

# Update baseline if requested
if [ "$UPDATE_BASELINE" = true ]; then
    echo -e "${YELLOW}Updating performance baseline...${NC}"
    cp "$CURRENT_RESULTS" "$BASELINE_FILE"
    echo -e "${GREEN}✓ Baseline updated${NC}"
    exit 0
fi

# Compare performance results
echo -e "${YELLOW}Comparing performance against baseline...${NC}"

# Simple JSON parsing function (requires jq for full functionality, but provides fallback)
get_test_time() {
    local file="$1"
    local test_name="$2"

    if command -v jq >/dev/null 2>&1; then
        jq -r ".tests[] | select(.name == \"$test_name\") | .avg_time_ms" "$file" 2>/dev/null || echo "0"
    else
        # Fallback: basic grep-based parsing
        grep -A 10 "\"name\": \"$test_name\"" "$file" | grep "avg_time_ms" | head -1 | sed 's/.*: //' | sed 's/[,}]//' || echo "0"
    fi
}

# Initialize report
cat > "$REPORT_FILE" << EOF
{
  "timestamp": "$(date -Iseconds)",
  "baseline_file": "$BASELINE_FILE",
  "current_file": "$CURRENT_RESULTS",
  "thresholds": {
    "warning": $WARN_THRESHOLD,
    "failure": $FAIL_THRESHOLD
  },
  "comparisons": [
EOF

# Get list of test names from current results
if command -v jq >/dev/null 2>&1; then
    test_names=$(jq -r '.tests[].name' "$CURRENT_RESULTS" 2>/dev/null)
else
    # Fallback: extract test names with grep
    test_names=$(grep '"name":' "$CURRENT_RESULTS" | sed 's/.*"name": "//' | sed 's/".*//')
fi

# Track overall regression status
OVERALL_STATUS="PASS"
WARNING_COUNT=0
FAILURE_COUNT=0
COMPARISON_COUNT=0

# Compare each test
first_comparison=true
for test_name in $test_names; do
    baseline_time=$(get_test_time "$BASELINE_FILE" "$test_name")
    current_time=$(get_test_time "$CURRENT_RESULTS" "$test_name")

    # Skip if either time is 0 (test not found or failed)
    if [ "$baseline_time" = "0" ] || [ "$current_time" = "0" ]; then
        [ "$VERBOSE" = true ] && echo -e "${YELLOW}⚠ Skipping $test_name (missing data)${NC}"
        continue
    fi

    # Calculate percentage change
    if [ "$baseline_time" != "0" ]; then
        # Using awk for floating point calculation
        percentage_change=$(awk "BEGIN {printf \"%.1f\", (($current_time - $baseline_time) / $baseline_time) * 100}")
        status="PASS"
        color="$GREEN"
        symbol="✓"

        # Check thresholds
        if awk "BEGIN {exit !($percentage_change > $FAIL_THRESHOLD)}"; then
            status="FAIL"
            color="$RED"
            symbol="✗"
            OVERALL_STATUS="FAIL"
            ((FAILURE_COUNT++))
        elif awk "BEGIN {exit !($percentage_change > $WARN_THRESHOLD)}"; then
            status="WARN"
            color="$YELLOW"
            symbol="⚠"
            [ "$OVERALL_STATUS" != "FAIL" ] && OVERALL_STATUS="WARN"
            ((WARNING_COUNT++))
        fi

        # Display result
        echo -e "${color}${symbol} $test_name: ${baseline_time}ms → ${current_time}ms (${percentage_change}%)${NC}"

        # Add to JSON report
        [ "$first_comparison" = false ] && echo "," >> "$REPORT_FILE"
        cat >> "$REPORT_FILE" << EOF
    {
      "test_name": "$test_name",
      "baseline_time_ms": $baseline_time,
      "current_time_ms": $current_time,
      "percentage_change": $percentage_change,
      "status": "$status"
    }
EOF
        first_comparison=false
        ((COMPARISON_COUNT++))
    fi
done

# Complete JSON report
cat >> "$REPORT_FILE" << EOF

  ],
  "summary": {
    "total_comparisons": $COMPARISON_COUNT,
    "warnings": $WARNING_COUNT,
    "failures": $FAILURE_COUNT,
    "overall_status": "$OVERALL_STATUS"
  }
}
EOF

echo
echo -e "${BLUE}═══════════════════════════════════════${NC}"
echo -e "${BLUE}        Regression Test Summary          ${NC}"
echo -e "${BLUE}═══════════════════════════════════════${NC}"

echo -e "Tests compared: $COMPARISON_COUNT"
echo -e "Warnings: $WARNING_COUNT (>${WARN_THRESHOLD}% slower)"
echo -e "Failures: $FAILURE_COUNT (>${FAIL_THRESHOLD}% slower)"
echo -e "Report saved to: $REPORT_FILE"

case "$OVERALL_STATUS" in
    "PASS")
        echo -e "${GREEN}🎉 No performance regressions detected! 🎉${NC}"
        exit 0
        ;;
    "WARN")
        echo -e "${YELLOW}⚠ Performance warnings detected${NC}"
        echo -e "Some tests show performance degradation above ${WARN_THRESHOLD}%"
        exit 0
        ;;
    "FAIL")
        echo -e "${RED}❌ Performance regressions detected! ❌${NC}"
        echo -e "Some tests show significant performance degradation above ${FAIL_THRESHOLD}%"
        exit 1
        ;;
esac