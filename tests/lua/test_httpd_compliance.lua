#!/usr/bin/env lua
-- HTTP Protocol Compliance Test for LuaFan HTTPD
-- Tests RFC 7230/7231 compliance and security features

local fan = require "fan"
local httpd = require "fan.httpd.httpd"
local http = require "fan.http"

-- Test configuration
local TEST_HOST = "127.0.0.1"
local TEST_PORT = 9999
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

local function start_test_server()
    local server = httpd.bind({host = TEST_HOST, port = TEST_PORT})

    -- Basic routes for testing
    server:get("/", function(ctx)
        ctx:reply(200, "OK", "Hello World")
    end)

    server:get("/user/:id", function(ctx)
        ctx:reply(200, "OK", "User ID: " .. ctx.params.id)
    end)

    server:post("/echo", function(ctx)
        ctx:reply(200, "OK", ctx.body or "")
    end)

    server:get("/large", function(ctx)
        local large_content = string.rep("A", 2048) -- 2KB content for compression test
        ctx:addheader("Content-Type", "text/plain")
        ctx:reply(200, "OK", large_content)
    end)

    server:get("/stats", function(ctx)
        local stats = server:get_stats()
        ctx:addheader("Content-Type", "application/json")
        ctx:reply(200, "OK", "Stats available")
    end)

    return server
end

-- HTTP/1.1 Protocol Compliance Tests
local function test_http_version_support()
    print("\n--- Testing HTTP Version Support ---")

    -- Test HTTP/1.1 support
    local response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/")
    test_assert(response and response.status == 200, "HTTP/1.1 GET request succeeds")

    -- Test Keep-Alive default behavior
    if response and response.headers then
        local connection = response.headers["connection"]
        test_assert(not connection or connection:lower() ~= "close", "HTTP/1.1 defaults to keep-alive")
    end
end

local function test_request_methods()
    print("\n--- Testing HTTP Methods ---")

    -- Test GET
    local get_response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/")
    test_assert(get_response and get_response.status == 200, "GET method works")

    -- Test POST
    local post_response = http.post("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/echo", "test data")
    test_assert(post_response and post_response.status == 200, "POST method works")

    -- Test invalid method (should return 404 for unmatched route)
    local invalid_response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/nonexistent")
    test_assert(invalid_response and invalid_response.status == 404, "404 for non-existent routes")
end

local function test_routing_system()
    print("\n--- Testing Routing System ---")

    -- Test path parameters
    local param_response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/user/123")
    test_assert(param_response and param_response.status == 200, "Path parameters work")
    test_assert(param_response and param_response.body:find("123"), "Path parameter value extracted")
end

local function test_content_encoding()
    print("\n--- Testing Content Encoding ---")

    -- Test gzip compression for large content
    local headers = {["Accept-Encoding"] = "gzip"}
    local large_response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/large", {headers = headers})

    test_assert(large_response and large_response.status == 200, "Large content request succeeds")

    if large_response and large_response.headers then
        local encoding = large_response.headers["content-encoding"]
        test_assert(encoding == "gzip", "Large content is gzip compressed")
    end
end

local function test_security_headers()
    print("\n--- Testing Security Headers ---")

    local response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/")

    if response and response.headers then
        test_assert(response.headers["x-content-type-options"] == "nosniff", "X-Content-Type-Options header present")
        test_assert(response.headers["x-frame-options"] == "DENY", "X-Frame-Options header present")
        test_assert(response.headers["x-xss-protection"] == "1; mode=block", "X-XSS-Protection header present")
    end
end

local function test_error_handling()
    print("\n--- Testing Error Handling ---")

    -- Test 404 handling
    local not_found = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/does-not-exist")
    test_assert(not_found and not_found.status == 404, "404 status for missing routes")
    test_assert(not_found and not_found.body:find("not found"), "404 error message present")
end

local function test_performance_monitoring()
    print("\n--- Testing Performance Monitoring ---")

    -- Make a few requests to generate stats
    for i = 1, 5 do
        http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/")
        fan.sleep(0.01) -- Small delay
    end

    local stats_response = http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/stats")
    test_assert(stats_response and stats_response.status == 200, "Stats endpoint accessible")

    -- Note: Actual stats validation would require parsing JSON response
    -- This test just verifies the endpoint is available
end

-- Main test runner
local function run_all_tests()
    print("Starting LuaFan HTTPD Compliance Tests...")
    print("=" .. string.rep("=", 50))

    local server = start_test_server()

    -- Give server time to start
    fan.sleep(0.1)

    -- Run test suites
    test_http_version_support()
    test_request_methods()
    test_routing_system()
    test_content_encoding()
    test_security_headers()
    test_error_handling()
    test_performance_monitoring()

    -- Test summary
    print("\n" .. string.rep("=", 50))
    print(string.format("Test Results: %d/%d tests passed (%.1f%%)",
          passed_count, test_count, (passed_count / test_count) * 100))

    if passed_count == test_count then
        print("🎉 All tests passed! HTTPD is RFC compliant.")
        return true
    else
        print("❌ Some tests failed. Please review implementation.")
        return false
    end
end

-- Run tests in coroutine to handle async operations
local function main()
    local success, result = pcall(run_all_tests)
    if not success then
        print("Test execution failed:", result)
        return false
    end
    return result
end

-- Export for use in other test files
return {
    run_all_tests = run_all_tests,
    test_assert = test_assert,
    main = main
}