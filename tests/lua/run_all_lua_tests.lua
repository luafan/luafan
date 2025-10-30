#!/usr/bin/env lua

-- LuaFan All Lua Tests Runner
-- Single entry point to run all Lua test cases at once
-- Now supports parameterized execution

-- Set up LUA_PATH and LUA_CPATH for proper module loading
package.path = "../modules/?.lua;../modules/?/init.lua;./lua/framework/?.lua;;" .. package.path
package.cpath = "../?.so;;" .. package.cpath

local TestFramework = require('test_framework')

-- Mock config module if needed
package.preload['config'] = function()
    return {pool_size = 10, debug = false}
end

-- Command line argument parsing
local function parse_arguments(args)
    local config = {
        show_help = false,
        list_tests = false,
        test_file = nil,
        pattern = nil,
        exclude = nil,
        run_all = true
    }

    local i = 1
    while i <= #args do
        local arg = args[i]

        if arg == "--help" or arg == "-h" then
            config.show_help = true
            break
        elseif arg == "--list" then
            config.list_tests = true
        elseif arg == "--file" then
            i = i + 1
            if i <= #args then
                config.test_file = args[i]
                config.run_all = false
            else
                error("--file requires a filename argument")
            end
        elseif arg == "--pattern" then
            i = i + 1
            if i <= #args then
                config.pattern = args[i]
                config.run_all = false
            else
                error("--pattern requires a pattern argument")
            end
        elseif arg == "--exclude" then
            i = i + 1
            if i <= #args then
                config.exclude = args[i]
            else
                error("--exclude requires a pattern argument")
            end
        else
            -- Unknown argument, ignore or could add error handling
            print(string.format("Warning: Unknown argument '%s'", arg))
        end

        i = i + 1
    end

    return config
end

-- Function to show usage help
local function show_help()
    print("LuaFan All Lua Tests Runner")
    print("===========================")
    print("")
    print("Usage: lua run_all_lua_tests.lua [OPTIONS]")
    print("")
    print("OPTIONS:")
    print("  --help, -h         Show this help message")
    print("  --list             List all available test files")
    print("  --file <filename>  Run a specific test file")
    print("  --pattern <pattern> Run test files matching pattern")
    print("  --exclude <pattern> Exclude test files matching pattern")
    print("")
    print("Examples:")
    print("  lua run_all_lua_tests.lua                    # Run all tests")
    print("  lua run_all_lua_tests.lua --list             # List all test files")
    print("  lua run_all_lua_tests.lua --file test_fan_core.lua")
    print("  lua run_all_lua_tests.lua --pattern 'test_mariadb_.*'")
    print("  lua run_all_lua_tests.lua --exclude 'test_.*_orm.lua'")
    print("  lua run_all_lua_tests.lua --pattern 'test_fan_.*' --exclude 'test_fan_httpd.*'")
    print("")
end

-- Parse command line arguments
local config = parse_arguments(arg or {})

if config.show_help then
    show_help()
    os.exit(0)
end

print("LuaFan All Lua Tests Runner")
print("===========================")

-- Function to check if a test file exists
local function file_exists(filename)
    local file = io.open(filename, "r")
    if file then
        file:close()
        return true
    end
    return false
end

-- List of all test files to run (in order)
local test_files = {
    -- "test_framework.lua",  -- Skip framework test
    "test_fan_core.lua",
    "test_fan_utils.lua",
    "test_fan_objectbuf.lua",
    "test_fan_pool.lua",
    "test_fan_worker.lua",
    "test_fan_upnp.lua",
    "test_fan_httpd_core.lua",
    -- "test_fan_httpd.lua",
    -- "test_fan_httpd_loop.lua",
    "test_fan_httpd_lua.lua",
    "test_fan_http_enhanced.lua",
    "test_fan_stream.lua",
    "test_fan_connector.lua",
    "test_mariadb_orm.lua",
    "test_mariadb_basic.lua",     -- Basic consolidated MariaDB core tests
    "test_mariadb_phase1.lua",    -- Phase 1: Connection and basic table operations
    "test_mariadb_phase2a.lua",   -- Phase 2A: Data types testing
    "test_mariadb_phase2b.lua",   -- Phase 2B: Basic CRUD operations
    "test_mariadb_phase2c.lua",   -- Phase 2C: Constraints and indexes
    "test_mariadb_real.lua",      -- Full integration tests
    "test_mariadb_comprehensive.lua",   -- Comprehensive advanced features testing
    "test_mariadb_simple_debug.lua",
    "test_sqlite3_orm.lua",
    "test_integration_http_server.lua",
    "test_tcpd_callback_self_first.lua",
    "test_udpd_callback_self_first.lua",
    "test_udpd_dest_getip.lua",
    "test_fan_evdns.lua",
    "test_evdns_integration.lua",
    "test_memory_leak_fix.lua",
    "test_tcpd_memory_leak_fix.lua",
    -- Add more test files here as they are completed
}

