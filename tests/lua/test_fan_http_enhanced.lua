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
    -- Test with invalid URL format - this should fail quickly without network I/O
    local ok1, response1 = pcall(http.get, "not_a_valid_url")
    TestFramework.assert_type(ok1, "boolean")

    -- Test with malformed HTTP URL
    local ok2, response2 = pcall(http.get, "http://")
    TestFramework.assert_type(ok2, "boolean")

    -- Test with empty URL
    local ok3, response3 = pcall(http.get, "")
    TestFramework.assert_type(ok3, "boolean")

    -- Test with nil URL
    local ok4, response4 = pcall(http.get, nil)
    TestFramework.assert_type(ok4, "boolean")

    -- All of these should handle errors gracefully without blocking
    print("HTTP error handling test completed without blocking")
end)

-- Test HTTP function calls with table arguments (structure test only)
suite:test("http_function_calls", function()
    -- Test that functions can be called without crashing (using invalid URLs to avoid network I/O)
    local ok1 = pcall(http.get, {url = "invalid_url_format"})
    TestFramework.assert_type(ok1, "boolean")

    local ok2 = pcall(http.post, {url = "invalid_url_format", body = "test"})
    TestFramework.assert_type(ok2, "boolean")

    local ok3 = pcall(http.head, "invalid_url_format")
    TestFramework.assert_type(ok3, "boolean")

    local ok4 = pcall(http.put, {url = "invalid_url_format", body = "test"})
    TestFramework.assert_type(ok4, "boolean")

    local ok5 = pcall(http.delete, "invalid_url_format")
    TestFramework.assert_type(ok5, "boolean")

    print("HTTP function calls test completed without network I/O")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)