#!/usr/bin/env lua

-- Asynchronous tcpd behavior tests.
--
-- Server-side read callbacks require the accept handshake: the server's
-- onaccept(apt) must call apt:bind{onread=..., ondisconnected=...} per
-- connection (reads are deferred until bind enables EV_READ -- see
-- tcpd_accept_bind). These tests exercise that full async handshake.
--
--   echo under concurrent clients,
--   sequential connect/teardown churn,
--   server-side and client-side disconnect notification,
--   large payload integrity across many reads,
--   send from inside onaccept,
--   worker-affinity server with main-thread clients,
--   multiple servers on distinct ports.

local TestFramework = require('test_framework')
local fan = require "fan"
local tcpd = require "fan.tcpd"

local suite = TestFramework.create_suite("tcpd async behavior")

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

local function close_server(srv)
    if type(srv) == "userdata" then
        pcall(function() srv:close() end)
    end
end

local function bind_server(opts)
    local srv = tcpd.bind(opts)
    local info = srv and srv:localinfo()
    return srv, info and info.port
end

-- 1. Echo server: 8 concurrent clients, each with its own payload.
suite:test("echo_concurrent_clients", function()
    local received = {}
    local server = tcpd.bind({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function(conn, buf)
                    conn:send(buf)
                end,
            }
        end,
    })
    local port = server and server:localinfo().port
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port) .. " server=" .. tostring(server))

    local clients = {}
    for i = 1, 8 do
        coroutine.wrap(function()
            local conn = tcpd.connect({
                host = "127.0.0.1", port = port,
                onread = function(_, buf)
                    received[i] = (received[i] or "") .. tostring(buf)
                end,
                ondisconnected = function() end,
            })
            assert(conn, "connect failed")
            clients[i] = conn  -- hold: GC would close the connection otherwise
            conn:send("payload-" .. i .. "-" .. string.rep("x", i * 100))
        end)()
    end

    local function all_done()
        for i = 1, 8 do
            local want = 10 + i * 100
            if not received[i] or #received[i] < want then
                return false
            end
        end
        return true
    end
    if not wait_until(all_done, 15) then
        for i = 1, 8 do
            print("DIAG client", i, "recv_len=", received[i] and #received[i] or "nil")
        end
        error("echo incomplete", 0)
    end
    for i = 1, 8 do
        local expected = "payload-" .. i .. "-" .. string.rep("x", i * 100)
        assert(received[i] == expected,
            "client " .. i .. " echo mismatch: got " .. #tostring(received[i]) .. " bytes")
        clients[i]:close()
    end
    close_server(server)
end)

-- 2. Churn: 5 sequential connect -> one message -> close cycles.
suite:test("sequential_connect_churn", function()
    local accepted = 0
    local disconnected = 0
    local messages = {}
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            accepted = accepted + 1
            apt:bind{
                onread = function(_, buf) table.insert(messages, tostring(buf)) end,
                ondisconnected = function() disconnected = disconnected + 1 end,
            }
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port))

    for i = 1, 5 do
        local conn = tcpd.connect({
            host = "127.0.0.1", port = port,
            onread = function() end,
            ondisconnected = function() end,
        })
        assert(conn, "connect " .. i .. " failed")
        conn:send("msg-" .. i)
        fan.sleep(0.05)
        conn:close()
    end

    assert(wait_until(function() return #messages == 5 end, 10),
        "churn messages incomplete: " .. #messages)
    for i = 1, 5 do
        assert(messages[i] == "msg-" .. i, "churn message " .. i .. " wrong: "
            .. tostring(messages[i]))
    end
    assert(wait_until(function() return disconnected == 5 end, 10),
        "churn disconnects incomplete: " .. disconnected)
    close_server(server)
end)

-- 3. Server-side close mid-stream: client's ondisconnected must fire.
suite:test("server_close_notifies_client", function()
    local client_disconnected = false
    local server_conn = nil
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function() end,
                ondisconnected = function() end,
            }
            server_conn = apt
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port))

    local client = tcpd.connect({
        host = "127.0.0.1", port = port,
        onread = function() end,
        ondisconnected = function() client_disconnected = true end,
    })
    assert(client, "connect failed")
    client:send("hello")

    assert(wait_until(function() return server_conn ~= nil end, 10),
        "server never accepted")
    server_conn:close()

    assert(wait_until(function() return client_disconnected end, 10),
        "client was not notified of server-side close")
    close_server(server)
