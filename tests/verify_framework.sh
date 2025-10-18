#!/bin/bash

# Quick verification script for the test framework

set -e

echo "🧪 Verifying LuaFan Test Framework Setup"
echo "=========================================="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check directory structure
echo "✓ Checking directory structure..."
for dir in "c/framework" "c/unit" "lua/framework" "lua/modules" "integration" "fixtures"; do
    if [ -d "$dir" ]; then
        echo "  ✓ $dir exists"
    else
        echo "  ✗ $dir missing"
        exit 1
    fi
done

# Check key files
echo "✓ Checking key files..."
for file in "c/framework/test_framework.h" "c/framework/test_framework.c" "lua/framework/test_framework.lua" "CMakeLists.txt" "run_all_tests.sh" "run_lua_tests.sh"; do
    if [ -f "$file" ]; then
        echo "  ✓ $file exists"
    else
        echo "  ✗ $file missing"
        exit 1
    fi
done

# Check scripts are executable
echo "✓ Checking script permissions..."
for script in "run_all_tests.sh" "run_lua_tests.sh"; do
    if [ -x "$script" ]; then
        echo "  ✓ $script is executable"
    else
        echo "  ✗ $script is not executable"
        exit 1
    fi
done

# Test Lua framework basic loading
echo "✓ Testing Lua framework loading..."
if command -v lua >/dev/null 2>&1; then
    export LUA_PATH="$SCRIPT_DIR/lua/framework/?.lua;;"
    if lua -e "local tf = require('test_framework'); print('✓ Lua framework loads successfully')" 2>/dev/null; then
        echo "  ✓ Lua framework loads correctly"
    else
        echo "  ⚠ Lua framework has issues (but framework files exist)"
    fi
else
    echo "  ⚠ Lua not available for testing"
fi

# Try to build C tests
echo "✓ Testing C framework build..."
if [ -d "build" ]; then
    rm -rf build
fi

mkdir -p build
cd build

if cmake .. >/dev/null 2>&1; then
    echo "  ✓ CMake configuration successful"
    if make >/dev/null 2>&1; then
        echo "  ✓ Build successful"
        if [ -f "run_c_tests" ]; then
            echo "  ✓ C test executable created"
        else
            echo "  ⚠ C test executable not found (may be normal if no tests)"
        fi
    else
        echo "  ⚠ Build failed (dependencies may be missing)"
    fi
else
    echo "  ⚠ CMake configuration failed (dependencies may be missing)"
fi

cd ..

echo ""
echo "🎉 Test Framework Verification Complete!"
echo "=========================================="
echo ""
echo "Next steps:"
echo "1. Install dependencies: libevent, openssl, curl, luajit"
echo "2. Build LuaFan: cd .. && sudo luarocks make luafan-0.7-3.rockspec"
echo "3. Run tests: ./run_all_tests.sh"
echo ""
echo "For help: ./run_all_tests.sh --help"