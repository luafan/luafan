#!/usr/bin/env lua

-- HTTP protocol compliance tests for the Lua fan.httpd implementation
-- (RFC 7230/7231 framing, routing, compression, security headers, stats).
--
-- Migrated from the old library-style tests/lua/test_httpd_compliance.lua. That
-- file only exported functions for tests/run_httpd_tests.lua, so it never ran in
-- the curated suite nor in CI. Three defects are fixed by the migration:
--   * the server used a fixed port (9999); every case now binds port 0 and reads
--     the real port back from the server object, so runs never clash;
--   * `http.get(url, {headers = ...})` passed a second argument that fan.http
--     ignores (request() takes a single args table), so the Accept-Encoding
--     header never reached the server and the gzip case could not assert;
--   * assertions ran only when a response existed ("if response then ..."), so a
--     missing response produced a silent pass. Every case now asserts
--     unconditionally.
--
-- Like the other standalone httpd suites (test_httpd_rfc_regressions.lua,
-- test_httpd_lifecycle_regressions.lua) each case drives its own fan.loop(), so
-- this file must run as its own process (see tests/run_lua_tests.sh).

local TestFramework = require("test_framework")
local fan = require "fan"
local http = require "fan.http"
local connector = require "fan.connector"
local httpd = require "fan.httpd.httpd"

local suite = TestFramework.create_suite("Lua HTTPD Protocol Compliance")

-- Read a raw response until the header terminator (or the peer closes).
-- fan.gettime() is the event loop's cached clock: it only advances when the
-- loop iterates, so the loop must yield. A non-yielding busy loop would never
-- reach the deadline nor observe the peer's disconnect and would spin forever.
local function read_response(client, timeout)
    timeout = (timeout or 2) * 1000
    local chunks = {}
    local deadline = fan.gettime() + timeout
    local attempts = 0
    while fan.gettime() < deadline and attempts < 500 do
        attempts = attempts + 1
        local input = client:receive(1)
        if not input then
            break
        end
        local data = input:GetBytes()
        if data and #data > 0 then
            table.insert(chunks, data)
            if table.concat(chunks):find("\r\n\r\n", 1, true) then
                break
            end
        end
        fan.sleep(0.001)
    end
    return table.concat(chunks)
end

-- Bind a router-based server on an ephemeral port, run `client(server)` inside
-- the event loop, then close the listener. Returns the client's value; a client
-- error is re-raised as a test failure instead of hanging the suite.
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
        fan.sleep(5)
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

local function base_routes(server)
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
        ctx:reply(200, "OK", string.rep("A", 2048))
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

suite:test("http_11_get_returns_200_and_body", function()
    local response = with_server(base_routes, function(server)
        return http.get(url(server, "/"))
    end)
    TestFramework.assert_equal(response.responseCode, 200)
    TestFramework.assert_equal(response.body, "Hello World")
end)

suite:test("http_11_defaults_to_keep_alive", function()
    -- fan.http always sends "Connection: close", so the HTTP/1.1 default has to
    -- be observed with a raw request that carries no Connection header.
    local raw = with_server(base_routes, function(server)
        local client = connector.connect("tcp://127.0.0.1:" .. server.port)
        client:send("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
        local response = read_response(client)
        client:close()
        return response
    end)
    TestFramework.assert_match(raw, "HTTP/1%.1 200")
    TestFramework.assert_true(raw:find("Connection: close", 1, true) == nil,
        "HTTP/1.1 without a Connection header must not announce close")
end)

suite:test("http_10_request_is_answered_and_closes", function()
    local raw = with_server(base_routes, function(server)
        local client = connector.connect("tcp://127.0.0.1:" .. server.port)
        client:send("GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n")
        local response = read_response(client)
        client:close()
        return response
    end)
    TestFramework.assert_match(raw, "HTTP/1%.0 200")
    TestFramework.assert_true(raw:find("Connection: close", 1, true) ~= nil,
        "HTTP/1.0 without keep-alive must announce close")
end)

suite:test("post_body_is_echoed", function()
    local response = with_server(base_routes, function(server)
        return http.post({ url = url(server, "/echo"), body = "test data" })
    end)
    TestFramework.assert_equal(response.responseCode, 200)
    TestFramework.assert_equal(response.body, "test data")
end)

suite:test("path_parameter_is_extracted", function()
    local response = with_server(base_routes, function(server)
        return http.get(url(server, "/user/123"))
    end)
    TestFramework.assert_equal(response.responseCode, 200)
    TestFramework.assert_equal(response.body, "User ID: 123")
end)

suite:test("unknown_route_returns_404_with_message", function()
    local response = with_server(base_routes, function(server)
        return http.get(url(server, "/does-not-exist"))
    end)
    TestFramework.assert_equal(response.responseCode, 404)
    TestFramework.assert_match(response.body or "", "not found")
end)

suite:test("large_body_is_gzip_compressed_when_requested", function()
    local plain = with_server(base_routes, function(server)
        return http.get(url(server, "/large"))
    end)
    TestFramework.assert_equal(plain.responseCode, 200)
    TestFramework.assert_equal(plain.headers["content-encoding"], nil)
    TestFramework.assert_equal(#plain.body, 2048)

    local gzipped = with_server(base_routes, function(server)
        return http.get({
            url = url(server, "/large"),
            headers = { ["Accept-Encoding"] = "gzip" },
        })
    end)
    TestFramework.assert_equal(gzipped.responseCode, 200)
    TestFramework.assert_equal(gzipped.headers["content-encoding"], "gzip")
    TestFramework.assert_true(#gzipped.body < 2048,
        "compressed body must be smaller than the 2KB plain body")
end)

suite:test("security_headers_are_present_on_served_response", function()
    local response = with_server(base_routes, function(server)
        return http.get(url(server, "/"))
    end)
    TestFramework.assert_equal(response.responseCode, 200)
    TestFramework.assert_equal(response.headers["x-content-type-options"], "nosniff")
    TestFramework.assert_equal(response.headers["x-frame-options"], "DENY")
    TestFramework.assert_equal(response.headers["x-xss-protection"], "1; mode=block")
end)

suite:test("stats_endpoint_reports_request_count", function()
    local response = with_server(base_routes, function(server)
        http.get(url(server, "/"))
        return http.get(url(server, "/stats"))
    end)
    TestFramework.assert_equal(response.responseCode, 200)
    TestFramework.assert_match(response.body or "", "requests_total")
    TestFramework.assert_match(response.body or "", "uptime")
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
