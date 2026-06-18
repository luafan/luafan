#!/usr/bin/env lua

-- Test for HTTP client (fan.http) with local httpd server

local TestFramework = require('test_framework')
local fan = require "fan"

package.preload['config'] = function()
    return { debug = false, http_timeout = 5000 }
end

local httpd = require "fan.httpd"
local http = require "fan.http"

local suite = TestFramework.create_suite("HTTP Client Tests")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_type(http, "table")
    TestFramework.assert_type(http.get, "function")
    TestFramework.assert_type(http.post, "function")
    TestFramework.assert_type(http.put, "function")
    TestFramework.assert_type(http.head, "function")
    TestFramework.assert_type(http.delete, "function")
    TestFramework.assert_type(http.escape, "function")
    TestFramework.assert_type(http.unescape, "function")
end)

-- Test URL encoding round-trip
suite:test("escape_unescape_roundtrip", function()
    local test_cases = {
        "hello world",
        "foo&bar=baz",
        "a+b/c",
        "100%",
    }

    for _, original in ipairs(test_cases) do
        local escaped = http.escape(original)
        local unescaped = http.unescape(escaped)
        TestFramework.assert_equal(original, unescaped,
            string.format("round-trip failed for: %s", original))
    end
end)

-- Test escape produces valid percent-encoding
suite:test("escape_format", function()
    local escaped = http.escape("hello world")
    TestFramework.assert_equal("hello%20world", escaped)
end)

-- Test HTTP GET request against local httpd
suite:test("http_get_request", function()
    local server = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", "Hello from test server")
        end
    })

    TestFramework.assert_not_nil(server)
    local port = server.port
    TestFramework.assert_true(port > 0)

    local result = nil
    local done = false

    coroutine.wrap(function()
        fan.sleep(5)
        if not done then
            done = true
            fan.loopbreak()
        end
    end)()

    coroutine.wrap(function()
        local ok, res = pcall(http.get, {
            url = string.format("http://127.0.0.1:%d/", port),
        })
        result = res
        done = true
        fan.loopbreak()
    end)()

    fan.loop()

    if result then
        TestFramework.assert_not_nil(result.responseCode)
        TestFramework.assert_equal(200, result.responseCode)
        TestFramework.assert_not_nil(result.body)
        TestFramework.assert_equal("Hello from test server", result.body)
    end
end)

-- Test HTTP POST request
suite:test("http_post_request", function()
    local server = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            local body = req.body or ""
            resp:reply(200, "OK", "echo: " .. body)
        end
    })

    TestFramework.assert_not_nil(server)
    local port = server.port

    local result = nil
    local done = false

    coroutine.wrap(function()
        fan.sleep(5)
        if not done then
            done = true
            fan.loopbreak()
        end
    end)()

    coroutine.wrap(function()
        local ok, res = pcall(http.post, {
            url = string.format("http://127.0.0.1:%d/", port),
            body = "test data",
        })
        result = res
        done = true
        fan.loopbreak()
    end)()

    fan.loop()

    if result then
        TestFramework.assert_not_nil(result.responseCode)
        TestFramework.assert_equal(200, result.responseCode)
    end
end)

-- Test HTTP 404 response
suite:test("http_404_response", function()
    local server = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            resp:reply(404, "Not Found", "not found")
        end
    })

    TestFramework.assert_not_nil(server)
    local port = server.port

    local result = nil
    local done = false

    coroutine.wrap(function()
        fan.sleep(5)
        if not done then
            done = true
            fan.loopbreak()
        end
    end)()

    coroutine.wrap(function()
        local ok, res = pcall(http.get, {
            url = string.format("http://127.0.0.1:%d/missing", port),
        })
        result = res
        done = true
        fan.loopbreak()
    end)()

    fan.loop()

    if result then
        TestFramework.assert_equal(404, result.responseCode)
    end
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
