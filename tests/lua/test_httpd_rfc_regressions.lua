#!/usr/bin/env lua

-- Regression tests for Lua HTTPD request framing and response semantics.

local TestFramework = require("test_framework")
local fan = require "fan"
local connector = require "fan.connector"
local httpd = require "fan.httpd.httpd"

local suite = TestFramework.create_suite("Lua HTTPD RFC Regression Tests")

local function read_response(client, timeout)
    timeout = timeout or 2
    local chunks = {}
    local deadline = fan.gettime() + timeout
    while fan.gettime() < deadline do
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
    end
    return table.concat(chunks)
end

local function run_raw(raw_request, service)
    local server = httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = service,
    })
    TestFramework.assert_true(server.port > 0)

    local result
    local done = false
    coroutine.wrap(function()
        local client = connector.connect("tcp://127.0.0.1:" .. server.port)
        client:send(raw_request)
        result = read_response(client)
        client:close()
        done = true
        fan.loopbreak()
    end)()
    coroutine.wrap(function()
        fan.sleep(3)
        if not done then
            done = true
            fan.loopbreak()
        end
    end)()
    fan.loop()
    return result
end

suite:test("bare_lf_is_rejected", function()
    local called = false
    local response = run_raw("GET / HTTP/1.1\nHost: localhost\n\n", function(req, resp)
        called = true
        resp:reply(200, "OK", "bad")
    end)
    TestFramework.assert_true(not called)
    TestFramework.assert_true(response:find("400 Bad Request", 1, true) ~= nil)
end)

suite:test("transfer_encoding_is_rejected", function()
    local called = false
    local response = run_raw("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n0\r\n\r\n", function(req, resp)
        called = true
        resp:reply(200, "OK", "bad")
    end)
    TestFramework.assert_true(not called)
    TestFramework.assert_true(response:find("501 Not Implemented", 1, true) ~= nil)
    TestFramework.assert_true(response:find("Connection: close", 1, true) ~= nil)
end)

suite:test("duplicate_content_length_is_rejected", function()
    local response = run_raw("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nContent-Length: 10\r\n\r\n1234567890", function(req, resp)
        resp:reply(200, "OK", "bad")
    end)
    TestFramework.assert_true(response:find("400 Bad Request", 1, true) ~= nil)
end)

suite:test("body_does_not_consume_next_request", function()
    local bodies = {}
    local count = 0
    local response = run_raw("POST /one HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbodyGET /two HTTP/1.1\r\nHost: localhost\r\n\r\n", function(req, resp)
        count = count + 1
        bodies[count] = req.body
        resp:reply(200, "OK", "ok")
    end)
    TestFramework.assert_equal(1, count)
    TestFramework.assert_equal("body", bodies[1])
    TestFramework.assert_true(response:find("200 OK", 1, true) ~= nil)
end)

suite:test("head_and_204_do_not_send_body", function()
    local head_response = run_raw("HEAD / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", function(req, resp)
        resp:reply(200, "OK", "hidden")
    end)
    TestFramework.assert_true(head_response:find("Content-Length: 6", 1, true) ~= nil)
    TestFramework.assert_true(head_response:find("\r\n\r\nhidden", 1, true) == nil)

    local no_content = run_raw("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", function(req, resp)
        resp:reply(204, "No Content", "hidden")
    end)
    TestFramework.assert_true(no_content:find("204 No Content", 1, true) ~= nil)
    TestFramework.assert_true(no_content:find("Content-Length:", 1, true) == nil)
    TestFramework.assert_true(no_content:find("Content-Encoding:", 1, true) == nil)
    TestFramework.assert_true(no_content:find("\r\n\r\nhidden", 1, true) == nil)
end)

suite:test("response_header_crlf_is_rejected", function()
    local response = run_raw("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", function(req, resp)
        resp:addheader("X-Bad", "value\r\nInjected: yes")
        resp:reply(200, "OK", "bad")
    end)
    TestFramework.assert_true(response:find("500 Internal Server Error", 1, true) ~= nil)
    TestFramework.assert_true(response:find("Injected: yes", 1, true) == nil)
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
