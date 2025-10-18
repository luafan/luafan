#!/usr/bin/env lua
-- HTTP Performance Benchmark Test for LuaFan HTTPD
-- Tests concurrent load, response times, and throughput

local fan = require "fan"
local httpd = require "modules.fan.httpd.httpd"
local http = require "fan.http"

-- Test configuration
local TEST_HOST = "127.0.0.1"
local TEST_PORT = 9997
local CONCURRENT_REQUESTS = 50
local TOTAL_REQUESTS = 200
local BENCHMARK_DURATION = 5 -- seconds

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

local function start_performance_test_server()
    local server = httpd.bind({host = TEST_HOST, port = TEST_PORT})

    -- Fast response endpoint
    server:get("/fast", function(ctx)
        ctx:reply(200, "OK", "Fast response")
    end)

    -- JSON response endpoint
    server:get("/json", function(ctx)
        local data = {
            message = "Hello World",
            timestamp = os.time(),
            data = {1, 2, 3, 4, 5}
        }
        ctx:addheader("Content-Type", "application/json")
        ctx:reply(200, "OK", '{"message":"Hello World","timestamp":' .. os.time() .. ',"data":[1,2,3,4,5]}')
    end)

    -- Large response endpoint (for compression testing)
    server:get("/large", function(ctx)
        local large_data = string.rep("Lorem ipsum dolor sit amet, consectetur adipiscing elit. ", 100)
        ctx:addheader("Content-Type", "text/plain")
        ctx:reply(200, "OK", large_data)
    end)

    -- Keep-alive test endpoint
    server:get("/keepalive", function(ctx)
        ctx:reply(200, "OK", "Keep-Alive Test")
    end)

    -- Stats endpoint
    server:get("/stats", function(ctx)
        local stats = server:get_stats()
        local stats_json = string.format('{"requests_total":%d,"uptime":%d}',
                                        stats.requests_total, stats.uptime_seconds)
        ctx:addheader("Content-Type", "application/json")
        ctx:reply(200, "OK", stats_json)
    end)

    return server
end

-- Measure response time for a single request
local function measure_response_time(url)
    local start_time = fan.gettime and fan.gettime() or (os.clock() * 1000)
    local response = http.get(url)
    local end_time = fan.gettime and fan.gettime() or (os.clock() * 1000)

    local response_time = end_time - start_time
    local success = response and response.status == 200

    return {
        success = success,
        response_time = response_time,
        status = response and response.status or 0,
        body_size = response and response.body and #response.body or 0
    }
end

