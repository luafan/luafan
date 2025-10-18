#!/bin/bash

# LuaFan Lua Test Runner
# This script runs all Lua unit tests

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo -e "${YELLOW}LuaFan Lua Test Runner${NC}"
echo "=================================="

# Check if LuaJIT is available
if command -v luajit >/dev/null 2>&1; then
    LUA_CMD="luajit"
    echo "Using LuaJIT"
elif command -v lua >/dev/null 2>&1; then
    LUA_CMD="lua"
    echo "Using Lua"
else
    echo -e "${RED}Error: Neither lua nor luajit found in PATH${NC}"
    exit 1
fi

# Set up Lua path to include project modules and test framework
export LUA_PATH="$PROJECT_ROOT/modules/?.lua;$PROJECT_ROOT/modules/?/init.lua;$SCRIPT_DIR/lua/framework/?.lua;$SCRIPT_DIR/lua/?.lua;;"
export LUA_CPATH="$PROJECT_ROOT/?.so;;"

# Check if LuaFan is available
echo "Checking LuaFan availability..."
if ! $LUA_CMD -e "require('fan')" >/dev/null 2>&1; then
    echo -e "${YELLOW}Warning: LuaFan module not available. Some tests may be skipped.${NC}"
fi

# Find all test files
TEST_FILES=($(find "$SCRIPT_DIR/lua" -name "test_*.lua" -type f | sort))

if [ ${#TEST_FILES[@]} -eq 0 ]; then
    echo -e "${YELLOW}No Lua test files found${NC}"
    exit 0
fi

echo "Found ${#TEST_FILES[@]} test file(s):"
for file in "${TEST_FILES[@]}"; do
    echo "  - $(basename "$file")"
done
echo

# Use run_all_lua_tests.lua to provide fan.loop context for all tests
echo -e "${YELLOW}Running all Lua tests in fan.loop context...${NC}"

ALL_LUA_TESTS_SCRIPT="$SCRIPT_DIR/lua/run_all_lua_tests.lua"
if [ -f "$ALL_LUA_TESTS_SCRIPT" ]; then
    # Run with timeout
    if timeout 30s $LUA_CMD "$ALL_LUA_TESTS_SCRIPT"; then
        echo -e "${GREEN}✓ All Lua tests completed successfully${NC}"
        TOTAL_FAILURES=0
        TESTS_RUN=1
    else
        exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "${RED}✗ Lua tests timed out (30s limit)${NC}"
        else
            echo -e "${RED}✗ Lua tests failed${NC}"
        fi
        TOTAL_FAILURES=1
        TESTS_RUN=1
    fi
else
    echo -e "${RED}Error: run_all_lua_tests.lua not found${NC}"
    TOTAL_FAILURES=1
    TESTS_RUN=1
fi

# Summary
echo "=================================="
echo "Lua Test Summary:"
echo "  Total test files: $TESTS_RUN"
echo "  Passed: $((TESTS_RUN - TOTAL_FAILURES))"
echo "  Failed: $TOTAL_FAILURES"

if [ $TOTAL_FAILURES -eq 0 ]; then
    echo -e "${GREEN}All Lua tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some Lua tests failed!${NC}"
    exit 1
fi