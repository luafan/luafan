#!/usr/bin/env lua
-- HTTP Security Test for LuaFan HTTPD
-- Tests security features and vulnerability resistance

local fan = require "fan"
local httpd = require "modules.fan.httpd.httpd"
local http = require "fan.http"

-- Test configuration
local TEST_HOST = "127.0.0.1"
local TEST_PORT = 9998
local test_results = {}
local test_count = 0
local passed_count = 0

-- Test utility functions
local function test_assert(condition, message)
    test_count = test_count + 1
    if condition then
        passed_count = passed_count + 1
        print(string.format("✓ PASS: %s", message))
        return true
    else
        print(string.format("✗ FAIL: %s", message))
        return false
    end
end

local function start_security_test_server()
    -- Configure with security features
    local config = require "config"
    config.enable_rate_limiting = true
    config.rate_limit_requests = 5  -- Low limit for testing
    config.rate_limit_window = 10   -- 10 second window
    config.max_content_length = 1024 -- 1KB limit for testing

    local server = httpd.bind({host = TEST_HOST, port = TEST_PORT})

    server:get("/", function(ctx)
        ctx:reply(200, "OK", "Test endpoint")
    end)

    server:post("/upload", function(ctx)
        ctx:reply(200, "OK", "Upload received: " .. #(ctx.body or ""))
    end)

    return server
end

-- Test invalid HTTP headers
local function test_malformed_headers()
    print("\n--- Testing Malformed Headers Handling ---")

    -- Test with various malformed headers using raw socket
    local tcpd = require "fan.tcpd"
    local conn = tcpd.connect({host = TEST_HOST, port = TEST_PORT})

    if conn then
        -- Send request with invalid header (missing colon)
        local malformed_request = "GET / HTTP/1.1\r\n" ..
                                 "Host: " .. TEST_HOST .. "\r\n" ..
                                 "Invalid-Header-No-Colon\r\n" ..
                                 "\r\n"

        conn:send(malformed_request)
        local response, err = conn:receive()

        if response then
            local response_str = response:GetBytes()
            -- Should still get a valid response (server handles invalid headers gracefully)
            test_assert(response_str:find("HTTP/1.1"), "Server handles malformed headers gracefully")
        end

        conn:close()
    end
end

-- Test oversized requests
local function test_oversized_requests()
    print("\n--- Testing Oversized Request Handling ---")

    -- Test oversized content
    local large_content = string.rep("X", 2048) -- 2KB, exceeds our 1KB test limit

    local response = http.post("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/upload", large_content)

    -- Should get 413 Content Too Large
    test_assert(response and response.status == 413, "Oversized content returns 413 status")
end

-- Test rate limiting
local function test_rate_limiting()
    print("\n--- Testing Rate Limiting ---")

    local responses = {}

    -- Make requests up to the limit
    for i = 1, 6 do -- One more than the limit
        local response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/")
        table.insert(responses, response)
        fan.sleep(0.1)
    end

    -- Check that early requests succeed
    test_assert(responses[1] and responses[1].status == 200, "First request within limit succeeds")
    test_assert(responses[5] and responses[5].status == 200, "Fifth request (at limit) succeeds")

    -- Check that request exceeding limit gets 429
    test_assert(responses[6] and responses[6].status == 429, "Request exceeding limit returns 429")
end

-- Test URI length limits
local function test_uri_length_limits()
    print("\n--- Testing URI Length Limits ---")

    -- Create a very long URI (over 2KB)
    local long_path = "/test/" .. string.rep("a", 2100)
    local response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. long_path)

    -- Should get 414 URI Too Long
    test_assert(response and response.status == 414, "Oversized URI returns 414 status")
end

-- Test HTTP version validation
local function test_http_version_validation()
    print("\n--- Testing HTTP Version Validation ---")

    local tcpd = require "fan.tcpd"
    local conn = tcpd.connect({host = TEST_HOST, port = TEST_PORT})

    if conn then
        -- Send request with unsupported HTTP version
        local invalid_version_request = "GET / HTTP/2.0\r\n" ..
                                       "Host: " .. TEST_HOST .. "\r\n" ..
                                       "\r\n"

        conn:send(invalid_version_request)
        local response, err = conn:receive()

        if response then
            local response_str = response:GetBytes()
            -- Should get 505 HTTP Version Not Supported
            test_assert(response_str:find("505"), "Unsupported HTTP version returns 505")
        end

        conn:close()
    end
end

-- Test security headers presence
local function test_security_headers_comprehensive()
    print("\n--- Testing Comprehensive Security Headers ---")

    local response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/")

    if response and response.headers then
        local security_headers = {
            ["x-content-type-options"] = "nosniff",
            ["x-frame-options"] = "DENY",
            ["x-xss-protection"] = "1; mode=block"
        }

        for header, expected_value in pairs(security_headers) do
            local actual_value = response.headers[header]
            test_assert(actual_value == expected_value,
                       string.format("Security header %s has correct value", header))
        end
    end
end

-- Test error message sanitization
local function test_error_message_sanitization()
    print("\n--- Testing Error Message Sanitization ---")

    -- Test 404 error doesn't expose system information
    local response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/../../etc/passwd")

    test_assert(response and response.status == 404, "Path traversal attempt returns 404")

    if response and response.body then
        -- Error message should not contain system paths or internal details
        test_assert(not response.body:find("/etc/"), "Error message doesn't expose system paths")
        test_assert(not response.body:find("lua"), "Error message doesn't expose implementation details")
    end
end

-- Test HTTP method validation
local function test_http_method_validation()
    print("\n--- Testing HTTP Method Validation ---")

    local tcpd = require "fan.tcpd"
    local conn = tcpd.connect({host = TEST_HOST, port = TEST_PORT})

    if conn then
        -- Send request with invalid HTTP method
        local invalid_method_request = "INVALID / HTTP/1.1\r\n" ..
                                      "Host: " .. TEST_HOST .. "\r\n" ..
                                      "\r\n"

        conn:send(invalid_method_request)
        local response, err = conn:receive()

        if response then
            local response_str = response:GetBytes()
            -- Should get 400 Bad Request for invalid method
            test_assert(response_str:find("400"), "Invalid HTTP method returns 400")
        end

        conn:close()
    end
end

-- Main security test runner
local function run_security_tests()
    print("Starting LuaFan HTTPD Security Tests...")
    print("=" .. string.rep("=", 50))

    local server = start_security_test_server()

    -- Give server time to start
    fan.sleep(0.2)

    -- Run security test suites
    test_malformed_headers()
    test_oversized_requests()
    test_rate_limiting()
    test_uri_length_limits()
    test_http_version_validation()
    test_security_headers_comprehensive()
    test_error_message_sanitization()
    test_http_method_validation()

    -- Test summary
    print("\n" .. string.rep("=", 50))
    print(string.format("Security Test Results: %d/%d tests passed (%.1f%%)",
          passed_count, test_count, (passed_count / test_count) * 100))

    if passed_count == test_count then
        print("🔒 All security tests passed! HTTPD is secure.")
        return true
    else
        print("⚠️  Some security tests failed. Please review security implementation.")
        return false
    end
end

-- Main function
local function main()
    local success, result = pcall(run_security_tests)
    if not success then
        print("Security test execution failed:", result)
        return false
    end
    return result
end

-- Export for use in other test files
return {
    run_security_tests = run_security_tests,
    test_assert = test_assert,
    main = main
}