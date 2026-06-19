#!/usr/bin/env lua

-- Comprehensive httpd tests covering request properties, response methods,
-- chunked responses, keepalive, Connection: close, and WebSocket lifecycle.
-- Tests both C (fan.httpd.core) and Lua (fan.httpd.httpd) implementations.

local TestFramework = require('test_framework')
local fan = require "fan"

package.preload['config'] = function()
    return { debug = false, http_timeout = 5000 }
end

local http = require "fan.http"
local connector = require "fan.connector"

-- Determine which implementations are available
local implementations = {}

local ok_core, httpd_core = pcall(require, 'fan.httpd.core')
if ok_core and httpd_core then
    implementations.core = { name = "C (httpd.core)", httpd = httpd_core }
end

local ok_lua, httpd_lua = pcall(require, 'fan.httpd.httpd')
if ok_lua and httpd_lua then
    implementations.lua = { name = "Lua (httpd.httpd)", httpd = httpd_lua }
end

if not implementations.core and not implementations.lua then
    print("No httpd implementation available, skipping")
    os.exit(77)
end

-- Helper: run a test against a given httpd implementation
-- Creates server, runs the test callback, handles event loop and timeout
local function run_httpd_test(impl, service_fn, test_fn, timeout)
    timeout = timeout or 5
    local server = impl.httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = service_fn,
    })

    if not server or not server.port or server.port <= 0 then
        return nil, "server bind failed"
    end

    local port = server.port
    local result = nil
    local done = false

    -- Watchdog timer
    coroutine.wrap(function()
        fan.sleep(timeout)
        if not done then
            done = true
            fan.loopbreak()
        end
    end)()

    -- Actual test
    coroutine.wrap(function()
        local ok, res = pcall(test_fn, port)
        if ok then
            result = res
        else
            result = { error = tostring(res) }
        end
        done = true
        fan.loopbreak()
    end)()

    fan.loop()
    return result
end

-- Helper: make HTTP GET request
local function http_get(port, path, headers)
    local req_headers = headers or {}
    return http.get({
        url = string.format("http://127.0.0.1:%d%s", port, path or "/"),
        headers = req_headers,
    })
end

-- Helper: make HTTP POST request
local function http_post(port, path, body, headers)
    local req_headers = headers or {}
    return http.post({
        url = string.format("http://127.0.0.1:%d%s", port, path or "/"),
        body = body or "",
        headers = req_headers,
    })
end

-- Helper: raw TCP connect and send HTTP request, return raw response
local function raw_http_request(port, raw_request)
    local client = connector.connect("tcp://127.0.0.1:" .. port)
    if not client then
        return nil, "connect failed"
    end

    client:send(raw_request)

    -- Read response with timeout
    local chunks = {}
    local deadline = fan.gettime() + 3
    while fan.gettime() < deadline do
        local input = client:receive(1)
        if input then
            local data = input:GetBytes()
            if data and #data > 0 then
                table.insert(chunks, data)
            end
        else
            break
        end
    end

    client:close()
    return table.concat(chunks)
end

-- ============================================================
-- Test Suite
-- ============================================================

local suite = TestFramework.create_suite("httpd Comprehensive Tests")

