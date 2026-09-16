#!/usr/bin/env lua

-- test_cross_worker_combinations.lua
--
-- Cross-worker CONCURRENCY matrix covering every luafan async transport at
-- once: tcpd, udpd, httpd (fan.httpd.core) and fifo.
--
-- Single-transport suites live in test_async_*.lua / test_stress_*.lua and
-- test_httpd_reply_from_other_worker.lua. This file covers the COMBINATIONS:
--
--   1. tcpd  worker-pinned echo listener  <-> concurrent clients (main base)
--   2. udpd  worker-pinned echo socket    <-> concurrent senders (main base)
--   3. httpd -> tcpd : the handler opens a client pinned to ITS OWN worker
--      (req.worker_id) so the roundtrip is serviced while the handler sleeps.
--   4. httpd -> udpd : same pinning rule for a datagram roundtrip.
--   5. httpd -> tcpd + udpd -> fifo wake -> reply from a DIFFERENT worker.
--      The handler parks `resp` and wakes a FIFO reader pinned to
--      target=(owner+1) % WORKERS; that worker's onread runs on its own
--      thread, so resp:reply() is marshaled back by httpd_reply_drain_cb.
--   6. a concurrent burst of path 5, covering several owner workers and
--      several reply workers simultaneously.
--
-- The difference between 3/4/5 and the existing cross-worker test: here a
-- single request touches tcpd + udpd + fifo + httpd, and every hop is
-- explicitly pinned so it is serviced on the worker that owns the request.
--
-- All threads share one Lua state, so this requires the locked Lua build
-- (fan_lua_lock.h force-included into both the interpreter and fan.so) plus a
-- worker pool. workers_init() must run before fan.loop():
--
--   cd tests && LUAN_TEST_WORKERS=4 lua lua/test_cross_worker_combinations.lua
--
-- Exit codes: 0 = pass, 1 = fail, 77 = skipped.

local fan = require 'fan'

local failed = false
local function check(name, cond, extra)
    if cond then
        print("PASS " .. name)
    else
        print("FAIL " .. name .. (extra ~= nil and (": " .. tostring(extra)) or ""))
        failed = true
    end
end

local function info(name, value)
    print("INFO " .. name .. ": " .. tostring(value))
end

local function wait_until(pred, secs)
    local steps = math.max(1, math.floor((secs or 5) / 0.01))
    for _ = 1, steps do
        if pred() then return true end
        fan.sleep(0.01)
    end
    return pred()
end

local function skip(reason)
    print("SKIP " .. reason)
    os.exit(77)
end

-- ------------------------------------------------------------- prerequisites
local ok_tcpd, tcpd = pcall(require, 'fan.tcpd')
if not ok_tcpd or type(tcpd) ~= 'table' or type(tcpd.bind) ~= 'function' then
    skip("fan.tcpd unavailable (" .. tostring(tcpd) .. ")")
end

local ok_udpd, udpd = pcall(require, 'fan.udpd')
if not ok_udpd or type(udpd) ~= 'table' or type(udpd.new) ~= 'function' then
    skip("fan.udpd unavailable (" .. tostring(udpd) .. ")")
end

local ok_fifo, fifo = pcall(require, 'fan.fifo')
if not ok_fifo or type(fifo) ~= 'table' or type(fifo.connect) ~= 'function' then
    skip("fan.fifo unavailable (" .. tostring(fifo) .. ")")
end

local ok_core, core = pcall(require, 'fan.httpd.core')
if not ok_core or type(core) ~= 'table' or type(core.bind) ~= 'function' then
    skip("fan.httpd.core unavailable (" .. tostring(core) .. ")")
end

local ok_http, http = pcall(require, 'fan.http')
if not ok_http or type(http) ~= 'table' or type(http.get) ~= 'function' then
    skip("fan.http unavailable (" .. tostring(http) .. ")")
end

-- The pool must exist before fan.loop(): worker bases are created here.
local WORKERS = tonumber(os.getenv and os.getenv("LUAN_TEST_WORKERS") or "") or 4
if (fan.worker_count() or 0) < WORKERS and fan.workers_init then
    pcall(fan.workers_init, WORKERS)
end
WORKERS = fan.worker_count() or 0
if WORKERS < 3 then
    skip("needs >= 3 event workers for distinct owner/reply pairs (worker_count="
             .. tostring(WORKERS) .. ")")
end
info("worker pool", WORKERS)

-- Pinned worker ids used by the shared backends. Keep them inside the pool.
local TCP_WORKER = 1 % WORKERS
local UDP_WORKER = 2 % WORKERS

