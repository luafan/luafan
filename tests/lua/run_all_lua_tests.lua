#!/usr/bin/env lua

-- LuaFan All Lua Tests Runner
-- Single entry point to run all Lua test cases at once

local TestFramework = require('test_framework')

-- Mock config module if needed
package.preload['config'] = function()
    return {pool_size = 10, debug = false}
end

print("LuaFan All Lua Tests Runner")
print("===========================")

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

-- Function to check if a test file exists
local function file_exists(filename)
    local file = io.open(filename, "r")
    if file then
        file:close()
        return true
    end
    return false
end

-- Load fan module for event loop
local fan = require('fan')

-- Main execution wrapped in fan.loop
fan.loop(function()
    local start_time = fan.gettime()

    -- Small delay to let event loop initialize
    fan.sleep(0.01)

    for _, filename in ipairs(test_files) do
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