end)

-- 4. Client-side close: server's ondisconnected fires for that connection.
suite:test("client_close_notifies_server", function()
    local server_disconnected = 0
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function() end,
                ondisconnected = function() server_disconnected = server_disconnected + 1 end,
            }
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port))

    local client = tcpd.connect({
        host = "127.0.0.1", port = port,
        onread = function() end,
        ondisconnected = function() end,
    })
    assert(client, "connect failed")
    client:send("bye")
    fan.sleep(0.05)
    client:close()

    assert(wait_until(function() return server_disconnected == 1 end, 10),
        "server was not notified of client close")
    close_server(server)
end)

-- 5. Large payload integrity: 512KB crosses in many TCP segments and is
-- reassembled byte-exact.
suite:test("large_payload_integrity", function()
    local total = 0
    local body = ""
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function(_, buf)
                    total = total + #tostring(buf)
                    body = body .. tostring(buf)
                end,
            }
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port))

    local payload = string.rep("ABCDEFGH", 64 * 1024) -- 512KB
    local client = tcpd.connect({
        host = "127.0.0.1", port = port,
        onread = function() end,
        ondisconnected = function() end,
    })
    assert(client, "connect failed")
    local client_ref = client  -- hold: GC would close the connection otherwise
    -- chunked sends to exercise the write path repeatedly
    for i = 1, 8 do
        client:send(string.sub(payload, (i - 1) * 65536 + 1, i * 65536))
    end

    assert(wait_until(function() return total >= 512 * 1024 end, 20),
        "large payload incomplete: " .. total)
    assert(body == payload, "large payload content mismatch")
    client:close()
    close_server(server)
end)

-- 6. Send from inside onaccept: must reach the client.
suite:test("send_from_onaccept", function()
    local got_welcome = false
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(self, apt)
            apt:bind{
                onread = function() end,
                ondisconnected = function() end,
            }
            apt:send("welcome")
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port))

    local client = tcpd.connect({
        host = "127.0.0.1", port = port,
        onread = function(_, buf)
            if string.find(tostring(buf), "welcome", 1, true) then
                got_welcome = true
            end
        end,
        ondisconnected = function() end,
    })
    assert(client, "connect failed")

    assert(wait_until(function() return got_welcome end, 10),
        "onaccept send never arrived")
    client:close()
    close_server(server)
end)

-- 7. Worker-affinity server, main-thread clients: callbacks run on the worker.
suite:test("worker_affinity_server", function()
    if fan.worker_count() == 0 then
        print("    (skip: no worker pool available)")
        return
    end

    local echoed = false
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        worker = 1,
        onaccept = function(self, apt)
            apt:bind{
                onread = function(conn, buf)
                    conn:send(buf)
                end,
                ondisconnected = function() end,
            }
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: port=" .. tostring(port))

    local client = tcpd.connect({
        host = "127.0.0.1", port = port,
        onread = function(_, buf)
            if tostring(buf) == "affinity" then echoed = true end
        end,
        ondisconnected = function() end,
    })
    assert(client, "connect failed")
    client:send("affinity")

    assert(wait_until(function() return echoed end, 10),
        "worker-affinity echo never arrived")
    client:close()
    close_server(server)
end)

