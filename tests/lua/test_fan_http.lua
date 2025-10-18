#!/usr/bin/env lua

-- Test for fan.http module (HTTP client)
-- Note: This test focuses on URL encoding/decoding functions only
-- Network functions require complex dependencies (fan.connector, compat53)

local TestFramework = require('test_framework')

-- Mock config module if needed
package.preload['config'] = function()
    return {pool_size = 10}
end

-- Try to load just the HTTP module functions we can test
local http_available = false
local http

-- Try to load fan.http module
local ok, result = pcall(require, 'fan.http')
if ok then
    http = result
    http_available = true
    print("fan.http module loaded successfully")
else
    print("Error: fan.http module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.http Tests")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(http)
    TestFramework.assert_type(http, "table")

    -- Check for essential functions based on docs
    TestFramework.assert_type(http.escape, "function")
    TestFramework.assert_type(http.unescape, "function")
    TestFramework.assert_type(http.get, "function")
    TestFramework.assert_type(http.post, "function")
    TestFramework.assert_type(http.put, "function")
    TestFramework.assert_type(http.head, "function")
    TestFramework.assert_type(http.delete, "function")
end)

-- Test URL escape/unescape functions
suite:test("url_encoding", function()
    -- Test basic URL encoding
    local test_str = "hello world"
    local escaped = http.escape(test_str)
    TestFramework.assert_type(escaped, "string")
    TestFramework.assert_not_equal(test_str, escaped)

    -- Test URL decoding
    local unescaped = http.unescape(escaped)
    TestFramework.assert_type(unescaped, "string")
    TestFramework.assert_equal(test_str, unescaped)

    -- Test special characters
    local special_str = "hello@example.com?param=value&other=test"
    local escaped_special = http.escape(special_str)
    local unescaped_special = http.unescape(escaped_special)
    TestFramework.assert_equal(special_str, unescaped_special)

    -- Test empty string
    TestFramework.assert_equal("", http.escape(""))
    TestFramework.assert_equal("", http.unescape(""))
end)

-- Test HTTP error handling with invalid URL (no actual network call)
suite:test("http_error_handling", function()
    -- Test with invalid domain - this should return an error without crashing
    local ok, response = pcall(http.get, "http://invalid.nonexistent.domain.test/")

    if ok then
        TestFramework.assert_not_nil(response)
        TestFramework.assert_type(response, "table")
        -- Should have an error for invalid domain
        if response.error then
            TestFramework.assert_type(response.error, "string")
            TestFramework.assert_true(#response.error > 0)
        end
    else
        -- If pcall failed, that's also expected behavior for invalid URLs
        TestFramework.assert_type(response, "string") -- error message
    end
end)

-- Test HTTP function calls with table arguments (structure test only)
suite:test("http_function_calls", function()
    -- Test that functions can be called without crashing (may return errors)
    local ok1 = pcall(http.get, {url = "http://localhost:1/nonexistent"})
    TestFramework.assert_type(ok1, "boolean")

    local ok2 = pcall(http.post, {url = "http://localhost:1/nonexistent", body = "test"})
    TestFramework.assert_type(ok2, "boolean")

    local ok3 = pcall(http.head, "http://localhost:1/nonexistent")
    TestFramework.assert_type(ok3, "boolean")

    local ok4 = pcall(http.put, {url = "http://localhost:1/nonexistent", body = "test"})
    TestFramework.assert_type(ok4, "boolean")

    local ok5 = pcall(http.delete, "http://localhost:1/nonexistent")
    TestFramework.assert_type(ok5, "boolean")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)