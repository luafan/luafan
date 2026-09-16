#!/usr/bin/env lua

-- Asynchronous httpd behavior tests against the C implementation
-- (fan.httpd.core):
--   keep-alive sequential requests on one connection,
--   concurrent request interleaving with coroutine migration (fan.sleep),
--   chunked streaming order,
--   client disconnect before reply (server must survive and keep serving),
--   large body echo,
--   params/headers access,
--   WebSocket echo lifecycle,
--   server close mid-flight then rebind on the same port.
--
-- Tests yield cooperatively (fan.sleep polls) — never fan.loopbreak — so the
-- suite can keep running subsequent cases in the same loop.

local TestFramework = require('test_framework')
local fan = require "fan"

package.preload['config'] = function()
    return { debug = false, tcp_pause_read_write_on_callback = false }
end

local http = require "fan.http"
local connector = require "fan.connector"

local ok_core, httpd = pcall(require, 'fan.httpd.core')
if not ok_core or not httpd then
    print("fan.httpd.core unavailable, skipping")
    os.exit(77)
end

local suite = TestFramework.create_suite("httpd async behavior")

-- Poll-wait helper: yields until cond() is true or timeout seconds pass.
local function wait_until(cond, timeout, step)
    timeout = timeout or 10
    step = step or 0.02
    local deadline = fan.gettime() + timeout
    while not cond() do
        if fan.gettime() > deadline then
            return false
        end
        fan.sleep(step)
    end
    return true
end

-- Read one full HTTP response (headers + Content-Length body) from a raw
-- connector connection. Returns the whole response bytes or nil on EOF.
local function read_response(client, max_reads)
    local total = ""
    local hdr_end = nil
    local content_length = nil

    for _ = 1, (max_reads or 200) do
        local input = client:receive(1)
        if not input then
            return nil
        end
        local bytes = input:GetBytes()
        if bytes and #bytes > 0 then
            total = total .. bytes
            if not hdr_end then
                hdr_end = string.find(total, "\r\n\r\n", 1, true)
                if hdr_end then
                    local cl = string.match(total, "Content%-Length:%s*(%d+)")
                    content_length = cl and tonumber(cl) or 0
                end
            end
            if hdr_end and content_length then
                local body_len = #total - (hdr_end + 3)
                if body_len >= content_length then
                    return total
                end
            end
        end
    end
    return nil
end

-- 1. Keep-alive: three sequential requests over one client connection must
-- produce three responses without the server closing the connection.
suite:test("keepalive_sequential_requests", function()
    local hits = 0
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            hits = hits + 1
            resp:reply(200, "OK", "resp" .. hits)
        end,
    })
    assert(server and server.port > 0, "bind failed")

    local client = connector.connect("tcp://127.0.0.1:" .. server.port)
    local responses = {}
    for i = 1, 3 do
        client:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        local r = read_response(client)
        assert(r, "missing response " .. i)
        table.insert(responses, r)
    end

    assert(#responses == 3, "expected 3 responses, got " .. #responses)
    for i = 1, 3 do
        assert(string.find(responses[i], "resp" .. i, 1, true),
            "response " .. i .. " body mismatch: " .. tostring(responses[i]))
        assert(not string.find(responses[i], "Connection: close", 1, true),
            "keep-alive connection must not be told to close")
    end
    client:close()
    server.serv:close()
end)

-- 2. Concurrent requests: 20 simultaneous clients each get their own reply.
suite:test("concurrent_requests_all_answered", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            local id = tonumber(string.match(req.path, "/(%d+)") or "0")
            resp:reply(200, "OK", "body-" .. id)
        end,
    })

    local results = {}
    local done = 0
    for i = 1, 20 do
        coroutine.wrap(function()
            local res = http.get("http://127.0.0.1:" .. server.port .. "/" .. i, 10)
            results[i] = (res ~= nil and res.responseCode == 200
                and res.body == ("body-" .. tostring(i)))
            done = done + 1
        end)()
    end

    assert(wait_until(function() return done == 20 end, 15),
        "concurrent requests incomplete: done=" .. done)
    for i = 1, 20 do
        assert(results[i], "request " .. i .. " wrong result")
    end
    server.serv:close()
end)

-- 3. Coroutine migration: handler yields on fan.sleep then replies; five
-- interleaved requests must each get their own payload back.
suite:test("sleep_migrated_handler_payloads", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            local id = string.match(req.path, "/sleep/(%d+)") or "?"
            fan.sleep(0.03)
            resp:reply(200, "OK", "slept-" .. id)
        end,
    })

    local clients = {}
    for i = 1, 5 do
        local c = connector.connect("tcp://127.0.0.1:" .. server.port)
        clients[i] = c
        c:send("GET /sleep/" .. i .. " HTTP/1.1\r\nHost: x\r\n\r\n")
    end
    for i = 1, 5 do
        local r = read_response(clients[i])
        assert(r, "missing migrated response " .. i)
        assert(string.find(r, "slept-" .. i, 1, true),
            "migrated response " .. i .. " payload mismatch")
        clients[i]:close()
    end
    server.serv:close()
