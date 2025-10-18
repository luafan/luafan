--[[
LuaFan Test Framework
Provides a simple but comprehensive testing framework for Lua modules
]]

local TestFramework = {}

-- Test result tracking
local TestResults = {
    total_tests = 0,
    passed_tests = 0,
    failed_tests = 0,
    skipped_tests = 0,
    total_time = 0,
    current_suite = nil,
    current_test = nil,
    test_failed = false
}

-- Assertion functions
local function assert_equal(actual, expected, message)
    message = message or string.format("Expected %s, got %s", tostring(expected), tostring(actual))
    if actual ~= expected then
        error(message, 2)
    end
end

local function assert_not_equal(actual, expected, message)
    message = message or string.format("Expected not %s, got %s", tostring(expected), tostring(actual))
    if actual == expected then
        error(message, 2)
    end
end

local function assert_true(value, message)
    message = message or string.format("Expected true, got %s", tostring(value))
    if not value then
        error(message, 2)
    end
end

local function assert_false(value, message)
    message = message or string.format("Expected false, got %s", tostring(value))
    if value then
        error(message, 2)
    end
end

local function assert_nil(value, message)
    message = message or string.format("Expected nil, got %s", tostring(value))
    if value ~= nil then
        error(message, 2)
    end
end

local function assert_not_nil(value, message)
    message = message or "Expected non-nil value"
    if value == nil then
        error(message, 2)
    end
end

local function assert_type(value, expected_type, message)
    local actual_type = type(value)
    message = message or string.format("Expected type %s, got %s", expected_type, actual_type)
    if actual_type ~= expected_type then
        error(message, 2)
    end
end

local function assert_match(string, pattern, message)
    message = message or string.format("String '%s' does not match pattern '%s'", string, pattern)
    if not string.match(string, pattern) then
        error(message, 2)
    end
end

local function assert_error(func, expected_error, message)
    message = message or "Expected function to throw an error"
    local ok, err = pcall(func)
    if ok then
        error(message, 2)
    end
    if expected_error and not string.match(err, expected_error) then
        error(string.format("Expected error matching '%s', got '%s'", expected_error, err), 2)
    end
end

-- Time utilities
local function get_time()
    if fan and fan.gettime then
        return fan.gettime()
    elseif os.clock then
        return os.clock()
    else
        return os.time()
    end
end

-- Test suite creation
function TestFramework.create_suite(name)
    local suite = {
        name = name,
        tests = {},
        setup = nil,
        teardown = nil,
        before_each = nil,
        after_each = nil
    }

    -- Add test to suite
    function suite:test(test_name, test_func)
        table.insert(self.tests, {
            name = test_name,
            func = test_func
        })
        return self
    end

    -- Set setup function (runs once before all tests)
    function suite:set_setup(func)
        self.setup = func
        return self
    end

    -- Set teardown function (runs once after all tests)
    function suite:set_teardown(func)
        self.teardown = func
        return self
    end

    -- Set before_each function (runs before each test)
    function suite:set_before_each(func)
        self.before_each = func
        return self
    end

    -- Set after_each function (runs after each test)
    function suite:set_after_each(func)
        self.after_each = func
        return self
    end

    return suite
end

-- Global test execution state for fan.loop mode
local TestExecution = {
    tests_queue = {},
    current_test_index = 1,
    results = {},
    suite = nil,
    running_in_loop = false
}

-- Execute a single test within the global event loop
local function execute_test_in_loop(suite, test)
    local start_time = get_time()

    -- Run before_each if provided
    if suite.before_each then
        local ok, err = pcall(suite.before_each)
        if not ok then
            return {
                name = test.name,
                ok = false,
                error = "before_each error: " .. tostring(err),
                duration = get_time() - start_time
            }
        end
    end

    -- Run the test with pcall protection
    local test_ok, test_err = pcall(test.func)

    -- Run after_each if provided
    if suite.after_each then
        local after_ok, after_err = pcall(suite.after_each)
        if not after_ok and test_ok then -- Only report after_each error if test passed
            test_ok = false
            test_err = "after_each error: " .. tostring(after_err)
        end
    end

    local end_time = get_time()
    return {
        name = test.name,
        ok = test_ok,
        error = test_err,
        duration = end_time - start_time
    }
end

-- Process next test in the queue (called within fan.loop)
local function process_next_test()
    if TestExecution.current_test_index > #TestExecution.tests_queue then
        -- All tests completed, break the loop
        if _G.fan and _G.fan.loopbreak then
            _G.fan.loopbreak()
        end
        return
    end

    local test_info = TestExecution.tests_queue[TestExecution.current_test_index]
    local result = execute_test_in_loop(test_info.suite, test_info.test)

    -- Store result
    TestExecution.results[TestExecution.current_test_index] = result

    -- Move to next test
    TestExecution.current_test_index = TestExecution.current_test_index + 1

    -- Schedule next test execution (yield to event loop)
    if _G.fan and _G.fan.sleep then
        _G.fan.sleep(0.001) -- Tiny delay to yield control
    end

    -- Continue with next test
    process_next_test()
end