-- 8. Two servers on distinct ports are independent.
suite:test("two_servers_independent", function()
    local got_a, got_b = false, false
    local mk = function(tag)
        local srv, p = bind_server({
            host = "127.0.0.1", port = 0,
            onaccept = function(self, apt)
                apt:bind{
                    onread = function(_, buf)
                        if tostring(buf) == tag then
                            if tag == "A" then got_a = true else got_b = true end
                        end
                    end,
                }
            end,
        })
        assert(type(p) == "number", "bind failed for " .. tag .. ": p=" .. tostring(p))
        return srv, p
    end
    local srv_a, a = mk("A")
    local srv_b, b = mk("B")
    assert(a ~= b, "two servers should bind distinct ports")

    local ca = tcpd.connect({ host = "127.0.0.1", port = a,
        onread = function() end, ondisconnected = function() end })
    local cb = tcpd.connect({ host = "127.0.0.1", port = b,
        onread = function() end, ondisconnected = function() end })
    ca:send("A")
    cb:send("B")

    assert(wait_until(function() return got_a and got_b end, 10),
        "both servers did not receive their own message")
    ca:close()
    cb:close()
    close_server(srv_a)
    close_server(srv_b)
end)

-- 9. onaccept arity contract. Every accept must hand the callback
-- (server, accept) -- always arity 2 under callback_self_first. The documented
-- failed-accept path (kernel hand-over that cannot be set up locally) hands
-- (server, nil); it must never deliver a bare nil in first position, which
-- would read as self == nil and make `accept:bind{...}` raise "attempt to
-- index a nil value". Rapid connect/teardown churn is the pattern that reaches
-- that path, so this case doubles as the trigger.
--
-- The failed-accept branch itself is validated with a scratch build that forces
-- the path (mirrors test_httpd_async_teardown.lua scenario E):
--   cmake -S . -B /tmp/inject -DCMAKE_C_FLAGS=-DTCPD_ACCEPT_FAIL_INJECT_EVERY=1
--   LUA_CPATH='/tmp/inject/?.so;;' lua lua/test_async_tcpd.lua
-- With injection, failed_accept > 0 and this case must still pass (self never
-- nil, arity always 2); the pre-fix code delivered a single nil, i.e. self nil.
suite:test("onaccept_arity_contract", function()
    local calls, binds, failed_accept, self_missing, bad_arity = 0, 0, 0, 0, 0
    local server, port = bind_server({
        host = "127.0.0.1", port = 0,
        onaccept = function(...)
            local argc = select('#', ...)
            local srv, apt = ...
            calls = calls + 1
            if argc ~= 2 then
                bad_arity = bad_arity + 1
            end
            if srv == nil then
                self_missing = self_missing + 1
            end
            if apt == nil then
                failed_accept = failed_accept + 1   -- expected return value
            else
                binds = binds + 1
                apt:bind{
                    onread = function(conn, buf) conn:send(buf) end,
                    ondisconnected = function() end,
                }
            end
        end,
    })
    assert(type(port) == "number" and port > 0, "tcpd bind failed: " .. tostring(port))

    local clients = {}
    for i = 1, 40 do
        local conn = tcpd.connect({
            host = "127.0.0.1", port = port,
            onread = function() end,
            ondisconnected = function() end,
        })
        if conn then
            clients[i] = conn
            conn:send("arity-" .. i)
        end
    end
    fan.sleep(0.02)
    for i = 1, 40 do
        if clients[i] then pcall(function() clients[i]:close() end) end
    end
    fan.sleep(0.3)

    assert(calls > 0, "onaccept never ran")
    assert(bad_arity == 0,
        "onaccept arity was not 2 in " .. bad_arity .. " call(s)")
    assert(self_missing == 0,
        "onaccept received self == nil " .. self_missing .. " time(s)")
    assert(binds > 0, "no accepted connection was ever bound")
    print("    (accepts=" .. calls .. " bound=" .. binds
        .. " failed_accept=" .. failed_accept .. ")")
    close_server(server)
end)

-- Optional worker pool (LUAN_TEST_WORKERS=N); must precede fan.loop().
TestFramework.init_workers_from_env()

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    os.exit(failed > 0 and 1 or 0)
end)