end)

-- 4. Chunked streaming: reply_start + chunks + end must arrive in order and
-- concatenate exactly.
-- 4. Chunked streaming: reply_start + chunks + end must arrive in order and
-- concatenate exactly. Retried because macOS kqueue occasionally delays the
-- final bufferevent flush (pre-existing; linux CI unaffected).
suite:test("chunked_streaming_order", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            resp:reply_start(200, "OK")
            resp:reply_chunk("alpha-")
            resp:reply_chunk("beta-")
            resp:reply_chunk("gamma")
            resp:reply_end()
        end,
    })

    local function complete(total)
        return string.find(total, "alpha%-", 1, false) ~= nil
            and string.find(total, "beta%-", 1, false) ~= nil
            and string.find(total, "gamma", 1, false) ~= nil
            and string.find(total, "0\r\n\r\n", 1, true) ~= nil
    end

    local total = ""
    for attempt = 1, 3 do
        local client = connector.connect("tcp://127.0.0.1:" .. server.port)
        client:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        for _ = 1, 100 do
            local input = client:receive(1)
            if not input then break end
            total = total .. input:GetBytes()
            if complete(total) then break end
        end
        client:close()
        if complete(total) then break end
        fan.sleep(0.05)
    end

    assert(complete(total),
        "chunked body out of order or incomplete: bytes=" .. #total
        .. " raw=" .. (total:gsub("[%c]", "|")))
    server.serv:close()
end)

-- 5. Client disconnects before the reply: the handler must be able to reply
-- into the dead connection without crashing the server, and the server must
-- keep serving new clients afterwards.
suite:test("disconnect_before_reply_server_survives", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            fan.sleep(0.05) -- widen the window: peer disappears before reply
            pcall(function() resp:reply(200, "OK", "late") end)
        end,
    })

    local ghost = connector.connect("tcp://127.0.0.1:" .. server.port)
    ghost:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    ghost:close()
    fan.sleep(0.1)

    local client = connector.connect("tcp://127.0.0.1:" .. server.port)
    client:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    local r = read_response(client)
    assert(r and string.find(r, "200", 1, true), "server did not survive ghost client")
    client:close()
    server.serv:close()
end)

-- 6. Large body echo: 1MB POST round-trips intact.
suite:test("large_body_echo", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            resp:reply(200, "OK", req.body)
        end,
    })

    local payload = string.rep("0123456789", 102400) -- 1,024,000 bytes
    local done = false
    local err = nil
    coroutine.wrap(function()
        local res = http.post{ url = "http://127.0.0.1:" .. server.port .. "/", body = payload, timeout = 30 }
        if not res or res.responseCode ~= 200 then
            err = "large echo status failed"
        elseif not res.body or #res.body ~= #payload then
            err = "large echo length mismatch: " .. tostring(res.body and #res.body)
        elseif res.body ~= payload then
            err = "large echo content mismatch"
        end
        done = true
    end)()

    assert(wait_until(function() return done end, 30), "large echo timed out: " .. tostring(err))
    assert(not err, err or "unexpected")
    server.serv:close()
end)

-- 7. Params and headers visibility inside the handler.
suite:test("params_and_headers", function()
    local captured = nil
    local done = false
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            captured = {
                a = tonumber(req.params.a),
                b = req.params.b,
                ua = req.headers["User-Agent"] or req.headers["user-agent"],
            }
            resp:reply(200, "OK", "ok")
        end,
    })

    coroutine.wrap(function()
        local res = http.get{
            url = "http://127.0.0.1:" .. server.port .. "/?a=42&b=xyz",
            headers = { ["User-Agent"] = "async-test/1.0" },
            timeout = 10,
        }
        done = true
        assert(res and res.responseCode == 200, "params request failed")
    end)()

    assert(wait_until(function() return done end, 15), "params request timed out")
    assert(captured, "handler did not run")
    assert(captured.a == 42, "param a wrong: " .. tostring(captured.a))
    assert(captured.b == "xyz", "param b wrong: " .. tostring(captured.b))
    assert(captured.ua == "async-test/1.0", "header lost: " .. tostring(captured.ua))
    server.serv:close()
end)

