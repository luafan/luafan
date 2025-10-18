#!/usr/bin/env lua
-- Comprehensive HTTPD Test Runner
-- Runs all HTTP server tests and generates a report

local fan = require "fan"

-- Import test modules
local compliance_tests = require "tests.lua.test_httpd_compliance"
local security_tests = require "tests.lua.test_httpd_security"
local performance_tests = require "tests.lua.test_httpd_performance"

-- Test configuration
local TEST_SUITES = {
    {
        name = "HTTP Protocol Compliance",
        module = compliance_tests,
        runner = "run_all_tests",
        description = "RFC 7230/7231 compliance and basic functionality"
    },
    {
        name = "Security Tests",
        module = security_tests,
        runner = "run_security_tests",
        description = "Security features and vulnerability resistance"
    },
    {
        name = "Performance Tests",
        module = performance_tests,
        runner = "run_performance_tests",
        description = "Performance benchmarks and load testing"
    }
}

-- Global test results
local overall_results = {
    suites_run = 0,
    suites_passed = 0,
    total_tests = 0,
    total_passed = 0,
    start_time = os.time(),
    end_time = nil,
    suite_results = {}
}

-- Print test header
local function print_header()
    print("╔" .. string.rep("═", 60) .. "╗")
    print("║" .. string.format("%s", " LuaFan HTTPD Comprehensive Test Suite"):sub(1,60) .. string.rep(" ", math.max(0, 60 - #" LuaFan HTTPD Comprehensive Test Suite")) .. "║")
    print("║" .. string.rep(" ", 60) .. "║")
    print("║  Testing all aspects of the HTTP server implementation  ║")
    print("║  • Protocol Compliance (RFC 7230/7231)                  ║")
    print("║  • Security Features & Vulnerability Tests              ║")
    print("║  • Performance Benchmarks & Load Testing               ║")
    print("╚" .. string.rep("═", 60) .. "╝")
    print()
end

-- Print test suite header
local function print_suite_header(suite_info)
    print("┌" .. string.rep("─", 58) .. "┐")
    print("│ " .. suite_info.name .. string.rep(" ", 57 - #suite_info.name) .. "│")
    print("│ " .. suite_info.description .. string.rep(" ", 57 - #suite_info.description) .. "│")
    print("└" .. string.rep("─", 58) .. "┘")
end

-- Run a single test suite
local function run_test_suite(suite_info)
    print_suite_header(suite_info)

    local suite_start_time = os.time()
    local success = false
    local error_message = nil

    -- Attempt to run the test suite
    local pcall_success, result = pcall(function()
        return suite_info.module[suite_info.runner]()
    end)

    if pcall_success then
        success = result
    else
        error_message = result
        print("❌ Test suite crashed:", error_message)
    end

    local suite_end_time = os.time()
    local duration = suite_end_time - suite_start_time

    -- Record results
    local suite_result = {
        name = suite_info.name,
        success = success,
        duration = duration,
        error = error_message
    }

    table.insert(overall_results.suite_results, suite_result)
    overall_results.suites_run = overall_results.suites_run + 1

    if success then
        overall_results.suites_passed = overall_results.suites_passed + 1
        print(string.format("✅ %s completed successfully in %ds", suite_info.name, duration))
    else
        print(string.format("❌ %s failed after %ds", suite_info.name, duration))
    end

    print() -- Empty line for spacing
    return success
end

-- Generate comprehensive test report
local function generate_report()
    overall_results.end_time = os.time()
    local total_duration = overall_results.end_time - overall_results.start_time

    print("╔" .. string.rep("═", 60) .. "╗")
    print("║" .. string.format("%s", " COMPREHENSIVE TEST REPORT"):sub(1,60) .. string.rep(" ", math.max(0, 60 - #" COMPREHENSIVE TEST REPORT")) .. "║")
    print("╠" .. string.rep("═", 60) .. "╣")

    -- Overall statistics
    print(string.format("║ Test Suites: %d/%d passed (%.1f%%)%s║",
          overall_results.suites_passed,
          overall_results.suites_run,
          (overall_results.suites_passed / overall_results.suites_run) * 100,
          string.rep(" ", 60 - 35)))

    print(string.format("║ Total Duration: %d seconds%s║",
          total_duration,
          string.rep(" ", 60 - 25)))

    print("╠" .. string.rep("─", 60) .. "╣")

    -- Individual suite results
    for _, suite_result in ipairs(overall_results.suite_results) do
        local status_icon = suite_result.success and "✅" or "❌"
        local status_text = suite_result.success and "PASS" or "FAIL"

        print(string.format("║ %s %-40s %s (%ds) ║",
              status_icon,
              suite_result.name,
              status_text,
              suite_result.duration))

        if not suite_result.success and suite_result.error then
            print(string.format("║   Error: %-50s ║", suite_result.error:sub(1, 50)))
        end
    end

    print("╠" .. string.rep("═", 60) .. "╣")

    -- Final verdict
    if overall_results.suites_passed == overall_results.suites_run then
        print("║ 🎉 ALL TESTS PASSED! HTTPD is ready for production.     ║")
        print("║                                                          ║")
        print("║ ✓ HTTP Protocol Compliance: RFC 7230/7231 compliant     ║")
        print("║ ✓ Security: Hardened against common vulnerabilities     ║")
        print("║ ✓ Performance: Meets performance benchmarks             ║")
    else
        print("║ ⚠️  SOME TESTS FAILED! Review issues before deployment.  ║")
        print("║                                                          ║")

        -- Show which areas need attention
        for _, suite_result in ipairs(overall_results.suite_results) do
            if not suite_result.success then
                if suite_result.name:find("Compliance") then
                    print("║ ❌ Protocol issues may cause client compatibility       ║")
                elseif suite_result.name:find("Security") then
                    print("║ ❌ Security vulnerabilities present - high priority     ║")
                elseif suite_result.name:find("Performance") then
                    print("║ ❌ Performance issues may affect scalability            ║")
                end
            end
        end
    end

    print("╚" .. string.rep("═", 60) .. "╝")

    return overall_results.suites_passed == overall_results.suites_run
end

-- Main test runner
local function run_all_tests()
    print_header()

    -- Run each test suite
    for _, suite_info in ipairs(TEST_SUITES) do
        run_test_suite(suite_info)

        -- Give some time between test suites to avoid port conflicts
        fan.sleep(0.5)
    end

    -- Generate and display final report
    local all_passed = generate_report()

    -- Return exit code
    return all_passed and 0 or 1
end

-- Command line interface
local function main(args)
    local exit_code = 0

    if args and args[1] == "--help" then
        print("LuaFan HTTPD Test Runner")
        print()
        print("Usage: lua run_httpd_tests.lua [options]")
        print()
        print("Options:")
        print("  --help          Show this help message")
        print("  --compliance    Run only compliance tests")
        print("  --security      Run only security tests")
        print("  --performance   Run only performance tests")
        print()
        print("By default, all test suites are run.")
        return 0
    end

    -- Handle specific test suite requests
    if args and args[1] then
        local suite_name = args[1]:gsub("^%-%-", "")
        local found_suite = false

        for _, suite_info in ipairs(TEST_SUITES) do
            if suite_info.name:lower():find(suite_name) then
                print_header()
                local success = run_test_suite(suite_info)
                exit_code = success and 0 or 1
                found_suite = true
                break
            end
        end

        if not found_suite then
            print("Error: Unknown test suite '" .. suite_name .. "'")
            print("Available suites: compliance, security, performance")
            return 1
        end
    else
        -- Run all tests
        exit_code = run_all_tests()
    end

    return exit_code
end

-- If run as script, execute main function
if arg then
    local exit_code = main(arg)
    os.exit(exit_code)
end

-- Export for use as module
return {
    run_all_tests = run_all_tests,
    run_test_suite = run_test_suite,
    generate_report = generate_report,
    main = main
}