-- ------------------------------------------------------------------- helpers
local function bind(service)
    local srv = core.bind({ host = "127.0.0.1", port = 0, onService = service })
    if not srv or not srv.port or srv.port <= 0 then
        return nil
    end
    return srv
end

local function close_srv(srv)
    if srv and srv.serv and srv.serv.close then
        pcall(function() srv.serv:close() end)
    end
end

local function fetch(url, secs)
    local out, done = nil, false
    coroutine.wrap(function()
        local ok, res = pcall(http.get, { url = url })
        out = ok and res or { error = tostring(res) }
        done = true
    end)()
    if not wait_until(function() return done end, secs or 10) then
        return nil, "timeout"
    end
    return out
end

local function count_ok(tbl, n, mk)
    local ok = 0
    for i = 1, n do
        if tbl[i] == mk(i) then
            ok = ok + 1
        end
    end
    return ok
end

-- Connect pinning: `worker` is the HTTP request's owner, so the connection's
-- events land on the base that services the handler's own fan.sleep().
local function pinned(worker)
    if worker == nil then return {} end
    return { worker = worker }
end

-- tcpd roundtrip over a worker-pinned echo listener.
local function tcp_roundtrip(port, payload, worker, secs)
    if type(port) ~= "number" then return nil, "no port" end
    local args = pinned(worker)
    args.host, args.port = "127.0.0.1", port
    args.callback_self_first = true
    local out, done = nil, false
    args.onread = function(_, buf)
        out = (out or "") .. tostring(buf)
        if out == payload then done = true end
    end
    args.ondisconnected = function() end
    local conn = tcpd.connect(args)
    if not conn then return nil, "connect failed" end
    if not conn:send(payload) then
        pcall(function() conn:close() end)
        return nil, "send failed"
    end
    wait_until(function() return done end, secs or 10)
    pcall(function() conn:close() end)
    return out
end

-- udpd roundtrip over a worker-pinned echo socket.
local function udp_roundtrip(port, payload, worker, secs)
    if type(port) ~= "number" then return nil, "no port" end
    local args = {
        bind_host = "127.0.0.1",
        bind_port = 0,
        callback_self_first = true,
        onsendready = function() end,
    }
    if worker ~= nil then args.worker = worker end
    local got = nil
    args.onread = function(_, data)
        got = got or tostring(data)
    end
    local sock = udpd.new(args)
    if not sock then return nil, "udpd.new failed" end
    local dest = udpd.make_dest("127.0.0.1", port)
    if not dest then
        pcall(function() sock:close() end)
        return nil, "make_dest failed"
    end
    sock:send(payload, dest)
    wait_until(function() return got ~= nil end, secs or 10)
    pcall(function() sock:close() end)
    return got
end