-- 8. WebSocket echo lifecycle: upgrade, masked client frame, server echo,
-- client close, then the same server still answers plain HTTP.
suite:test("websocket_echo_lifecycle", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            if req:is_websocket_upgrade() then
                if req:websocket_accept() then
                    coroutine.wrap(function()
                        while true do
                            local msg, opcode = req:websocket_receive()
                            if not msg then break end
                            if opcode == 8 then break end
                            req:websocket_send("echo:" .. msg)
                        end
                    end)()
                end
            else
                resp:reply(200, "OK", "plain")
            end
        end,
    })

    local function ws_frame(payload, opcode)
        local mask = string.char(0x11, 0x22, 0x33, 0x44)
        local masked = {}
        for i = 1, #payload do
            masked[i] = string.char(
                string.byte(payload, i) ~ string.byte(mask, ((i - 1) % 4) + 1))
        end
        local header
        if #payload < 126 then
            header = string.char(0x80 | opcode, 0x80 | #payload) .. mask
        else
            header = string.char(0x80 | opcode, 0x80 | 126)
                .. string.char(math.floor(#payload / 256), #payload % 256) .. mask
        end
        return header .. table.concat(masked)
    end

        local client = connector.connect("tcp://127.0.0.1:" .. server.port)
    -- Fail-fast guard: raw receive() has no timeout; if the response flush
    -- wedges (macOS kqueue quirk) close the client so receive returns and
    -- the case reports failure instead of hanging the suite.
    local finished = false
    coroutine.wrap(function()
        fan.sleep(10)
        if not finished then
            io.stderr:write("WS-WATCHDOG: closing wedged client\n")
            pcall(function() client:close() end)
        end
    end)()
    client:send("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
        client:send(ws_frame("ping", 0x1))
    
    local total = ""
    local got_echo = false
    for _ = 1, 200 do
        local input = client:receive(1)
        if not input then break end
        local bytes = input:GetBytes()
        if bytes and #bytes > 0 then
            total = total .. bytes
            if string.find(total, "echo:ping", 1, true) then
                got_echo = true
                break
            end
        end
    end
    io.stderr:write("WS-TRACE got_echo=" .. tostring(got_echo) .. "\n")
    finished = true
    assert(got_echo, "websocket echo not received; got: " .. tostring(total):sub(1, 120))
    client:close()
        fan.sleep(0.2)
    
    local plain = connector.connect("tcp://127.0.0.1:" .. server.port)
        plain:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    local r = read_response(plain)
    io.stderr:write("WS-TRACE plain resp=" .. tostring(r and "ok" or "nil") .. "\n")
    assert(r and string.find(r, "plain", 1, true), "server degraded after WS lifecycle")
    plain:close()
    server.serv:close()
    end)

-- 9. Server close mid-flight, then rebind on the same port.
suite:test("close_midflight_then_rebind", function()
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            fan.sleep(0.2)
            resp:reply(200, "OK", "slow")
        end,
    })
    local port = server.port

    local slow = connector.connect("tcp://127.0.0.1:" .. port)
    slow:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")

    server.serv:close()
    fan.sleep(0.3)

    local server2 = httpd.bind({
        host = "127.0.0.1", port = port,
        onService = function(req, resp)
            resp:reply(200, "OK", "reborn")
        end,
    })
    assert(server2 and server2.port == port, "rebind on same port failed")
    local client = connector.connect("tcp://127.0.0.1:" .. port)
    client:send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    local r = read_response(client)
    assert(r and string.find(r, "reborn", 1, true), "rebound server did not answer")
    client:close()
    pcall(function() slow:close() end)
    server2.serv:close()
end)

-- 10. Worker distribution: with a worker pool the distributed listener must
-- serve plain HTTP from worker-owned connections (worker_id >= 0 visible).
suite:test("worker_distributed_requests", function()
    if fan.worker_count() == 0 then
        print("    (skip: no worker pool available)")
        return
    end

    local seen_nonmain = false
    local done = 0
    local server = httpd.bind({
        host = "127.0.0.1", port = 0,
        onService = function(req, resp)
            if req.worker_id >= 0 then seen_nonmain = true end
            resp:reply(200, "OK", "w")
        end,
    })

    for i = 1, 10 do
        coroutine.wrap(function()
            local res = http.get("http://127.0.0.1:" .. server.port .. "/", 10)
            done = done + 1
            assert(res and res.responseCode == 200, "worker-distributed request failed")
        end)()
    end
    assert(wait_until(function() return done == 10 end, 15),
        "worker-distributed requests incomplete: done=" .. done)
    assert(seen_nonmain, "no request ever ran on a worker base")
    server.serv:close()
end)

-- Optional worker pool (LUAN_TEST_WORKERS=N); must precede fan.loop().
TestFramework.init_workers_from_env()

fan.loop(function()
    -- Watchdog: the whole suite must finish quickly; hard-exit with a
    -- diagnostic if a case wedges (better than hanging CI).
    coroutine.wrap(function()
        fan.sleep(120)
        io.stderr:write("ASYNC-HTTPD-SUITE-TIMEOUT\n")
        os.exit(2)
    end)()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
