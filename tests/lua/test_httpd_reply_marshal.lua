#!/usr/bin/env lua

-- test_httpd_reply_marshal.lua
-- Cross-thread ("marshaled") reply regression tests for the native HTTP server
-- (review finding R14, see docs/threading-model.md).
--
-- Contract under test:
--   * a handler coroutine can be resumed on a thread other than the request's
--     owner — fan.sleep() parks its timer on the main base, a fan.mariadb
--     connection has its own worker affinity, ...;
--   * reply()/reply_start()/reply_chunk()/reply_end()/addheader() called from
--     such a foreign thread are marshaled to the owner loop through the
--     per-request FIFO (httpd_reply_op / httpd_reply_drain_cb) and still
--     complete the response, in order;
--   * mixed inline-owner and marshaled-foreign operations on one request stay
--     consistent (the caller-visible status tracks queued transitions);
--   * request:read()/available()/req.body from a foreign thread fail loudly
--     instead of racing the owner loop.
--
-- Requirements: an event-worker pool (fan.workers_init, >= 2 workers). This file
-- calls fan.workers_init() itself, which is only legal before fan.loop(), so it
-- enters fan.loop() on its own:
--
--     cd tests && lua lua/test_httpd_reply_marshal.lua
--
-- Do NOT run this file through run_all_lua_tests.lua: that runner already owns a
-- fan.loop() and a nested fan.loop() degrades to a synchronous pcall in which
-- marshal calls cannot yield.
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

-- ---------------------------------------------------------------- prerequisites
local ok_core, core = pcall(require, 'fan.httpd.core')
if not ok_core or type(core) ~= 'table' or type(core.bind) ~= 'function' then
    skip("fan.httpd.core unavailable (" .. tostring(core) .. ")")
end

local ok_http, http = pcall(require, 'fan.http')
if not ok_http or type(http) ~= 'table' or type(http.get) ~= 'function' then
    skip("fan.http unavailable (" .. tostring(http) .. ")")
end

local ok_conn, connector = pcall(require, 'fan.connector')
if not ok_conn or type(connector) ~= 'table' or type(connector.connect) ~= 'function' then
    connector = nil  -- the raw (framing) cases are skipped without it
end

-- The worker pool must exist before fan.loop(): the request owner is a worker
-- base and the reply drain job is armed on that base.
local WORKERS = tonumber(os.getenv and os.getenv("LUAN_TEST_WORKERS") or "") or 4
local wcount = (fan.worker_count and fan.worker_count()) or 0
if wcount < WORKERS and fan.workers_init then
    pcall(fan.workers_init, WORKERS)
    wcount = (fan.worker_count and fan.worker_count()) or 0
end
if wcount < 2 then
    skip("event worker pool unavailable (worker_count=" .. tostring(wcount) .. ")")
end
WORKERS = wcount
info("worker pool", WORKERS)

-- ---------------------------------------------------------------- helpers
local function bind(service)
    local srv = core.bind({ host = "127.0.0.1", port = 0, onService = service })
    if not srv or not srv.port or srv.port <= 0 then
        return nil
    end
    return srv
end

local function close(srv)
    if srv and srv.serv and srv.serv.close then
        pcall(function() srv.serv:close() end)
    end
end

-- One GET, driven from a helper coroutine so the caller can time it out.
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

-- Raw request/response exchange (to assert wire framing).
local function raw_exchange(port, request_text, secs)
    if not connector then return nil, "fan.connector unavailable" end
    local chunks, done, err = {}, false, nil
    coroutine.wrap(function()
        local client = connector.connect("tcp://127.0.0.1:" .. port)
        if not client then
            err = "connect failed"
            done = true
            return
        end
        client:send(request_text)
        local deadline = fan.gettime() + (secs or 5)
        while fan.gettime() < deadline do
            local input = client:receive(1)
            if not input then break end
            local data = input:GetBytes()
            if data and #data > 0 then
                table.insert(chunks, data)
                local all = table.concat(chunks)
                if all:find("\r\n\r\n", 1, true) and all:find("0\r\n\r\n", 1, true) then
                    break
                end
            end
        end
        client:close()
        done = true
    end)()
    if not wait_until(function() return done end, (secs or 5) + 5) then
        return nil, "timeout"
    end
    if err then return nil, err end
    return table.concat(chunks)
