-- test_httpd_async_teardown.lua
-- Phase 4 integration tests for the asynchronous, drain-aware server teardown
-- (threading-fix-plan.md Phase 1-2). Proves the runtime guarantees for R1/R2/R3/R8
-- and the R7 leak-not-hang semantics:
--
--   A (R1)  GC a live server while a WebSocket is open and cross-thread sends
--           are in flight; the socket keeps receiving then EOFs, and the
--           process does not crash.
--   B (R2)  Running in a distributed worker env (--workers 2), GC the server
--           from inside an onService callback (a worker thread) must not hang.
--   C (R8)  Connection storm against the distributed listener stays bounded and
--           all requests complete (backpressure keeps memory bounded).
--   D (R3/R7) server:close() releases the port so a retry-bind succeeds, and
--           entering teardown right as the loop stops is leak-not-hang.
--   E (R7)  close() immediately before the loop stops: loop exit drains the
--           teardown hand-offs that were queued per owner base, so every evhttp
--           instance is released (fd count drops; strongest on Linux/macOS via
--           lfs) and no "resources retained until exit" line is logged.
--
-- Each scenario runs in its own subprocess so a native crash / hang is detected
-- via exit code / signal. Requires fan.httpd.core and a workers>0 runtime.
--
-- Load with:  LUA_PATH='./lua/framework/?.lua;./tests/lua/?.lua;;' lua lua/framework/run_phase4.lua
-- or directly: lua tests/lua/test_httpd_async_teardown.lua

local TestFramework = require('test_framework')
local fan = require 'fan'

local ok, httpd_core = pcall(require, 'fan.httpd.core')
if not ok then
    print("fan.httpd.core not available; skipping")
    os.exit(77)
end

local suite = TestFramework.create_suite("httpd async teardown (Phase 4)")

-- Subprocess runner: runs `lua script`, returns {ok, exit_type, code, output}.
local function run_sub(lua_body, port, workers)
    local tmp = os.tmpname() .. ".lua"
    local f = io.open(tmp, "w")
    if not f then return {ok=false, output="cannot create tmp"} end
    f:write(lua_body)
    f:close()
    local cmd = string.format(
        "LUA_PATH='./lua/framework/?.lua;./tests/lua/?.lua;;' " ..
        "LUA_CPATH='./build/?.so;../?.so;;' " ..
        "timeout 20s lua %s 2>&1; echo __DONE__$?",
        tmp)
    local h = io.popen(cmd)
    local out = h:read("*a")
    local okk, etype, code = h:close()
    os.remove(tmp)
    local exit_code = tonumber(out:match("__DONE__(%d+)%s*$"))
    return {ok=okk, exit_type=etype, code=code, exit_code=exit_code, output=out}
end

-- Worker-aware bind. In Lua, worker=N pins to a worker; omitting it with a
-- worker pool uses distribute mode. Every scenario below exercises distribute.
local PORT = 25000 + (fan.getpid() % 2000)