-- Function to filter test files based on config
local function filter_test_files(files, config)
    local filtered = {}

    -- If specific file is requested, just return it without checking if it's in the list
    if config.test_file then
        return {config.test_file}
    end

    for _, filename in ipairs(files) do
        local include = true

        -- Check pattern matching
        if config.pattern then
            include = string.match(filename, config.pattern) ~= nil
        end

        -- Check exclude pattern
        if include and config.exclude then
            include = string.match(filename, config.exclude) == nil
        end

        if include then
            table.insert(filtered, filename)
        end
    end

    return filtered
end

-- Function to list all available test files
local function list_test_files()
    print("Available test files:")
    print("====================")

    for i, filename in ipairs(test_files) do
        local filepath = "lua/" .. filename
        local status = file_exists(filepath) and "✓" or "✗"
        print(string.format("%2d. %s %s", i, status, filename))
    end

    print("")
    print("Legend: ✓ = exists, ✗ = missing")
    print(string.format("Total: %d test files", #test_files))
    return
end

-- Apply configuration
if config.list_tests then
    list_test_files()
    os.exit(0)
end

-- Filter test files based on config
local filtered_test_files = filter_test_files(test_files, config)

if #filtered_test_files == 0 then
    print("No test files match the specified criteria.")
    if config.pattern then
        print(string.format("No files match pattern '%s'.", config.pattern))
    end
    os.exit(1)
end

-- Show what tests will be run
if not config.run_all then
    print(string.format("Running %d filtered test file(s):", #filtered_test_files))
    for _, filename in ipairs(filtered_test_files) do
        print(string.format("  - %s", filename))
    end
    print("")
end

-- Results tracking
local total_files = 0
local passed_files = 0
local failed_files = 0
local total_tests = 0
local total_passed = 0
local total_failed = 0

-- Function to run a single test file
local function run_test_file(filename)
    print(string.format("\n[Running %s]", filename))
    print(string.rep("=", 50))

    local filepath = "lua/" .. filename
    local chunk, err = loadfile(filepath)

    if not chunk then
        print(string.format("✗ Failed to load %s: %s", filename, err))
        failed_files = failed_files + 1
        return false
    end

    -- Capture the original exit function to prevent early exit
    local original_exit = os.exit
    local exit_code = 0
    local exit_called = false

    os.exit = function(code)
        exit_code = code or 0
        exit_called = true
        error("__TEST_EXIT__") -- Use error to break execution
    end

    -- Run the test file
    local success, result = pcall(chunk)

    -- Restore original exit function
    os.exit = original_exit

    if not success then
        if string.match(result, "__TEST_EXIT__") then
            -- Test completed normally with exit
            if exit_code == 0 then
                print(string.format("✓ %s passed", filename))
                passed_files = passed_files + 1
                return true
            else
                print(string.format("✗ %s failed (exit code: %d)", filename, exit_code))
                failed_files = failed_files + 1
                return false
            end
        else
            -- Test crashed with actual error
            print(string.format("✗ %s crashed: %s", filename, result))
            failed_files = failed_files + 1
            return false
        end
    else
        -- Test completed without calling exit (shouldn't happen normally)
        print(string.format("? %s completed without exit", filename))
        passed_files = passed_files + 1
        return true
    end
end

-- Load fan module for event loop
local fan = require('fan')

-- Main execution wrapped in fan.loop
fan.loop(function()
    local start_time = fan.gettime()

    -- Small delay to let event loop initialize
    fan.sleep(0.01)

    for _, filename in ipairs(filtered_test_files) do
        local filepath = "lua/" .. filename

        if file_exists(filepath) then
            total_files = total_files + 1
            run_test_file(filename)

            -- Small yield between test files
            fan.sleep(0.001)
        else
            print(string.format("⚠ Skipping %s (file not found)", filename))
        end
    end

    local end_time = fan.gettime()
    local total_time = end_time - start_time

    -- Print summary
    print("\n" .. string.rep("=", 60))
    print("FINAL TEST SUMMARY")
    print(string.rep("=", 60))
    print(string.format("Total test files:  %d", total_files))
    print(string.format("Passed files:      %d", passed_files))
    print(string.format("Failed files:      %d", failed_files))
    print(string.format("Success rate:      %.1f%%", total_files > 0 and (passed_files / total_files * 100) or 0))
    print(string.format("Total time:        %.3f seconds", total_time))
    print(string.rep("=", 60))

    if failed_files > 0 then
        print("OVERALL RESULT: FAILED")
        print(string.format("✗ %d test file(s) failed", failed_files))
        fan.loopbreak()
        os.exit(1)
    else
        print("OVERALL RESULT: PASSED")
        print(string.format("✓ All %d test file(s) passed", passed_files))
        fan.loopbreak()
        os.exit(0)
    end
end)