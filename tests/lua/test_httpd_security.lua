#!/usr/bin/env lua

-- HTTP security / hardening tests for the Lua fan.httpd implementation
-- (framing rejection, request limits, rate limiting, error hygiene).
--
-- Migrated from the old library-style tests/lua/test_httpd_security.lua, which
-- only exported functions for tests/run_httpd_tests.lua and therefore never ran
-- in the curated suite nor in CI. Fixed by the migration:
--   * fixed port 9998 replaced by an ephemeral bind (port 0) per case;
--   * the global `config` keys the suite mutates (max_content_length, rate
--     limiting) are saved and restored in a suite teardown, and the rate-limit
--     test is the last one so no later case inherits a tripped limiter;
--   * "if response then assert(...)" guards removed: every case now fails when
--     the expected response is missing. Several old expectations were wrong
--     (an unknown method falls through the router to 404, not 400) and are
--     asserted against the behaviour the server actually implements.
--
-- Each case drives its own fan.loop(), so this file must run as its own process
-- (see tests/run_lua_tests.sh).

local TestFramework = require("test_framework")
local fan = require "fan"
local http = require "fan.http"
local connector = require "fan.connector"
local httpd = require "fan.httpd.httpd"
local config = require "config"

local suite = TestFramework.create_suite("Lua HTTPD Security Tests")

local CONFIG_KEYS = {
    "max_content_length",
    "enable_rate_limiting",
    "rate_limit_requests",
    "rate_limit_window",
}
local saved_config = {}
for _, key in ipairs(CONFIG_KEYS) do
    saved_config[key] = config[key]
end

suite:set_teardown(function()
    for _, key in ipairs(CONFIG_KEYS) do
        config[key] = saved_config[key]
    end
end)

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

-- Bind an ephemeral server, send one raw request, return the raw response text.
local function raw_request(request, configure)
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
        local ok, value = pcall(function()
            local client = connector.connect("tcp://127.0.0.1:" .. server.port)
            client:send(request)
            local response = read_response(client)
            client:close()
            return response
        end)
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

-- Bind an ephemeral server, run `client(server)` inside the event loop.
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

local function url(server, path)
    return "http://127.0.0.1:" .. server.port .. path
end

local function base_routes(server)
    server:get("/", function(ctx)
        ctx:reply(200, "OK", "Test endpoint")
    end)
    server:post("/upload", function(ctx)
        ctx:reply(200, "OK", "Upload received: " .. #(ctx.body or ""))
    end)
end

suite:test("malformed_header_line_returns_400", function()
    local raw = raw_request(
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nInvalid-Header-No-Colon\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "400 Bad Request")
end)

suite:test("invalid_content_length_returns_400", function()
    local raw = raw_request(
        "POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: abc\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "400 Bad Request")
end)

suite:test("oversized_header_value_returns_431", function()
    local raw = raw_request(
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Big: " .. string.rep("a", 9000) .. "\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "431 Request Header Fields Too Large")
end)

suite:test("oversized_content_length_returns_413", function()
    config.max_content_length = 1024
    -- The framing check runs while the headers are parsed, so the 413 is sent
    -- without the client ever transmitting the body.
    local raw = raw_request(
        "POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2048\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "413 Content Too Large")
end)

suite:test("oversized_uri_returns_414", function()
    local raw = raw_request(
        "GET /" .. string.rep("a", 2100) .. " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "414 URI Too Long")
end)

suite:test("unsupported_http_version_returns_505", function()
    local raw = raw_request(
        "GET / HTTP/2.0\r\nHost: 127.0.0.1\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "505 HTTP Version Not Supported")
end)

suite:test("unknown_method_is_not_routed", function()
    -- The request-line token grammar accepts any method, so an unknown verb is
    -- parsed and then falls through the router to its not-found handler.
    local raw = raw_request(
        "INVALID / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
        base_routes)
    TestFramework.assert_match(raw, "404 Not Found")
    TestFramework.assert_true(raw:find("200 OK", 1, true) == nil,
        "unknown method must not be served")
end)

suite:test("path_traversal_returns_404_without_leaking_paths", function()
    local response = with_server(base_routes, function(server)
        return http.get(url(server, "/../../etc/passwd"))
    end)
    TestFramework.assert_equal(response.responseCode, 404)
    local body = (response.body or ""):lower()
    TestFramework.assert_true(body:find("/etc/", 1, true) == nil, "body must not expose filesystem paths")
    TestFramework.assert_true(body:find("lua", 1, true) == nil, "body must not expose implementation details")
end)

suite:test("rate_limit_exceeded_returns_429", function()
    -- Keep this case last: the limiter counter is process-global per client IP.
    config.enable_rate_limiting = true
    config.rate_limit_requests = 5
    config.rate_limit_window = 10

    local ok, err = pcall(function()
        local responses = with_server(base_routes, function(server)
            local out = {}
            for _ = 1, 6 do
                table.insert(out, http.get(url(server, "/")))
            end
            return out
        end)

        TestFramework.assert_equal(#responses, 6)
        for i = 1, 5 do
            TestFramework.assert_equal(responses[i].responseCode, 200,
                string.format("request %d is inside the limit", i))
        end
        TestFramework.assert_equal(responses[6].responseCode, 429)
        TestFramework.assert_match(responses[6].body or "", "Rate limit exceeded")
    end)

    config.enable_rate_limiting = false
    if not ok then
        error(err, 0)
    end
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