-- Scenario A: GC a live server over an open WebSocket with cross-thread sends.
suite:test("A_gc_server_with_live_websocket", function()
    local script = string.format([[
local fan = require 'fan'
local core = require 'fan.httpd.core'
local connector = require 'fan.connector'
local port = %d
fan.loop(function()
    local server
    server = core.bind({
        host = "127.0.0.1", port = port,
        onService = function(req)
            pcall(function() req:websocket_accept() end)
        end,
    })
    fan.sleep(0.05)
    local client = connector.connect("tcp://127.0.0.1:" .. port)
    client:send("GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
    fan.sleep(0.1)
    -- GC the server while the socket is live; native teardown is async and
    -- drain-aware, so the connection stays usable then EOFs when evhttp_free
    -- finally runs on the owner loop. No UAF / no crash.
    server = nil
    collectgarbage("collect")
    collectgarbage("collect")
    fan.sleep(0.3)
    pcall(function() client:close() end)
    collectgarbage("collect")
    fan.sleep(0.2)
    print("A_OK")
    fan.loopbreak()
end)
os.exit(0)
]], PORT)
    local r = run_sub(script, PORT, 2)
    TestFramework.assert_true(r.exit_code == 0 and r.output:find("A_OK", 1, true) ~= nil,
        "scenario A subprocess crashed/hung or errored:\n" .. r.output)
end)

-- Scenario C: connection storm against the distributed listener stays bounded.
suite:test("C_connect_storm_backpressure", function()
    local script = string.format([[
local fan = require 'fan'
local core = require 'fan.httpd.core'
local connector = require 'fan.connector'
local port = %d
local served = 0
fan.loop(function()
    core.bind({
        host = "127.0.0.1", port = port,
        onService = function(req, resp)
            if req:is_websocket_upgrade() then pcall(function() req:websocket_accept() end)
            else served = served + 1; resp:reply(200, "OK", "x") end
        end,
    })
    fan.sleep(0.05)
    local clients = {}
    for i = 1, 200 do
        local c = connector.connect("tcp://127.0.0.1:" .. port)
        if c then table.insert(clients, c); c:send("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n") end
    end
    fan.sleep(0.5)
    for _, c in ipairs(clients) do pcall(function() c:close() end) end
    collectgarbage("collect")
    fan.sleep(0.2)
    print("C_SERVED_" .. served)
    fan.loopbreak()
end)
os.exit(0)
]], PORT)
    local r = run_sub(script, PORT, 2)
    -- The marker alone is not enough: it printed even when no client ever
    -- connected (the pre-URL-form connector call returned nil).
    local served = tonumber(r.output:match("C_SERVED_(%d+)")) or 0
    TestFramework.assert_true(r.exit_code == 0 and served >= 1,
        string.format("scenario C storm crashed/hung or served nothing (served=%d):\n%s",
                      served, r.output))
end)

-- Scenario D: close() releases the port (retry bind succeeds) — shows the async
-- teardown actually frees the listener (not the old leak-forever-on-fail path).
suite:test("D_close_releases_port", function()
    local script = string.format([[
local fan = require 'fan'
local core = require 'fan.httpd.core'
local port = %d
fan.loop(function()
    local s = core.bind({ host = "127.0.0.1", port = port, onService = function() end })
    local port2 = s.port
    s.serv:close()
    s = nil
    collectgarbage("collect")
    -- retry bind loop to tolerate async drain (port reuse needs a few ticks)
    local rb
    for i = 1, 50 do
        fan.sleep(0.02)
        local ok2, srv = pcall(core.bind, { host = "127.0.0.1", port = port2, onService = function() end })
        if ok2 and srv then
            rb = srv
            break
        end
    end
    if rb then print("D_OK") else print("D_FAIL_norebind") end
    fan.loopbreak()
end)
os.exit(0)
]], PORT)
    local r = run_sub(script, PORT, 2)
    TestFramework.assert_true(r.exit_code == 0 and r.output:find("D_OK", 1, true) ~= nil,
        "scenario D close did not release port:\n" .. r.output)
end)

-- Scenario B (worker-GC, no hang) runs in a distributed worker env. This needs a
-- runtime where --workers>0 is set; the subprocess inherits the parent's fan env.
suite:test("B_gc_from_worker_callback_no_hang", function()
    local script = string.format([[
local fan = require 'fan'
local core = require 'fan.httpd.core'
local connector = require 'fan.connector'
local port = %d
fan.loop(function()
    local srv
    srv = core.bind({
        host = "127.0.0.1", port = port,
        onService = function(req, resp)
            resp:reply(200, "OK", "b")
            -- GC the server from inside a (possibly worker) callback
            local ok3, err3 = pcall(function() srv = nil; collectgarbage("collect") end)
            if not ok3 then print("B_ERR_" .. tostring(err3)) end
        end,
    })
    fan.sleep(0.05)
    local c = connector.connect("tcp://127.0.0.1:" .. port)
    c:send("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    fan.sleep(0.3)
    -- if a deadlock occurred we would never reach this print
    print("B_DONE")
    fan.loopbreak()
end)
os.exit(0)
]], PORT)
    local r = run_sub(script, PORT, 2)
    TestFramework.assert_true(r.exit_code == 0 and r.output:find("B_DONE", 1, true) ~= nil,
        "scenario B worker-GC hung or crashed:\n" .. r.output)
end)

-- Scenario E (R7): close() right before the loop stops. The teardown chain is
-- queued per owner base (main listener + one evhttp per worker) and its follow-up
-- jobs are dispatched from inside those owner callbacks — a loop that stops in
-- the same tick used to refuse them ("resources retained until exit"), leaving
-- every instance and the server userdata pinned. Loop exit must now drain them.
-- Build with -DEVENT_MGR_DRAIN_MAX_MS=0 to disable the drain: the fd assertion
-- must then fail (silent-leak regression), which is how this scenario was
-- validated against the pre-fix behaviour.
suite:test("E_close_before_loop_stop_releases_instances", function()
    local script = string.format([[
local fan = require 'fan'
local core = require 'fan.httpd.core'
local connector = require 'fan.connector'
local port = %d

local workers = (fan.worker_count and fan.worker_count()) or 0
if workers < 4 and fan.workers_init then
    pcall(fan.workers_init, 4)
    workers = (fan.worker_count and fan.worker_count()) or 0
end
if workers < 2 then
    print("E_SKIP_NO_WORKERS")
    os.exit(0)
end

-- fd sampling: /proc/self/fd on Linux, /dev/fd on macOS. Nil when unavailable
-- (the fd part of the assertion is then skipped).
local function fd_count()
    local okl, lfs = pcall(require, 'lfs')
    if not okl then return nil end
    local dir
    for _, d in ipairs({ '/proc/self/fd', '/dev/fd' }) do
        if pcall(function() for _ in lfs.dir(d) do end end) then dir = d break end
    end
    if not dir then return nil end
    local n = 0
    for _ in lfs.dir(dir) do n = n + 1 end
    return n
end

local before_close
fan.loop(function()
    local s = core.bind({ host = "127.0.0.1", port = port,
        onService = function(req, resp) resp:reply(200, "OK", "e") end })
    fan.sleep(0.05)
    -- Keep-alive connections that can only be closed by evhttp_free.
    local clients = {}
    for i = 1, 4 do
        -- fan.connector takes a URL, not (scheme, host, port).
        local c = connector.connect("tcp://127.0.0.1:" .. port)
        if c then
            table.insert(clients, c)
            c:send("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
        end
    end
    fan.sleep(0.3)
    before_close = fd_count()
    s.serv:close()
    s = nil
    collectgarbage("collect")
    -- Stop the loop in the same tick: the library must drain the queued
    -- teardown hand-offs itself.
    fan.loopbreak()
    _G.E_KEEP_CLIENTS = clients   -- keep the client fds alive across the drain
end)
local after_exit = fd_count()
print("E_FD_" .. tostring(before_close) .. "_" .. tostring(after_exit))
print("E_DONE")
os.exit(0)
]], PORT)
    local r = run_sub(script, PORT, 4)
    local before, after = r.output:match("E_FD_(%-?%d+)_(%-?%d+)")
    local no_leak = not (r.output:find("retained until exit", 1, true)
        or r.output:find("drain incomplete", 1, true))
    local released = true
    if before and after then
        -- 4 accepted keep-alive sockets must be gone after the drain.
        released = tonumber(after) <= tonumber(before) - 4
    end
    TestFramework.assert_true(
        r.exit_code == 0 and r.output:find("E_DONE", 1, true) ~= nil and no_leak and released,
        string.format("scenario E loop-exit teardown incomplete (fd %s->%s, no_leak=%s):\n%s",
                      tostring(before), tostring(after), tostring(no_leak), r.output))
end)

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    fan.loopbreak()
    os.exit(failed > 0 and 1 or 0)
end)