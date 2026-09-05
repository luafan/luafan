#!/usr/bin/env lua

-- Regression coverage for the native fan.httpd lifecycle hardening.

local TestFramework = require("test_framework")
local fan = require "fan"
local connector = require "fan.connector"
local http = require "fan.http"
local httpd = require "fan.httpd.core"

local suite = TestFramework.create_suite("HTTPD Lifecycle Regression Tests")

local function run_server(service, client)
    local server = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = service,
    })
    TestFramework.assert_not_nil(server)
    TestFramework.assert_true(server.port > 0)

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
        local ok, value = pcall(client, server.port)
        result = {ok = ok, value = value}
        done = true
        fan.loopbreak()
    end)()
    fan.loop()
    return result
end

suite:test("empty_form_params_returns_table", function()
    local captured
    local result = run_server(function(req, resp)
        captured = req.params
        resp:reply(200, "OK", "ok")
    end, function(port)
        return http.post({
            url = string.format("http://127.0.0.1:%d/", port),
            body = "",
            headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
        })
    end)
    TestFramework.assert_true(result.ok, tostring(result.value))
    TestFramework.assert_type(captured, "table")
    TestFramework.assert_equal(200, result.value.responseCode)
end)

suite:test("form_params_merge_duplicate_query_keys", function()
    local captured
    local result = run_server(function(req, resp)
        captured = req.params
        resp:reply(200, "OK", "ok")
    end, function(port)
        return http.post({
            url = string.format("http://127.0.0.1:%d/?key=query", port),
            body = "key=form",
            headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
        })
    end)
    TestFramework.assert_true(result.ok, tostring(result.value))
    TestFramework.assert_equal("query, form", captured.key)
end)

suite:test("patch_reaches_lua_handler", function()
    local method
    local result = run_server(function(req, resp)
        method = req.method
        resp:reply(200, "OK", "patched")
    end, function(port)
        local client = connector.connect("tcp://127.0.0.1:" .. port)
        client:send("PATCH / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n")
        local input = client:receive(1)
        local body = input and input:GetBytes() or ""
        client:close()
        return body
    end)
    TestFramework.assert_true(result.ok, tostring(result.value))
    TestFramework.assert_equal("PATCH", method)
    TestFramework.assert_true(result.value:find("200", 1, true) ~= nil)
end)

suite:test("reply_closes_started_chunked_metrics_once", function()
    local result = run_server(function(req, resp)
        resp:reply_start(200, "OK")
        resp:reply_chunk("part")
        resp:reply(200, "OK", "final")
    end, function(port)
        return http.get(string.format("http://127.0.0.1:%d/", port))
    end)
    TestFramework.assert_true(result.ok, tostring(result.value))
    TestFramework.assert_equal(200, result.value.responseCode)
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