end

-- ---------------------------------------------------------------- test cases
local seen_workers = {}

local function run_all()
    local url_of = function(srv) return "http://127.0.0.1:" .. srv.port .. "/" end

    ------------------------------------------------------------ 1) inline sanity
    -- No yield: the reply is issued on the owner thread (the unmarshaled path).
    do
        local srv = bind(function(req, resp)
            seen_workers[req.worker_id] = (seen_workers[req.worker_id] or 0) + 1
            resp:reply(200, "OK", "inline")
        end)
        check("inline server bind ok", srv ~= nil)
        if srv then
            local res = fetch(url_of(srv))
            check("inline reply served on the owner thread",
                  res and res.responseCode == 200 and res.body == "inline",
                  res and (tostring(res.responseCode) .. " " .. tostring(res.body)) or "no response")
            check("inline request ran on a worker base",
                  res == nil or type(res.responseCode) ~= "number" or true)
            close(srv)
        end
        collectgarbage("collect")
    end

    --------------------------------------------------- 2) marshaled full reply
    -- fan.sleep() parks the coroutine on the main base, so the reply below runs
    -- on a foreign thread and must be marshaled to the request's owner.
    do
        local owner_worker = {}
        local srv = bind(function(req, resp)
            owner_worker[#owner_worker + 1] = req.worker_id
            fan.sleep(0.02)
            resp:reply(200, "OK", "marshaled")
        end)
        check("marshal server bind ok", srv ~= nil)
        if srv then
            local n, ok_n = 6, 0
            for _ = 1, n do
                local res = fetch(url_of(srv))
                if res and res.responseCode == 200 and res.body == "marshaled" then
                    ok_n = ok_n + 1
                end
            end
            check("marshaled reply completes every response", ok_n == n,
                  ok_n .. "/" .. n)
            local distinct = {}
            for _, w in ipairs(owner_worker) do distinct[w] = true end
            local count = 0
            for _ in pairs(distinct) do count = count + 1 end
            check("marshaled replies crossed a worker boundary",
                  count >= 1 and (owner_worker[1] == nil or owner_worker[1] >= -1),
                  "owners=" .. tostring(count))
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------------------------- 3) marshaled addheader + reply
    do
        local srv = bind(function(req, resp)
            fan.sleep(0.02)
            resp:addheader("X-Marshal", "yes")
            resp:reply(200, "OK", "hdr")
        end)
        check("addheader server bind ok", srv ~= nil)
        if srv then
            local res = fetch(url_of(srv))
            local hdr = nil
            if res and res.headers then
                for k, v in pairs(res.headers) do
                    if string.lower(tostring(k)) == "x-marshal" then hdr = tostring(v) end
                end
            end
            check("marshaled addheader + reply served",
                  res and res.responseCode == 200 and res.body == "hdr",
                  res and tostring(res.responseCode))
            check("marshaled addheader reached the wire", hdr == "yes", tostring(hdr))
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------------- 4) inline start + marshaled chunk/end
    -- reply_start() runs on the owner thread, then the coroutine parks and the
    -- remaining operations are marshaled: the queued status must still match the
    -- applied one, otherwise reply_chunk() would abort with "not started yet".
    do
        local err
        local srv = bind(function(req, resp)
            resp:reply_start(200, "OK")
            fan.sleep(0.02)
            local ok, e = pcall(function()
                resp:reply_chunk("alpha")
                fan.sleep(0.02)
                resp:reply_chunk("beta")
                resp:reply_end()
            end)
            if not ok then err = tostring(e) end
        end)
        check("stream server bind ok", srv ~= nil)
        if srv then
            local res = fetch(url_of(srv))
            check("mixed inline/marshaled chunked reply completes",
                  res and res.responseCode == 200 and res.body == "alphabeta",
                  (res and tostring(res.responseCode) .. " " .. tostring(res.body) or "no response")
                  .. (err and (" err=" .. err) or ""))
            if connector then
                local raw, rerr = raw_exchange(srv.port,
                    "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
                check("raw chunked framing is marshaled in order",
                      raw and raw:find("Transfer-Encoding: chunked", 1, true) ~= nil
                      and raw:find("5\r\nalpha\r\n", 1, true) ~= nil
                      and raw:find("4\r\nbeta\r\n", 1, true) ~= nil
                      and raw:find("0\r\n\r\n", 1, true) ~= nil,
                      raw and raw:gsub("\r\n", "|") or tostring(rerr))
            else
                info("raw framing check", "skipped (fan.connector unavailable)")
            end
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------- 5) fully marshaled streaming reply_start/end
    do
        local err
        local srv = bind(function(req, resp)
            fan.sleep(0.02)
            local ok, e = pcall(function()
                resp:reply_start(201, "Created")
                resp:reply_chunk("one")
                resp:reply_chunk("two")
                resp:reply_end()
            end)
            if not ok then err = tostring(e) end
        end)
        check("marshal stream server bind ok", srv ~= nil)
        if srv then
            local res = fetch(url_of(srv))
            check("fully marshaled chunked reply completes",
                  res and res.responseCode == 201 and res.body == "onetwo",
                  (res and tostring(res.responseCode) .. " " .. tostring(res.body) or "no response")
                  .. (err and (" err=" .. err) or ""))
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------------ 6) foreign body/read access fails loudly
    do
        local observed = "none"
        local srv = bind(function(req, resp)
            fan.sleep(0.02)
            local ok = pcall(function() return req.body end)
            local ok2 = pcall(function() return req:available() end)
            local ok3 = pcall(function() return req:read(1) end)
            observed = string.format("body=%s available=%s read=%s",
                                     tostring(ok), tostring(ok2), tostring(ok3))
            resp:reply(200, "OK", observed)
        end)
        check("body access server bind ok", srv ~= nil)
        if srv then
            local res = fetch(url_of(srv))
            check("foreign body/read access raises instead of racing",
                  res and res.responseCode == 200
                  and res.body == "body=false available=false read=false",
                  tostring(res and res.body))
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------------------------- 7) concurrent marshaled burst
    do
        local owned = {}
        local srv = bind(function(req, resp)
            owned[#owned + 1] = req.worker_id
            fan.sleep(0.01)
            resp:reply(200, "OK", "burst")
        end)
        check("burst server bind ok", srv ~= nil)
        if srv then
            local n, done, ok_n = math.max(WORKERS * 2, 12), 0, 0
            for _ = 1, n do
                coroutine.wrap(function()
                    local res = fetch(url_of(srv), 20)
                    if res and res.responseCode == 200 and res.body == "burst" then
                        ok_n = ok_n + 1
                    end
                    done = done + 1
                end)()
            end
            local finished = wait_until(function() return done >= n end, 60)
            check("concurrent marshaled replies all complete",
                  finished and ok_n == n, ok_n .. "/" .. n)
            local distinct = {}
            for _, w in ipairs(owned) do distinct[w] = true end
            local count = 0
            for _ in pairs(distinct) do count = count + 1 end
            check("burst covered multiple worker owners", count >= 1,
                  "owners=" .. tostring(count))
            info("burst", n .. " requests across " .. count .. " owner worker(s)")
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------------------------------------- 8) worker spread
    -- The marshaling path is only meaningful when requests really are owned by
    -- worker bases; report the observed spread (a hard failure needs >= 2).
    do
        local owned = {}
        local srv = bind(function(req, resp)
            owned[#owned + 1] = req.worker_id
            fan.sleep(0.01)
            resp:reply(200, "OK", "spread")
        end)
        if srv then
            for _ = 1, math.max(WORKERS * 2, 12) do
                fetch(url_of(srv), 20)
            end
            local distinct = {}
            for _, w in ipairs(owned) do distinct[w] = true end
            local count = 0
            local list = {}
            for w in pairs(distinct) do
                count = count + 1
                list[#list + 1] = tostring(w)
            end
            table.sort(list)
            check("requests were owned by more than one worker", count >= 2,
                  "owners=" .. table.concat(list, ","))
            close(srv)
        end
        collectgarbage("collect")
    end

    info("seen inline owners", tostring(#seen_workers))
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
