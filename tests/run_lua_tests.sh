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

# Interpreter selection.
#   LUAFAN_LUA_BIN            explicit interpreter (e.g. /usr/local/bin/lua from
#                             tests/build_hooked_lua.sh -- the "hooked" build)
#   luajit / lua              whatever PATH offers otherwise
# A hooked interpreter owns the global Lua lock itself; a stock one makes luafan
# fall back to its resume wrapper. Either way the lock mode is printed below so a
# test log always says which shape was exercised (see docs/threading-model.md).
if [ -n "${LUAFAN_LUA_BIN:-}" ]; then
    LUA_CMD="$LUAFAN_LUA_BIN"
    echo "Using LUAFAN_LUA_BIN=$LUA_CMD"
elif command -v luajit >/dev/null 2>&1; then
    LUA_CMD="luajit"
    echo "Using LuaJIT"
elif command -v lua >/dev/null 2>&1; then
    LUA_CMD="lua"
    echo "Using Lua"
else
    echo -e "${RED}Error: Neither lua nor luajit found in PATH${NC}"
    exit 1
fi

# Run from tests/ directory (lua test files use relative paths)
cd "$SCRIPT_DIR"

# Set up Lua path to include project modules and test framework
export LUA_PATH="$PROJECT_ROOT/modules/?.lua;$PROJECT_ROOT/modules/?/init.lua;$SCRIPT_DIR/lua/framework/?.lua;$SCRIPT_DIR/lua/?.lua;;"
export LUA_CPATH="$SCRIPT_DIR/build/?.so;$PROJECT_ROOT/?.so;;"

# Check if LuaFan is available and report the lock mode of this run
echo "Checking LuaFan availability..."
if ! $LUA_CMD -e "require('fan')" >/dev/null 2>&1; then
    echo -e "${YELLOW}Warning: LuaFan module not available. Some tests may be skipped.${NC}"
fi

LOCK_MODE=$($LUA_CMD -e "local ok, fan = pcall(require, 'fan'); if ok and fan.diag_lock_mode then io.write(fan.diag_lock_mode()) else io.write('unavailable') end" 2>/dev/null || echo "unavailable")
echo "Lua lock mode: $LOCK_MODE (core-hook = hooked interpreter, wrapper = stock interpreter + luafan guard)"

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
    if timeout 120s $LUA_CMD "$ALL_LUA_TESTS_SCRIPT"; then
        echo -e "${GREEN}✓ All Lua tests completed successfully${NC}"
        TOTAL_FAILURES=0
        TESTS_RUN=1
    else
        exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "${RED}✗ Lua tests timed out (120s limit)${NC}"
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

# Worker/lock granularity suite, run as its own process -- the same step CI runs.
# It is deliberately NOT in the curated list inside run_all_lua_tests.lua: that
# runner executes each test file inside pcall() inside fan.loop(), and a test that
# drives its own file-scope fan.loop() ends that nested loop with fan.loopbreak()/
# os.exit(), which also ends the runner's loop and silently truncates the run.
# Needs a lock build that carries the diagnostics (LUAFAN_TESTING=ON)
# and >= 4 event workers; it exits 77 (SKIP) otherwise, so on a default dev build
# this step is just a skip line.
LOCK_TEST="$SCRIPT_DIR/lua/test_lock_granularity.lua"
if [ -f "$LOCK_TEST" ]; then
    echo
    echo -e "${YELLOW}Running worker lock granularity tests...${NC}"
    if timeout 180s $LUA_CMD "$LOCK_TEST"; then
        echo -e "${GREEN}✓ Lock granularity test passed${NC}"
        TESTS_RUN=$((TESTS_RUN + 1))
    else
        lock_exit=$?
        if [ "$lock_exit" -eq 77 ]; then
            echo -e "${YELLOW}⊝ Lock granularity test skipped (needs LUAFAN_TESTING build and >= 4 workers)${NC}"
        else
            if [ "$lock_exit" -eq 124 ]; then
                echo -e "${RED}✗ Lock granularity test timed out${NC}"
            else
                echo -e "${RED}✗ Lock granularity test failed${NC}"
            fi
            TESTS_RUN=$((TESTS_RUN + 1))
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        fi
    fi
fi

# Suites that need their own process (and therefore their own event loop), because
# they cannot work inside the curated runner above:
#   (a) they start a file-scope fan.loop() and end it with fan.loopbreak()/os.exit();
#       a break inside such a nested loop also ends the runner's loop, which
#       silently truncates the run (no summary, exit code still 0);
#   (b) they drive the loop themselves with a bare fan.loop(), which makes no
#       progress nested inside the runner's loop (their helpers return nil), even
#       though they pass in a fresh process.
# Each one runs here as its own process, in the same lock shape as the rest of the
# suite -- same treatment as the lock granularity step above. Keep this list in
# sync with the note in lua/run_all_lua_tests.lua.
STANDALONE_TESTS="
test_tcpd_concurrent_lifecycle.lua
test_udpd_event_lifecycle.lua
test_udpd_send_ready_race.lua
test_httpd_websocket_lifecycle.lua
test_mariadb_pending_event.lua
test_mariadb_workers.lua
test_evdns_integration.lua
test_luafan_mainevent_lifetime.lua
test_httpd_lifecycle_regressions.lua
test_httpd_rfc_regressions.lua
test_http_client.lua
test_httpd_compliance.lua
test_httpd_security.lua
test_httpd_performance.lua
"
for standalone_name in $STANDALONE_TESTS; do
    standalone_path="$SCRIPT_DIR/lua/$standalone_name"
    if [ ! -f "$standalone_path" ]; then
        echo -e "${YELLOW}⊝ $standalone_name not found - skipped${NC}"
        continue
    fi

    echo
    echo -e "${YELLOW}Running $standalone_name (standalone)...${NC}"
    if timeout 180s $LUA_CMD "$standalone_path"; then
        echo -e "${GREEN}✓ $standalone_name passed${NC}"
        TESTS_RUN=$((TESTS_RUN + 1))
    else
        standalone_exit=$?
        if [ "$standalone_exit" -eq 124 ]; then
            echo -e "${RED}✗ $standalone_name timed out${NC}"
        elif [ "$standalone_exit" -eq 77 ]; then
            echo -e "${YELLOW}⊝ $standalone_name skipped${NC}"
            TESTS_RUN=$((TESTS_RUN + 1))
            continue
        else
            echo -e "${RED}✗ $standalone_name failed (exit $standalone_exit)${NC}"
        fi
        TESTS_RUN=$((TESTS_RUN + 1))
        TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
    fi
done

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