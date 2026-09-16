--[[
LuaFan Test Framework
Provides a simple but comprehensive testing framework for Lua modules
]]

local TestFramework = {}

-- Skip sentinel for distinguishing skips from failures
local SKIP_SENTINEL = "__TEST_SKIPPED__"

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

-- Skip a test with a reason
function TestFramework.skip_test(reason)
    error(SKIP_SENTINEL .. (reason or ""), 2)
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
        elseif type(err) == "string" and string.find(err, "__TEST_EXIT__", 1, true) then
            error(err, 0)
        elseif type(err) == "string" and string.find(err, SKIP_SENTINEL, 1, true) then
            local reason = string.sub(err, #SKIP_SENTINEL + 1)
            if #reason > 0 then
                print(string.format("SKIP: %s", reason))
            else
                print("SKIP")
            end
            TestResults.skipped_tests = TestResults.skipped_tests + 1
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
            if type(err) == "string" and string.find(err, "__TEST_EXIT__", 1, true) then
                error(err, 0)
            end
            print(string.format("Suite setup failed: %s", err))
            return 1
        end
    end

    -- Run all tests
    local suite_skipped = 0
    for _, test in ipairs(suite.tests) do
        local before_failed = TestResults.failed_tests
        local before_skipped = TestResults.skipped_tests
        run_test(suite, test)
        if TestResults.failed_tests > before_failed then
            suite_failed = suite_failed + 1
        elseif TestResults.skipped_tests > before_skipped then
            suite_skipped = suite_skipped + 1
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

    print(string.format("\nSuite '%s' completed: %d passed, %d failed, %d skipped (%.3fs)",
          suite.name, suite_passed, suite_failed, suite_skipped, suite_duration))

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

-- Shell command that runs `script_file` in a child process with the SAME Lua
-- interpreter and the SAME search paths as this process (stderr merged, hard
-- timeout). Crash/race tests used to hardcode `lua` plus literal
-- LUA_PATH/LUA_CPATH values, so a run driven by an out-of-tree build dir
-- spawned a child that could not load the same fan.so and died with
-- "module 'fan' not found" -- reported as a bogus test failure instead of the
-- crash/race the child was supposed to provoke. Callers append their own exit
-- sentinel, e.g. `TestFramework.child_lua_command(f, 15) .. "; echo $?"`.
function TestFramework.child_lua_command(script_file, timeout_secs)
    local timeout = timeout_secs or 60
    local function shq(s)
        return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
    end
    -- Prefer the paths this process was actually started with; fall back to the
    -- resolved search paths so a run without LUA_PATH/LUA_CPATH still works.
    local child_path = os.getenv("LUA_PATH")
    if not child_path or #child_path == 0 then child_path = package.path end
    local child_cpath = os.getenv("LUA_CPATH")
    if not child_cpath or #child_cpath == 0 then child_cpath = package.cpath end
    local interp = (arg and arg[-1]) or "lua"
    return "LUA_PATH=" .. shq(child_path)
        .. " LUA_CPATH=" .. shq(child_cpath)
        .. " timeout " .. tostring(timeout) .. "s "
        .. shq(interp) .. " " .. shq(script_file) .. " 2>&1"
end

-- Optional event-worker pool for suites that assert worker affinity.
-- MUST run before fan.loop(): event_mgr refuses workers_init() while the loop
-- is running. LUAN_TEST_WORKERS=N (N >= 1) enables the pool; unset/0 keeps the
-- suite single-threaded, in which case worker-only cases skip themselves.
function TestFramework.init_workers_from_env()
    local ok_fan, fan = pcall(require, 'fan')
    if not ok_fan or type(fan) ~= 'table' then
        return 0
    end
    local n = tonumber(os.getenv and os.getenv("LUAN_TEST_WORKERS") or "") or 0
    if n <= 0 then
        return 0
    end
    local have = (fan.worker_count and fan.worker_count()) or 0
    if have < n and fan.workers_init then
        local ok, ret = pcall(fan.workers_init, n)
        if not ok or (ret ~= nil and ret ~= 0) then
            return have
        end
    end
    return (fan.worker_count and fan.worker_count()) or 0
end

return TestFramework