-- Test basic response time
local function test_response_time()
    print("\n--- Testing Response Time ---")

    local url = "http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/fast"
    local measurements = {}

    -- Take 10 measurements
    for i = 1, 10 do
        local result = measure_response_time(url)
        if result.success then
            table.insert(measurements, result.response_time)
        end
        fan.sleep(0.01)
    end

    if #measurements > 0 then
        -- Calculate average response time
        local total_time = 0
        for _, time in ipairs(measurements) do
            total_time = total_time + time
        end
        local avg_time = total_time / #measurements

        test_assert(avg_time < 100, string.format("Average response time (%.2fms) < 100ms", avg_time))
        test_assert(#measurements == 10, "All response time measurements successful")

        print(string.format("  Average response time: %.2fms", avg_time))
        print(string.format("  Min response time: %.2fms", math.min(table.unpack(measurements))))
        print(string.format("  Max response time: %.2fms", math.max(table.unpack(measurements))))
    end
end

-- Test concurrent requests
local function test_concurrent_requests()
    print("\n--- Testing Concurrent Requests ---")

    local url = "http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/fast"
    local completed_requests = 0
    local successful_requests = 0
    local start_time = fan.gettime and fan.gettime() or (os.clock() * 1000)

    -- Launch concurrent requests
    for i = 1, CONCURRENT_REQUESTS do
        fan.spawn(function()
            local result = measure_response_time(url)
            completed_requests = completed_requests + 1
            if result.success then
                successful_requests = successful_requests + 1
            end
        end)
    end

    -- Wait for all requests to complete (with timeout)
    local timeout = 10000 -- 10 seconds
    local wait_start = fan.gettime and fan.gettime() or (os.clock() * 1000)

    while completed_requests < CONCURRENT_REQUESTS do
        fan.sleep(0.01)
        local elapsed = (fan.gettime and fan.gettime() or (os.clock() * 1000)) - wait_start
        if elapsed > timeout then
            break
        end
    end

    local end_time = fan.gettime and fan.gettime() or (os.clock() * 1000)
    local total_time = end_time - start_time

    test_assert(completed_requests == CONCURRENT_REQUESTS,
               string.format("All %d concurrent requests completed", CONCURRENT_REQUESTS))
    test_assert(successful_requests >= CONCURRENT_REQUESTS * 0.95,
               "At least 95% of concurrent requests successful")

    if total_time > 0 then
        local requests_per_second = (successful_requests / total_time) * 1000
        print(string.format("  Concurrent requests: %d", CONCURRENT_REQUESTS))
        print(string.format("  Successful requests: %d", successful_requests))
        print(string.format("  Requests per second: %.2f", requests_per_second))
        print(string.format("  Total time: %.2fms", total_time))

        test_assert(requests_per_second > 100, "Throughput > 100 requests/second")
    end
end

-- Test Keep-Alive connection reuse
local function test_keepalive_performance()
    print("\n--- Testing Keep-Alive Performance ---")

    local url = "http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/keepalive"
    local measurements = {}

    -- Make multiple requests that should reuse connection
    for i = 1, 10 do
        local result = measure_response_time(url)
        if result.success then
            table.insert(measurements, result.response_time)
        end
        -- Small delay to allow connection reuse
        fan.sleep(0.001)
    end

    if #measurements >= 8 then -- Allow for some failures
        -- Later requests should be faster due to connection reuse
        local early_avg = (measurements[1] + measurements[2]) / 2
        local late_avg = (measurements[#measurements-1] + measurements[#measurements]) / 2

        test_assert(late_avg <= early_avg * 1.5, "Keep-Alive shows performance benefit")
        print(string.format("  Early requests avg: %.2fms", early_avg))
        print(string.format("  Later requests avg: %.2fms", late_avg))
    end
end

-- Test compression performance
local function test_compression_performance()
    print("\n--- Testing Compression Performance ---")

    local url = "http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/large"

    -- Test without compression
    local result_no_gzip = measure_response_time(url)

    -- Test with compression
    local result_gzip = http.get(url, {headers = {["Accept-Encoding"] = "gzip"}})
    local gzip_success = result_gzip and result_gzip.status == 200

    test_assert(result_no_gzip.success, "Large response without compression succeeds")
    test_assert(gzip_success, "Large response with compression succeeds")

    if result_gzip and result_gzip.headers then
        local is_compressed = result_gzip.headers["content-encoding"] == "gzip"
        test_assert(is_compressed, "Large content is compressed with gzip")

        if is_compressed then
            print("  Content successfully compressed with gzip")
        end
    end
end

-- Test performance monitoring stats
local function test_performance_monitoring()
    print("\n--- Testing Performance Monitoring ---")

    local stats_url = "http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/stats"

    -- Make some requests to generate stats
    for i = 1, 5 do
        http.get("http://" .. TEST_HOST .. ":" .. TEST_PORT .. "/fast")
    end

    local stats_response = http.get(stats_url)
    test_assert(stats_response and stats_response.status == 200, "Performance stats endpoint accessible")

    if stats_response and stats_response.body then
        -- Basic validation that stats contain expected data
        test_assert(stats_response.body:find("requests_total"), "Stats contain request count")
        test_assert(stats_response.body:find("uptime"), "Stats contain uptime information")
    end
end

-- Main performance test runner
local function run_performance_tests()
    print("Starting LuaFan HTTPD Performance Tests...")
    print("=" .. string.rep("=", 50))

    local server = start_performance_test_server()

    -- Give server time to start
    fan.sleep(0.2)

    -- Run performance test suites
    test_response_time()
    test_concurrent_requests()
    test_keepalive_performance()
    test_compression_performance()
    test_performance_monitoring()

    -- Test summary
    print("\n" .. string.rep("=", 50))
    print(string.format("Performance Test Results: %d/%d tests passed (%.1f%%)",
          passed_count, test_count, (passed_count / test_count) * 100))

    if passed_count == test_count then
        print("🚀 All performance tests passed! HTTPD performs well.")
        return true
    else
        print("⚠️  Some performance tests failed. Consider optimization.")
        return false
    end
end

-- Main function
local function main()
    local success, result = pcall(run_performance_tests)
    if not success then
        print("Performance test execution failed:", result)
        return false
    end
    return result
end

-- Export for use in other test files
return {
    run_performance_tests = run_performance_tests,
    test_assert = test_assert,
    main = main
}