-- ------------------------------------------------------------------ the work
local function run_all()
    local url_of = function(srv) return "http://127.0.0.1:" .. srv.port .. "/" end

    -- ==================================== shared worker-pinned backends =====
    local tcp_srv = tcpd.bind({
        host = "127.0.0.1", port = 0,
        worker = TCP_WORKER,
        callback_self_first = true,
        onaccept = function(_, apt)
            apt:bind{
                callback_self_first = true,
                onread = function(conn, buf) conn:send(buf) end,
                ondisconnected = function() end,
            }
        end,
    })
    local tcp_port = tcp_srv and tcp_srv:localinfo().port
    check("tcpd echo listener bound to worker " .. TCP_WORKER,
          type(tcp_port) == "number" and tcp_port > 0, tostring(tcp_srv))

    local udp_srv = udpd.new{
        bind_host = "127.0.0.1", bind_port = 0,
        worker = UDP_WORKER,
        callback_self_first = true,
        onread = function(conn, data, from) conn:send(data, from) end,
        onsendready = function() end,
    }
    local udp_port = udp_srv and udp_srv:getPort()
    check("udpd echo socket bound to worker " .. UDP_WORKER,
          type(udp_port) == "number" and udp_port > 0, tostring(udp_srv))

    -- ================================= 1) tcpd under concurrent clients =====
    if tcp_port then
        local N = 24
        local got, done = {}, 0
        for i = 1, N do
            coroutine.wrap(function()
                local payload = "xw-tcp-" .. i .. "-" .. string.rep("t", i)
                local out = tcp_roundtrip(tcp_port, payload, nil, 10)
                got[i] = out or "TIMEOUT"
                done = done + 1
            end)()
        end
        local finished = wait_until(function() return done == N end, 40)
        local ok_n = count_ok(got, N,
            function(i) return "xw-tcp-" .. i .. "-" .. string.rep("t", i) end)
        check("tcpd: " .. N .. " concurrent clients echoed byte-exact",
              finished and ok_n == N, "ok=" .. ok_n .. "/" .. N .. " done=" .. done)
    end

    -- ================================= 2) udpd under concurrent senders =====
    if udp_port then
        local N = 24
        local sockets, got, done = {}, {}, 0
        for i = 1, N do
            sockets[i] = udpd.new{
                bind_host = "127.0.0.1", bind_port = 0,
                callback_self_first = true,
                onread = function(_, data) got[i] = got[i] or tostring(data) end,
                onsendready = function() end,
            }
        end
        local dest = udpd.make_dest("127.0.0.1", udp_port)
        for i = 1, N do
            coroutine.wrap(function()
                local payload = "xw-udp-" .. i .. "-" .. string.rep("u", i)
                if sockets[i] and dest then
                    sockets[i]:send(payload, dest)
                end
                wait_until(function() return got[i] ~= nil end, 10)
                got[i] = got[i] or "TIMEOUT"
                done = done + 1
            end)()
        end
        local finished = wait_until(function() return done == N end, 40)
        local ok_n = count_ok(got, N,
            function(i) return "xw-udp-" .. i .. "-" .. string.rep("u", i) end)
        check("udpd: " .. N .. " concurrent datagrams echoed byte-exact",
              finished and ok_n == N, "ok=" .. ok_n .. "/" .. N .. " done=" .. done)
        for i = 1, N do
            if sockets[i] then pcall(function() sockets[i]:close() end) end
        end
    end

    -- ===================================== 3+4) httpd -> tcpd / udpd ========
    -- Handler pins the client to its own worker (req.worker_id) so the
    -- roundtrip is serviced while the handler is parked in fan.sleep().
    local srv_single = bind(function(req, resp)
        local owner = req.worker_id
        local tag = (req.params and req.params.tag) or "0"
        if req.path == "/tcp" then
            local out = tcp_roundtrip(tcp_port, "echo-tcp-" .. tag, owner, 10)
            resp:reply(out and 200 or 500, out and "OK" or "ERR",
                       tostring(out) .. " owner=" .. tostring(owner))
        else
            local out = udp_roundtrip(udp_port, "echo-udp-" .. tag, owner, 10)
            resp:reply(out and 200 or 500, out and "OK" or "ERR",
                       tostring(out) .. " owner=" .. tostring(owner))
        end
    end)
    check("combined httpd server bind ok (single-hop paths)", srv_single ~= nil)

    if srv_single then
        local N = math.max(WORKERS * 2, 8)
        local tcp_ok, udp_ok, owners, done = 0, 0, {}, 0
        for i = 1, N do
            coroutine.wrap(function()
                local ra = fetch(url_of(srv_single) .. "tcp?tag=" .. i, 15)
                if ra and ra.responseCode == 200 then
                    -- body is "<echoed payload> owner=<worker_id>"
                    local body = tostring(ra.body)
                    if body:match("^(echo%-tcp%-%d+)") == "echo-tcp-" .. i then
                        tcp_ok = tcp_ok + 1
                    end
                    local ow = body:match("owner=(%-?%d+)")
                    if ow then owners[ow] = true end
                end
                local rb = fetch(url_of(srv_single) .. "udp?tag=" .. i, 15)
                if rb and rb.responseCode == 200
                    and tostring(rb.body):match("^(echo%-udp%-%d+)") == "echo-udp-" .. i then
                    udp_ok = udp_ok + 1
                end
                done = done + 1
            end)()
        end
        local finished = wait_until(function() return done == N end, 60)
        check("httpd -> tcpd roundtrip completed for every request",
              finished and tcp_ok == N, "ok=" .. tcp_ok .. "/" .. N .. " done=" .. done)
        check("httpd -> udpd roundtrip completed for every request",
              udp_ok == N, "ok=" .. udp_ok .. "/" .. N)
        local distinct, main_owner = 0, false
        for k in pairs(owners) do
            distinct = distinct + 1
            if k == "-1" then main_owner = true end
        end
        info("distinct request owners seen", distinct)
        info("main-base (worker_id=-1) owners seen", tostring(main_owner))
        check("single-hop requests were spread over multiple workers",
              distinct >= 2, "distinct owners=" .. distinct)
        close_srv(srv_single)
    end
    collectgarbage("collect")

    -- =========== 5+6) httpd -> tcpd + udpd -> fifo wake -> peer reply ======
    -- parked[target] holds responses whose reply must be produced by worker
    -- `target`; each target worker has a FIFO reader pinned to its own base.
    local parked, fifo_readers, fifo_names = {}, {}, {}
    for w = 0, WORKERS - 1 do parked[w] = {} end

    for w = 0, WORKERS - 1 do
        local name = os.tmpname()
        os.remove(name)          -- fifo.connect creates it
        fifo_names[#fifo_names + 1] = name
        fifo_readers[w] = fifo.connect{
            name = name,
            delete_on_close = true,
            rwmode = "rw",
            worker = w,
            onread = function()
                -- Runs on worker w's thread: reply from HERE so the response is
                -- marshaled back to the request owner.
                local list = parked[w]
                while #list > 0 do
                    local item = table.remove(list, 1)
                    if item then
                        local body = "xw tag=" .. item.tag
                            .. " tcp=" .. tostring(item.tcp)
                            .. " udp=" .. tostring(item.udp)
                            .. " reply-worker=" .. w
                            .. " owner=" .. tostring(item.owner)
                        local ok, err = pcall(item.resp.reply, item.resp, 200, "OK", body)
                        if not ok then info("cross-worker reply error", err) end
                    end
                end
            end,
        }
    end
    local readers_ok = 0
    for w = 0, WORKERS - 1 do
        if fifo_readers[w] and fifo_readers[w].send then readers_ok = readers_ok + 1 end
    end
    check("worker-pinned fifo readers created", readers_ok == WORKERS,
          readers_ok .. "/" .. WORKERS)

    local srv_mix = bind(function(req, resp)
        local owner = req.worker_id
        if owner == nil then owner = -1 end
        local target = (owner + 1) % WORKERS
        local tag = tostring(req.params and req.params.tag or "0") .. "-" .. tostring(owner)
        -- hop 1: tcpd   hop 2: udpd  (both pinned to the request's own worker)
        local t = tcp_roundtrip(tcp_port, "tcp-" .. tag, owner, 10)
        local u = udp_roundtrip(udp_port, "udp-" .. tag, owner, 10)
        -- hop 3: wake the target worker through its fifo; it owns the reply
        parked[target][#parked[target] + 1] =
            { resp = resp, owner = owner, tag = tag, tcp = t, udp = u }
        local rd = fifo_readers[target]
        if rd and rd.send then rd:send("wake") end
        return 0   -- answered by worker `target`
    end)
    check("combined httpd server bind ok (multi-hop path)", srv_mix ~= nil)

    if srv_mix then
        local BURST = WORKERS * 3
        local ok_n, done, laterr = 0, 0, nil
        local owners, repliers = {}, {}
        for i = 1, BURST do
            coroutine.wrap(function()
                local res = fetch(url_of(srv_mix) .. "mix?tag=" .. i, 20)
                if res and res.responseCode == 200 then
                    ok_n = ok_n + 1
                    local rw = res.body:match("reply%-worker=(%-?%d+)")
                    local ow = res.body:match("owner=(%-?%d+)")
                    local tcv = res.body:match("tcp=tcp%-(%S+)")
                    local udv = res.body:match("udp=udp%-(%S+)")
                    if rw and ow then
                        repliers[rw] = true
                        owners[ow] = true
                        if rw == ow then
                            laterr = laterr or ("reply came from the owner worker " .. rw)
                        end
                    end
                    if not tcv or not udv then
                        laterr = laterr or ("missing hop payloads: " .. tostring(res.body))
                    end
                end
                done = done + 1
            end)()
        end
        local finished = wait_until(function() return done >= BURST end, 90)
        check("multi-hop: every request answered (tcpd+udpd+fifo+cross-worker reply)",
              finished and ok_n == BURST, "ok=" .. ok_n .. "/" .. BURST .. " done=" .. done)
        check("multi-hop: no reply was produced on the request's owner worker",
              not laterr, laterr or "all replies crossed a worker boundary")

        local no, nr = 0, 0
        for _ in pairs(owners) do no = no + 1 end
        for _ in pairs(repliers) do nr = nr + 1 end
        info("owner workers observed", no)
        info("reply workers observed", nr)
        check("multi-hop burst covered more than one owner worker", no >= 2,
              "owners=" .. no)
    end

    --------------------------------------------------------------- cleanup
    close_srv(srv_mix)
    for w = 0, WORKERS - 1 do
        if fifo_readers[w] then
            pcall(function() fifo_readers[w]:close() end)
            fifo_readers[w] = nil
        end
    end
    for _, nm in ipairs(fifo_names) do pcall(os.remove, nm) end
    if tcp_srv then pcall(function() tcp_srv:close() end) end
    if udp_srv then pcall(function() udp_srv:close() end) end
    collectgarbage("collect")
end

fan.loop(function()
    local ok, err = pcall(run_all)
    if not ok then
        print("FAIL suite error: " .. tostring(err))
        failed = true
    end
    fan.loopbreak()
end)

if failed then
    print("RESULT FAIL")
    os.exit(1)
end
print("RESULT PASS")
os.exit(0)
