#!/bin/bash

# LuaFan All Lua Tests Runner Script
# Runs all Lua tests through a single entry point

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}LuaFan All Lua Tests Runner${NC}"
echo "===================================="

# Check if we're in the tests directory
if [[ ! -f "lua/run_all_lua_tests.lua" ]]; then
    echo -e "${RED}Error: Must run from tests directory${NC}"
    echo "Usage: cd /path/to/luafan/tests && ./run_all_lua_tests.sh"
    exit 1
fi

# Check if fan.so exists
if [[ ! -f "../fan.so" ]]; then
    echo -e "${YELLOW}Warning: fan.so not found, building...${NC}"
    cd ..
    cmake . && make
    cd tests
fi

echo "Setting up Lua environment..."
export LUA_PATH="../modules/?.lua;../modules/?/init.lua;./lua/framework/?.lua;;"
export LUA_CPATH="../?.so;;"

echo "Starting test execution..."
echo ""

# Run the main Lua test runner
lua lua/run_all_lua_tests.lua

exit_code=$?

if [ $exit_code -eq 0 ]; then
    echo -e "${GREEN}All Lua tests completed successfully!${NC}"
else
    echo -e "${RED}Some Lua tests failed.${NC}"
fi

exit $exit_code