-- Run a single test (entry point)
local function run_test(suite, test)
    TestResults.current_test = test.name
    print(string.format("  Running test: %s ... ", test.name))
    io.flush()

    if not TestExecution.running_in_loop then
        -- Traditional synchronous mode
        local start_time = get_time()

        -- Run before_each if provided
        if suite.before_each then
            local ok, err = pcall(suite.before_each)
            if not ok then
                print(string.format("FAIL (before_each error: %s)", err))
                TestResults.failed_tests = TestResults.failed_tests + 1
                TestResults.total_tests = TestResults.total_tests + 1
                return
            end
        end

        -- Run the test
        local ok, err = pcall(test.func)
        local end_time = get_time()
        local duration = end_time - start_time

        -- Run after_each if provided
        if suite.after_each then
            local after_ok, after_err = pcall(suite.after_each)
            if not after_ok then
                print(string.format("FAIL (after_each error: %s)", after_err))
                TestResults.failed_tests = TestResults.failed_tests + 1
                TestResults.total_tests = TestResults.total_tests + 1
                return
            end
        end

        -- Report result
        if ok then
            print(string.format("PASS (%.3fs)", duration))
            TestResults.passed_tests = TestResults.passed_tests + 1
        else
            print(string.format("FAIL (%.3fs): %s", duration, err))
            TestResults.failed_tests = TestResults.failed_tests + 1
        end

        TestResults.total_tests = TestResults.total_tests + 1
    else
        -- In fan.loop mode, results are handled after all tests complete
        -- Just increment total for now
        TestResults.total_tests = TestResults.total_tests + 1
    end
end

-- Run a test suite
function TestFramework.run_suite(suite)
    if not suite or not suite.tests then
        error("Invalid test suite")
    end

    print(string.format("\n=== Running Test Suite: %s ===", suite.name))
    TestResults.current_suite = suite.name

    local suite_start_time = get_time()
    local suite_passed = 0
    local suite_failed = 0

    -- Run setup if provided
    if suite.setup then
        local ok, err = pcall(suite.setup)
        if not ok then
            print(string.format("Suite setup failed: %s", err))
            return 1
        end
    end

    -- Run all tests
    for _, test in ipairs(suite.tests) do
        local before_failed = TestResults.failed_tests
        run_test(suite, test)
        if TestResults.failed_tests > before_failed then
            suite_failed = suite_failed + 1
        else
            suite_passed = suite_passed + 1
        end
    end

    -- Run teardown if provided
    if suite.teardown then
        local ok, err = pcall(suite.teardown)
        if not ok then
            print(string.format("Suite teardown failed: %s", err))
        end
    end

    local suite_end_time = get_time()
    local suite_duration = suite_end_time - suite_start_time

    print(string.format("\nSuite '%s' completed: %d passed, %d failed (%.3fs)",
          suite.name, suite_passed, suite_failed, suite_duration))

    TestResults.total_time = TestResults.total_time + suite_duration

    return suite_failed
end

-- Run multiple test suites
function TestFramework.run_all_tests(suites)
    print(string.format("Starting test run with %d suites...", #suites))

    -- Reset results
    TestResults.total_tests = 0
    TestResults.passed_tests = 0
    TestResults.failed_tests = 0
    TestResults.skipped_tests = 0
    TestResults.total_time = 0

    local start_time = get_time()
    local total_failures = 0

    for _, suite in ipairs(suites) do
        local failures = TestFramework.run_suite(suite)
        if failures > 0 then
            total_failures = total_failures + failures
        end
    end

    local end_time = get_time()
    TestResults.total_time = end_time - start_time

    TestFramework.print_results()

    return total_failures
end

-- Print test results
function TestFramework.print_results()
    print("\n" .. string.rep("=", 60))
    print("TEST RESULTS SUMMARY")
    print(string.rep("=", 60))
    print(string.format("Total Tests:   %d", TestResults.total_tests))
    print(string.format("Passed:        %d", TestResults.passed_tests))
    print(string.format("Failed:        %d", TestResults.failed_tests))
    print(string.format("Skipped:       %d", TestResults.skipped_tests))

    local success_rate = TestResults.total_tests > 0 and
                        (TestResults.passed_tests / TestResults.total_tests * 100) or 0
    print(string.format("Success Rate:  %.1f%%", success_rate))
    print(string.format("Total Time:    %.3f seconds", TestResults.total_time))
    print(string.rep("=", 60))

    if TestResults.failed_tests > 0 then
        print("RESULT: FAILED")
    else
        print("RESULT: PASSED")
    end
end

-- Simplified test wrapper for fan.loop mode
function TestFramework.async_test(test_func)
    -- Check if fan module is available
    local has_fan = _G.fan and type(_G.fan.loop) == "function" and type(_G.fan.loopbreak) == "function"

    if has_fan then
        -- Return a function that uses fan.loop with pcall protection
        return function()
            _G.fan.loop(function()
                local ok, err = pcall(test_func)
                _G.fan.loopbreak()
                if not ok then
                    error(err)
                end
            end)
        end
    else
        -- Return the original function for non-fan environments
        return test_func
    end
end

-- Export assertion functions
TestFramework.assert_equal = assert_equal
TestFramework.assert_not_equal = assert_not_equal
TestFramework.assert_true = assert_true
TestFramework.assert_false = assert_false
TestFramework.assert_nil = assert_nil
TestFramework.assert_not_nil = assert_not_nil
TestFramework.assert_type = assert_type
TestFramework.assert_match = assert_match
TestFramework.assert_error = assert_error

-- Export results for external access
TestFramework.results = TestResults

return TestFramework