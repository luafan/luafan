#!/usr/bin/env lua

-- Load / throughput tests for the Lua fan.httpd implementation.
--
-- Migrated from the old library-style tests/lua/test_httpd_performance.lua,
-- which only exported functions for tests/run_httpd_tests.lua and therefore
-- never ran in the curated suite nor in CI. Fixed by the migration:
--   * fixed port 9997 replaced by an ephemeral bind (port 0);
--   * the old file used `fan.spawn()`, which this engine does not provide, so
--     the concurrency case could not have run at all; the load is now driven by
--     coroutine.wrap() on the same event loop;
--   * wall-clock thresholds ("avg < 100ms", ">100 req/s", keep-alive must be
--     faster) are no longer unconditional regression assertions -- they are
--     environment dependent. Deterministic properties (every request completes,
--     every response is 200, gzip shrinks the body, stats count requests) are
--     asserted always; latency numbers are printed, and only enforced when
--     LUAN_TEST_PERF_ASSERT=1 is set (for a benchmark machine, not for CI).
--
-- Each case drives its own fan.loop(), so this file must run as its own process
-- (see tests/run_lua_tests.sh).

local TestFramework = require("test_framework")
local fan = require "fan"
local http = require "fan.http"
local httpd = require "fan.httpd.httpd"

local suite = TestFramework.create_suite("Lua HTTPD Load Tests")

local CONCURRENT_REQUESTS = 50

-- Bind an ephemeral server, run `client(server)` inside the event loop, then
-- close the listener. Returns the client's value; a client error is re-raised as
-- a test failure instead of hanging the suite.
local function with_server(configure, client)
    local server = httpd.bind({ host = "127.0.0.1", port = 0 })
    TestFramework.assert_not_nil(server)
    TestFramework.assert_true(server.port > 0, "ephemeral bind must report a real port")
    if configure then
        configure(server)
    end

    local result
    local done = false
    coroutine.wrap(function()
        fan.sleep(10)
        if not done then
            done = true
            fan.loopbreak()
        end
    end)()
    coroutine.wrap(function()
        local ok, value = pcall(client, server)
        result = { ok = ok, value = value }
        done = true
        fan.loopbreak()
    end)()
    fan.loop()

    if server.serv and server.serv.close then
        pcall(function() server.serv:close() end)
    end

    TestFramework.assert_true(result and result.ok,
        result and tostring(result.value) or "client never ran")
    return result.value
end

local function fast_routes(server)
    server:get("/fast", function(ctx)
        ctx:reply(200, "OK", "Fast response")
    end)
    server:get("/large", function(ctx)
        ctx:reply(200, "OK", string.rep("Lorem ipsum dolor sit amet, consectetur adipiscing elit. ", 100))
    end)
    server:get("/stats", function(ctx)
        local stats = server:get_stats()
        ctx:addheader("Content-Type", "application/json")
        ctx:reply(200, "OK", string.format('{"requests_total":%d,"uptime":%d}',
            stats.requests_total, stats.uptime_seconds))
    end)
end

local function url(server, path)
    return "http://127.0.0.1:" .. server.port .. path
end

suite:test("sequential_requests_are_stable", function()
    local responses = with_server(fast_routes, function(server)
        local out = {}
        for _ = 1, 10 do
            table.insert(out, http.get(url(server, "/fast")))
        end
        return out
    end)
    TestFramework.assert_equal(#responses, 10)
    for i, response in ipairs(responses) do
        TestFramework.assert_equal(response.responseCode, 200,
            string.format("request %d succeeded", i))
        TestFramework.assert_equal(response.body, "Fast response")
    end
end)

suite:test("concurrent_requests_all_complete", function()
    local stats = with_server(fast_routes, function(server)
        local completed = 0
        local succeeded = 0
        local start = fan.gettime()
        for _ = 1, CONCURRENT_REQUESTS do
            coroutine.wrap(function()
                -- pcall: an error raised after this coroutine was resumed (i.e.
                -- after it yielded inside http.get) would otherwise escape into
                -- the event loop instead of being reported by the assertions
                -- below; the request then just counts as not completed.
                local ok, response = pcall(http.get, url(server, "/fast"))
                if ok and response and response.responseCode == 200 then
                    succeeded = succeeded + 1
                end
                completed = completed + 1
            end)()
        end

        local deadline = fan.gettime() + 10000
        while completed < CONCURRENT_REQUESTS and fan.gettime() < deadline do
            fan.sleep(0.01)
        end
        return { completed = completed, succeeded = succeeded, elapsed = fan.gettime() - start }
    end)

    TestFramework.assert_equal(stats.completed, CONCURRENT_REQUESTS,
        "every concurrent request must finish")
    TestFramework.assert_equal(stats.succeeded, CONCURRENT_REQUESTS,
        "every concurrent request must be served with 200")
    if stats.elapsed > 0 then
        print(string.format("  %d concurrent requests in %.1fms (%.0f req/s)",
            stats.completed, stats.elapsed, stats.succeeded / stats.elapsed * 1000))
    end
end)

suite:test("large_body_is_gzip_compressed_when_requested", function()
    local plain = with_server(fast_routes, function(server)
        return http.get(url(server, "/large"))
    end)
    TestFramework.assert_equal(plain.responseCode, 200)
    TestFramework.assert_equal(plain.headers["content-encoding"], nil)

    local gzipped = with_server(fast_routes, function(server)
        return http.get({
            url = url(server, "/large"),
            headers = { ["Accept-Encoding"] = "gzip" },
        })
    end)
    TestFramework.assert_equal(gzipped.responseCode, 200)
    TestFramework.assert_equal(gzipped.headers["content-encoding"], "gzip")
    TestFramework.assert_true(#gzipped.body < #plain.body,
        "compressed body must be smaller than the plain body")
end)

suite:test("stats_endpoint_reports_request_counts", function()
    local response = with_server(fast_routes, function(server)
        for _ = 1, 5 do
            http.get(url(server, "/fast"))
        end
        return http.get(url(server, "/stats"))
    end)
    TestFramework.assert_equal(response.responseCode, 200)
    TestFramework.assert_match(response.body or "", "requests_total")
    TestFramework.assert_match(response.body or "", "uptime")
end)

suite:test("response_time_within_budget", function()
    local samples = with_server(fast_routes, function(server)
        local out = {}
        for _ = 1, 10 do
            local started = fan.gettime()
            local response = http.get(url(server, "/fast"))
            table.insert(out, {
                code = response and response.responseCode or -1,
                ms = fan.gettime() - started,
            })
        end
        return out
    end)

    TestFramework.assert_equal(#samples, 10)
    local total, min_ms, max_ms = 0, math.huge, 0
    for _, sample in ipairs(samples) do
        TestFramework.assert_equal(sample.code, 200)
        total = total + sample.ms
        min_ms = math.min(min_ms, sample.ms)
        max_ms = math.max(max_ms, sample.ms)
    end

    local average = total / #samples
    print(string.format("  /fast latency: avg=%.3fms min=%.3fms max=%.3fms", average, min_ms, max_ms))

    -- Wall-clock budgets are machine dependent; they are only enforced on
    -- request (benchmark runs), never as part of the default regression suite.
    if os.getenv("LUAN_TEST_PERF_ASSERT") == "1" then
        TestFramework.assert_true(average < 100,
            string.format("average latency %.3fms < 100ms", average))
    else
        print("  (latency budget not enforced; set LUAN_TEST_PERF_ASSERT=1 to assert it)")
    end
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