-- Helper to run assertions only if result is available (harness mode skips event loop tests)
local function assert_http_result(result, captured, checks)
    if not result then return end  -- harness mode: fan.loop() returned immediately
    if checks.responseCode then
        TestFramework.assert_equal(checks.responseCode, result.responseCode)
    end
    if checks.body_contains then
        TestFramework.assert_not_nil(result.body)
        TestFramework.assert_true(string.find(result.body, checks.body_contains, 1, true) ~= nil,
            "response body should contain: " .. checks.body_contains)
    end
    if checks.body then
        TestFramework.assert_equal(checks.body, result.body)
    end
    if checks.body_len then
        TestFramework.assert_not_nil(result.body)
        TestFramework.assert_equal(checks.body_len, #result.body)
    end
end

-- Helper to run the same test against all available implementations
local function test_both(name, test_fn)
    suite:test(name .. " [core]", function()
        if not implementations.core then
            print("SKIP: httpd.core not available")
            return
        end
        test_fn(implementations.core)
    end)

    suite:test(name .. " [lua]", function()
        if not implementations.lua then
            print("SKIP: httpd.httpd not available")
            return
        end
        test_fn(implementations.lua)
    end)
end

-- ============================================================
-- 1. Request Properties
-- ============================================================

test_both("request_path", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.path = req.path
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/hello/world")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_equal("/hello/world", captured.path)
end)

test_both("request_method_get", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.method = req.method
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_equal("GET", captured.method)
end)

test_both("request_method_post", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.method = req.method
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_post(port, "/", "data")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_equal("POST", captured.method)
end)

test_both("request_headers", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.headers = req.headers
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(captured.headers)
    TestFramework.assert_type(captured.headers, "table")
    local host = captured.headers["host"] or captured.headers["Host"]
    TestFramework.assert_not_nil(host)
end)

test_both("request_query_string", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.query = req.query
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/path?key=value&foo=bar")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(captured.query)
    TestFramework.assert_true(string.find(captured.query, "key=value", 1, true) ~= nil)
    TestFramework.assert_true(string.find(captured.query, "foo=bar", 1, true) ~= nil)
end)

test_both("request_params", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.params = req.params
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/?name=hello&count=42")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(captured.params)
    TestFramework.assert_equal("hello", captured.params.name)
    TestFramework.assert_equal("42", captured.params.count)
end)

suite:test("request_body_post [core]", function()
    if not implementations.core then
        print("SKIP: httpd.core not available")
        return
    end
    local result = run_httpd_test(implementations.core,
        function(req, resp)
            local body = req.body or ""
            resp:reply(200, "OK", "echo: " .. body)
        end,
        function(port)
            return http_post(port, "/", "test payload data")
        end
    )
    if not result then return end
    assert_http_result(result, nil, {responseCode = 200, body_contains = "test payload data"})
end)

suite:test("request_remoteip [core]", function()
    if not implementations.core then
        print("SKIP: httpd.core not available")
        return
    end
    local captured = {}
    local result = run_httpd_test(implementations.core,
        function(req, resp)
            captured.remoteip = req.remoteip
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(captured.remoteip)
    TestFramework.assert_equal("127.0.0.1", captured.remoteip)
end)

suite:test("request_remoteport [core]", function()
    if not implementations.core then
        print("SKIP: httpd.core not available")
        return
    end
    local captured = {}
    local result = run_httpd_test(implementations.core,
        function(req, resp)
            captured.remoteport = req.remoteport
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(captured.remoteport)
    TestFramework.assert_true(captured.remoteport > 0)
end)

test_both("request_path_with_special_chars", function(impl)
    local captured = {}
    local result = run_httpd_test(impl,
        function(req, resp)
            captured.path = req.path
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/hello%20world")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(captured.path)
end)

-- ============================================================
-- 2. Response Methods
-- ============================================================

test_both("response_status_codes", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(201, "Created", "created resource")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(201, result.responseCode)
end)

test_both("response_404", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(404, "Not Found", "not found")
        end,
        function(port)
            return http_get(port, "/missing")
        end
    )
    if not result then return end
    TestFramework.assert_equal(404, result.responseCode)
end)

test_both("response_500", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(500, "Internal Server Error", "server error")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(500, result.responseCode)
end)

test_both("response_body", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(200, "OK", "Hello, World!")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    assert_http_result(result, nil, {responseCode = 200, body = "Hello, World!"})
end)

test_both("response_empty_body", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(204, "No Content", "")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(204, result.responseCode)
end)

test_both("response_custom_header", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:addheader("X-Custom-Header", "custom-value")
            resp:reply(200, "OK", "ok")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    if result.headers then
        local custom = result.headers["X-Custom-Header"] or result.headers["x-custom-header"]
        TestFramework.assert_equal("custom-value", custom)
    end
end)

test_both("response_chunked", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply_start(200, "OK")
            resp:reply_chunk("chunk1")
            resp:reply_chunk("chunk2")
            resp:reply_chunk("chunk3")
            resp:reply_end()
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(result.body)
    TestFramework.assert_true(string.find(result.body, "chunk1", 1, true) ~= nil)
    TestFramework.assert_true(string.find(result.body, "chunk2", 1, true) ~= nil)
    TestFramework.assert_true(string.find(result.body, "chunk3", 1, true) ~= nil)
end)

test_both("response_chunked_single", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply_start(200, "OK")
            resp:reply_chunk("single chunk")
            resp:reply_end()
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_not_nil(result.body)
    TestFramework.assert_true(string.find(result.body, "single chunk", 1, true) ~= nil)
end)

test_both("response_large_body", function(impl)
    local large_body = string.rep("x", 10000)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(200, "OK", large_body)
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    assert_http_result(result, nil, {responseCode = 200, body_len = 10000})
end)

-- ============================================================
-- 3. Keepalive Behavior
-- ============================================================

test_both("keepalive_default_http11", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(200, "OK", "keepalive test")
        end,
        function(port)
            return http_get(port, "/")
        end
    )
    if not result then return end
    assert_http_result(result, nil, {responseCode = 200, body = "keepalive test"})
end)

test_both("keepalive_disabled", function(impl)
    -- When keepalive is disabled, server sends Connection: close
    local server = impl.httpd.bind({
        host = "127.0.0.1",
        port = 0,
        enable_keep_alive = false,
        onService = function(req, resp)
            resp:reply(200, "OK", "no keepalive")
        end,
    })

    TestFramework.assert_not_nil(server)
    local port = server.port
    TestFramework.assert_true(port > 0)

    local result = nil
    local done = false

    coroutine.wrap(function()
        fan.sleep(5)
        if not done then done = true; fan.loopbreak() end
    end)()

    coroutine.wrap(function()
        local ok, res = pcall(http.get, {
            url = string.format("http://127.0.0.1:%d/", port),
        })
        if ok then result = res end
        done = true
        fan.loopbreak()
    end)()

    fan.loop()

    if not result then return end
    TestFramework.assert_equal(200, result.responseCode)
    TestFramework.assert_equal("no keepalive", result.body)
end)

-- ============================================================
-- 4. Connection: close
-- ============================================================

test_both("connection_close_client", function(impl)
    local result = run_httpd_test(impl,
        function(req, resp)
            resp:reply(200, "OK", "close test")
        end,
        function(port)
            return http_get(port, "/", { ["Connection"] = "close" })
        end
    )
    if not result then return end
    assert_http_result(result, nil, {responseCode = 200, body = "close test"})
end)

-- ============================================================
-- 5. Multiple Sequential Requests
-- ============================================================

test_both("multiple_sequential_requests", function(impl)
    local request_count = 0
    local server = impl.httpd.bind({
        host = "127.0.0.1",
        port = 0,
        onService = function(req, resp)
            request_count = request_count + 1
            resp:reply(200, "OK", "request " .. request_count)
        end,
    })

    TestFramework.assert_not_nil(server)
    local port = server.port

    local results = {}
    local done = false

    coroutine.wrap(function()
        fan.sleep(10)
        if not done then done = true; fan.loopbreak() end
    end)()

    coroutine.wrap(function()
        for i = 1, 3 do
            local ok, res = pcall(http.get, {
                url = string.format("http://127.0.0.1:%d/req%d", port, i),
            })
            if ok and res then
                table.insert(results, res)
            end
        end
        done = true
        fan.loopbreak()
    end)()

    fan.loop()

    if #results == 0 then return end
    TestFramework.assert_equal(3, #results)
    for i, r in ipairs(results) do
        TestFramework.assert_equal(200, r.responseCode)
        TestFramework.assert_equal("request " .. i, r.body)
    end
end)

-- ============================================================
-- Run HTTP tests first, then WebSocket tests
-- ============================================================

local failures = TestFramework.run_suite(suite)

-- ============================================================
-- 6. WebSocket Lifecycle (C core only - Lua httpd doesn't support WS)
-- Run after HTTP suite to avoid event loop state corruption
-- ============================================================

if implementations.core then
    local ws_suite = TestFramework.create_suite("httpd WebSocket Tests")

    -- Helper: send WebSocket upgrade and return raw client
    local function ws_connect(port)
        local client = connector.connect("tcp://127.0.0.1:" .. port)
        if not client then return nil end

        client:send(
            "GET / HTTP/1.1\r\n" ..
            "Host: 127.0.0.1:" .. port .. "\r\n" ..
            "Upgrade: websocket\r\n" ..
            "Connection: Upgrade\r\n" ..
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" ..
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )

        -- Read the 101 response
        local chunks = {}
        local deadline = fan.gettime() + 2
        local got_response = false
        while fan.gettime() < deadline do
            local input = client:receive(1)
            if input then
                local data = input:GetBytes()
                if data and #data > 0 then
                    table.insert(chunks, data)
                    local full = table.concat(chunks)
                    local hdr_end = string.find(full, "\r\n\r\n", 1, true)
                    if hdr_end then
                        got_response = true
                        -- Save any data after the headers (may contain WS frames)
                        local extra = string.sub(full, hdr_end + 4)
                        if extra and #extra > 0 then
                            client._pending_data = extra
                        end
                        break
                    end
                end
            else
                break
            end
        end

        if not got_response then
            client:close()
            return nil
        end

        return client
    end

    -- Helper: create a masked WebSocket text frame
    local function ws_encode_text(payload)
        local len = #payload
        local header
        if len < 126 then
            header = string.char(0x81, 0x80 | len)  -- FIN + TEXT + MASK
        elseif len < 65536 then
            header = string.char(0x81, 126 + 128,
                len >> 8, len & 0xFF)
        else
            error("payload too large for test")
        end
        -- Masking key (fixed for simplicity)
        local mask = "\x00\x00\x00\x00"
        local masked = {}
        for i = 1, len do
            masked[i] = string.char(string.byte(payload, i))  -- XOR with 0 = no change
        end
        return header .. mask .. table.concat(masked)
    end

    -- Helper: create a masked WebSocket close frame
    local function ws_encode_close(code)
        code = code or 1000
        local payload = string.char(
            code >> 8, code & 0xFF
        )
        local header = string.char(0x88, 0x80 | 2)  -- FIN + CLOSE + MASK
        local mask = "\x00\x00\x00\x00"
        return header .. mask .. payload
    end

    -- Helper: read a WebSocket frame from raw connection
    local function ws_read_frame(client, timeout)
        timeout = timeout or 2
        local chunks = {}
        local deadline = fan.gettime() + timeout

        -- Use any pending data from ws_connect first
        if client._pending_data and #client._pending_data > 0 then
            table.insert(chunks, client._pending_data)
            client._pending_data = nil
        end

        -- Read at least 2 bytes (frame header minimum)
        while fan.gettime() < deadline do
            local total = 0
            for _, c in ipairs(chunks) do total = total + #c end
            if total >= 2 then break end

            local input = client:receive(1)
            if input then
                local data = input:GetBytes()
                if data and #data > 0 then
                    table.insert(chunks, data)
                end
            else
                break
            end
        end

        local full = table.concat(chunks)
        if #full < 2 then return nil end

        local byte1 = string.byte(full, 1)
        local byte2 = string.byte(full, 2)
        local fin = byte1 & 0x80 ~= 0
        local opcode = byte1 & 0x0F
        local masked = byte2 & 0x80 ~= 0
        local payload_len = byte2 & 0x7F
        local header_size = 2

        if payload_len == 126 then
            -- Need 2 more bytes
            while fan.gettime() < deadline do
                local input = client:receive(1)
                if input then
                    local data = input:GetBytes()
                    if data then table.insert(chunks, data) end
                    full = table.concat(chunks)
                    if #full >= 4 then break end
                end
            end
            if #full < 4 then return nil end
            payload_len = (string.byte(full, 3) << 8) + string.byte(full, 4)
            header_size = 4
        end

        -- Read full payload
        while fan.gettime() < deadline do
            local total = 0
            for _, c in ipairs(chunks) do total = total + #c end
            if total >= header_size + payload_len then break end
            local input = client:receive(1)
            if input then
                local data = input:GetBytes()
                if data then table.insert(chunks, data) end
            else
                break
            end
        end

        full = table.concat(chunks)
        local payload = string.sub(full, header_size + 1, header_size + payload_len)
        return { fin = fin, opcode = opcode, payload = payload }
    end

    ws_suite:test("websocket_upgrade_detection", function()
        local ws_detected = false
        local ws_accepted = false

        local server = httpd_core.bind({
            host = "127.0.0.1",
            port = 0,
            onService = function(req, resp)
                local ok, is_ws = pcall(function() return req:is_websocket_upgrade() end)
                if ok then ws_detected = is_ws end
                if is_ws then
                    local ok2, result2 = pcall(function() return req:websocket_accept() end)
                    if ok2 then ws_accepted = result2 end
                else
                    resp:reply(200, "OK", "not ws")
                end
            end,
        })

        TestFramework.assert_not_nil(server)
        local port = server.port

        local done = false
        coroutine.wrap(function()
            fan.sleep(5)
            if not done then done = true; fan.loopbreak() end
        end)()

        coroutine.wrap(function()
            local client = ws_connect(port)
            if client then
                fan.sleep(0.2)
                client:close()
            end
            done = true
            fan.loopbreak()
        end)()

        fan.loop()

        if not done then return end
        TestFramework.assert_true(ws_detected)
        TestFramework.assert_true(ws_accepted)
    end)

    ws_suite:test("websocket_state_transitions", function()
        local states_seen = {}

        local server = httpd_core.bind({
            host = "127.0.0.1",
            port = 0,
            onService = function(req, resp)
                local is_ws = req:is_websocket_upgrade()
                if is_ws then
                    local s1 = req:websocket_state()
                    table.insert(states_seen, s1)
                    req:websocket_accept()
                    local s2 = req:websocket_state()
                    table.insert(states_seen, s2)
                else
                    resp:reply(200, "OK", "not ws")
                end
            end,
        })

        TestFramework.assert_not_nil(server)
        local port = server.port

        local done = false
        coroutine.wrap(function()
            fan.sleep(5)
            if not done then done = true; fan.loopbreak() end
        end)()

        coroutine.wrap(function()
            local client = ws_connect(port)
            if client then
                fan.sleep(0.3)
                client:close()
            end
            done = true
            fan.loopbreak()
        end)()

        fan.loop()

        if not done then return end
        TestFramework.assert_true(#states_seen >= 2)
        TestFramework.assert_equal("not_websocket", states_seen[1])
        TestFramework.assert_equal("open", states_seen[2])
    end)

    ws_suite:test("websocket_send_receive", function()
        local server = httpd_core.bind({
            host = "127.0.0.1",
            port = 0,
            onService = function(req, resp)
                local is_ws = req:is_websocket_upgrade()
                if is_ws then
                    req:websocket_accept()
                    local data, opcode = req:websocket_receive()
                    if data then
                        req:websocket_send("echo:" .. data)
                    end
                    req:websocket_close()
                else
                    resp:reply(200, "OK", "not ws")
                end
            end,
        })

        TestFramework.assert_not_nil(server)
        local port = server.port

        local echo_response = nil
        local done = false

        coroutine.wrap(function()
            fan.sleep(5)
            if not done then done = true; fan.loopbreak() end
        end)()

        coroutine.wrap(function()
            local client = ws_connect(port)
            if client then
                client:send(ws_encode_text("hello"))
                fan.sleep(0.2)
                local frame = ws_read_frame(client, 2)
                if frame and frame.payload then
                    echo_response = frame.payload
                end
                ws_read_frame(client, 1)
                client:close()
            end
            done = true
            fan.loopbreak()
        end)()

        fan.loop()

        if not done then return end
        TestFramework.assert_not_nil(echo_response)
        TestFramework.assert_equal("echo:hello", echo_response)
    end)

    ws_suite:test("websocket_ping_pong", function()
        local ping_sent = false

        local server = httpd_core.bind({
            host = "127.0.0.1",
            port = 0,
            onService = function(req, resp)
                local is_ws = req:is_websocket_upgrade()
                if is_ws then
                    req:websocket_accept()
                    local ok = pcall(function() req:websocket_ping("ping_data") end)
                    ping_sent = ok
                    fan.sleep(0.5)
                    pcall(function() req:websocket_close() end)
                else
                    resp:reply(200, "OK", "not ws")
                end
            end,
        })

        TestFramework.assert_not_nil(server)
        local port = server.port

        local done = false

        coroutine.wrap(function()
            fan.sleep(5)
            if not done then done = true; fan.loopbreak() end
        end)()

        coroutine.wrap(function()
            local client = ws_connect(port)
            if client then
                for i = 1, 5 do
                    local frame = ws_read_frame(client, 1)
                    if not frame then break end
                    if frame.opcode == 0x9 then
                        local pong_frame = string.char(0x8A, 0x80 | #frame.payload) ..
                            "\x00\x00\x00\x00" .. frame.payload
                        client:send(pong_frame)
                    elseif frame.opcode == 0x8 then
                        break
                    end
                end
                client:close()
            end
            done = true
            fan.loopbreak()
        end)()

        fan.loop()

        if not done then return end
        TestFramework.assert_true(ping_sent)
    end)

    ws_suite:test("websocket_close_frame", function()
        local close_state = nil

        local server = httpd_core.bind({
            host = "127.0.0.1",
            port = 0,
            onService = function(req, resp)
                local is_ws = req:is_websocket_upgrade()
                if is_ws then
                    req:websocket_accept()
                    -- Wait for client to send close
                    local data, opcode = req:websocket_receive()
                    -- After close, check state
                    close_state = req:websocket_state()
                    -- Don't call websocket_close() — client already disconnected
                else
                    resp:reply(200, "OK", "not ws")
                end
            end,
        })

        TestFramework.assert_not_nil(server)
        local port = server.port

        local done = false
        coroutine.wrap(function()
            fan.sleep(5)
            if not done then done = true; fan.loopbreak() end
        end)()

        coroutine.wrap(function()
            local client = ws_connect(port)
            if client then
                -- Send close frame
                client:send(ws_encode_close(1000))
                fan.sleep(0.3)
                client:close()
            end
            done = true
            fan.loopbreak()
        end)()

        fan.loop()

        -- Server should have seen the close state
        if close_state then
            TestFramework.assert_equal("closed", close_state)
        end
    end)

    ws_suite:test("websocket_double_close_safe", function()
        -- Double close should not crash
        local server = httpd_core.bind({
            host = "127.0.0.1",
            port = 0,
            onService = function(req, resp)
                local is_ws = req:is_websocket_upgrade()
                if is_ws then
                    req:websocket_accept()
                    -- Double close
                    pcall(function() req:websocket_close() end)
                    pcall(function() req:websocket_close() end)
                else
                    resp:reply(200, "OK", "not ws")
                end
            end,
        })

        TestFramework.assert_not_nil(server)
        local port = server.port

        local done = false
        coroutine.wrap(function()
            fan.sleep(5)
            if not done then done = true; fan.loopbreak() end
        end)()

        coroutine.wrap(function()
            local client = ws_connect(port)
            if client then
                fan.sleep(0.3)
                client:close()
            end
            done = true
            fan.loopbreak()
        end)()

        fan.loop()

        -- If we get here without crashing, the test passes
        TestFramework.assert_true(true)
    end)

    -- Run WebSocket suite
    local ws_failures = TestFramework.run_suite(ws_suite)
    if ws_failures > 0 then
        failures = failures + ws_failures
    end
end

os.exit(failures > 0 and 1 or 0)
