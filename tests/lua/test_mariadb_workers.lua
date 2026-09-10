#!/usr/bin/env lua

-- test_mariadb_workers.lua
-- Multi-worker validation for the MariaDB module (fan.mariadb) under the luafan
-- event-worker pool.
--
-- Contract under test (docs/api/mariadb.md, docs/threading-model.md):
--   * mariadb.connect(..., worker) is an event-worker affinity: -1 keeps every
--     connection event on the main base, 0..N-1 pins them to that worker's
--     base, omitted selects round-robin, and invalid/out-of-range values raise;
--   * the async wait event is registered on that base, so the coroutine that
--     yielded inside mariadb is resumed by that base's thread — including the
--     cross-thread cases (main -> worker and worker -> another worker);
--   * statements and cursors inherit the connection's affinity.
--
-- Requirements:
--   * luafan built WITH src/mariadb (CMake/Linux build includes it; the macOS
--     Xcode targets exclude luafan/src/mariadb, so this file skips there);
--   * a reachable MariaDB (cd tests && ./docker-setup.sh start);
--   * an event-worker pool — this file calls fan.workers_init() itself, which
--     is only legal before fan.loop(), so the file enters fan.loop() on its own:
--
--       cd tests && lua lua/test_mariadb_workers.lua
--
-- Do NOT run this file through run_all_lua_tests.lua: that runner already owns
-- a fan.loop() and a nested fan.loop() degrades to a synchronous pcall in which
-- marshal calls cannot yield.
--
-- Exit codes: 0 = pass, 1 = fail, 77 = skipped (no fan.mariadb / no server /
-- no worker pool).

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

-- ------------------------------------------------------------------ prerequisites
local ok_maria, mariadb = pcall(require, 'fan.mariadb')
if not ok_maria or type(mariadb) ~= 'table' or type(mariadb.connect) ~= 'function' then
    skip("fan.mariadb unavailable (" .. tostring(mariadb) .. ")")
end

local DB = { host = "127.0.0.1", port = 3306, database = "test_db",
             user = "test_user", password = "test_password" }
do
    local ok_cfg, cfg = pcall(require, 'mariadb_test_config')
    if ok_cfg and type(cfg) == 'table' and cfg.DB_CONFIG then
        DB = cfg.DB_CONFIG
    end
    -- Container/CI override: when the test runs in a container, the MariaDB
    -- container is reachable through the docker host rather than loopback.
    local env = os.getenv
    if env then
        DB = {
            host = env("LUAN_TEST_DB_HOST") or DB.host,
            port = tonumber(env("LUAN_TEST_DB_PORT") or "") or DB.port,
            database = env("LUAN_TEST_DB_NAME") or DB.database,
            user = env("LUAN_TEST_DB_USER") or DB.user,
            password = env("LUAN_TEST_DB_PASSWORD") or DB.password,
        }
    end
end

-- The worker pool must exist before fan.loop(): mariadb resumes on the worker
-- base, and the bases are created by workers_init().
local WORKERS = tonumber(os.getenv and os.getenv("LUAN_TEST_WORKERS") or "") or 8
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

-- ------------------------------------------------------------------ helpers
local function connect(worker)
    if worker == nil then
        return mariadb.connect(DB.database, DB.user, DB.password, DB.host, DB.port)
    end
    return mariadb.connect(DB.database, DB.user, DB.password, DB.host, DB.port, worker)
end

-- Run one SELECT and return the single value of the first row.
local function scalar(conn, sql)
    local cursor, err = conn:execute(sql)
    if not cursor then return nil, err or "no cursor" end
    local row
    if type(cursor) == "userdata" then
        row = cursor:fetch()
        cursor:close()
    else
        row = cursor
    end
    if type(row) ~= "table" then return nil, "no row" end
    for _, v in pairs(row) do return v end
    return nil, "empty row"
end

-- Open a connection, run `queries` SELECTs, close it. Returns ok, extra.
local function roundtrip(worker, queries)
    local conn, cerr = connect(worker)
    if not conn then return false, "connect: " .. tostring(cerr) end
    for i = 1, (queries or 1) do
        local v, err = scalar(conn, "SELECT " .. i .. " AS v")
        if v ~= i then
            conn:close()
            return false, "query " .. i .. ": " .. tostring(v) .. " " .. tostring(err)
        end
    end
    conn:close()
    return true, nil
end

local server_setup = nil   -- httpd.core bind helper (required lazily)

local function load_httpd()
    if server_setup ~= nil then return server_setup end
    local ok_core, core = pcall(require, 'fan.httpd.core')
    local ok_http, http = pcall(require, 'fan.http.http')
    if not ok_core or not ok_http then
        server_setup = false
        return false
    end
    server_setup = { core = core, http = http }
    return server_setup
end

local function fetch(url, secs)
    local out
    coroutine.wrap(function()
        local ok, res = pcall(server_setup.http.get, { url = url })
        if ok then out = res end
    end)()
    wait_until(function() return out ~= nil end, secs or 10)
    return out
end

-- ------------------------------------------------------------------ test cases
local function run_all()
    ---------------------------------------------------------------- 1) validation
    do
        local cases = {
            { "worker >= pool size", WORKERS },
            { "worker < -1", -2 },
        }
        for _, c in ipairs(cases) do
            local ok = pcall(connect, c[2])
            check("connect rejects " .. c[1], not ok)
        end
        check("connect rejects worker non-integer",
              not pcall(mariadb.connect, DB.database, DB.user, DB.password,
                        DB.host, DB.port, "x"))

        for _, w in ipairs({ -1, 0, WORKERS - 1 }) do
            local ok, conn = pcall(connect, w)
            if ok and conn then conn:close() end
            check("connect accepts worker=" .. w, ok and conn ~= nil, conn)
        end
        local ok, conn = pcall(connect, nil)
        if ok and conn then conn:close() end
        check("connect accepts omitted worker (round-robin)", ok and conn ~= nil, conn)
    end

    ---------------------------------------------------------------- 2) main base
    do
        local ok, err = roundtrip(-1, 2)
        check("main-base connection serves queries", ok, err)
    end

    ------------------------------------------------- 3) main -> each worker base
    -- The wait event lives on worker i's base, so every resume happens on that
    -- worker thread while the caller is the main thread.
    local ok_all, first_err = true, nil
    for i = 0, WORKERS - 1 do
        local ok, err = roundtrip(i, 2)
        if not ok then
            ok_all = false
            first_err = first_err or ("worker " .. i .. ": " .. tostring(err))
        end
    end
    check("every worker base drives a main-thread mariadb call", ok_all, first_err)

    ------------------------------------------------- 4) round-robin (no worker)
    do
        local ok_all = true
        for _ = 1, WORKERS do
            local ok, err = roundtrip(nil, 1)
            if not ok then ok_all = false end
        end
        check("round-robin connections serve queries", ok_all)
    end

    local httpd = load_httpd()
    if not httpd then
        print("SKIP httpd.core based worker cases (fan.httpd.core/fan.http.http unavailable)")
        return
    end
    local core = httpd.core

    --------------------------------------- 5) worker thread, affinity = own worker
    do
        local seen, ok_req, bad = {}, 0, nil
        local srv = core.bind{
            host = "127.0.0.1", port = 0,
            onService = function(req, resp)
                local wid = req.worker_id
                seen[wid] = (seen[wid] or 0) + 1
                local ok, err = roundtrip(wid, 3)
                if ok then
                    ok_req = ok_req + 1
                    resp:reply(200, "OK", "ok")
                else
                    bad = bad or ("worker " .. tostring(wid) .. ": " .. tostring(err))
                    resp:reply(500, "ERR", tostring(err))
                end
            end,
        }
        check("worker-affinity server bind ok", srv and srv.port, srv and srv.port)

        local n = math.max(WORKERS * 2, 16)
        local done = 0
        for _ = 1, n do
            local res = fetch("http://127.0.0.1:" .. srv.port .. "/")
            if res and res.responseCode == 200 then done = done + 1 end
        end
        check("worker-thread connections serve queries", done == n,
              "ok=" .. done .. "/" .. n .. (bad and (" (" .. bad .. ")") or ""))

        local distinct = 0
        for _ in pairs(seen) do distinct = distinct + 1 end
        check("requests ran on every worker", distinct == WORKERS,
              "distinct=" .. distinct .. "/" .. WORKERS)

        ------------------------------------- 6) worker thread, no affinity given
        -- Round-robin may place the connection on ANOTHER worker than the one
        -- running the handler, so the resume crosses a worker boundary.
        local srv2 = core.bind{
            host = "127.0.0.1", port = 0,
            onService = function(req, resp)
                local ok, err = roundtrip(nil, 2)
                if ok then
                    resp:reply(200, "OK", "ok")
                else
                    resp:reply(500, "ERR", tostring(err))
                end
            end,
        }
        check("round-robin worker server bind ok", srv2 and srv2.port, srv2 and srv2.port)
        local done2 = 0
        local m = math.max(WORKERS * 2, 16)
        for _ = 1, m do
            local res = fetch("http://127.0.0.1:" .. srv2.port .. "/")
            if res and res.responseCode == 200 then done2 = done2 + 1 end
        end
        check("handler-issued round-robin connections serve queries", done2 == m,
              "ok=" .. done2 .. "/" .. m)
        srv2.serv:close()
        srv2 = nil
        collectgarbage("collect")

        ------------------------------------------- 7) concurrent load per worker
        local CONC = WORKERS * 4
        local PER_REQ = 5
        local hits = {}
        local srv3 = core.bind{
            host = "127.0.0.1", port = 0,
            onService = function(req, resp)
                local wid = req.worker_id
                hits[wid] = (hits[wid] or 0) + 1
                local ok, err = roundtrip(wid, PER_REQ)
                if ok then
                    resp:reply(200, "OK", "ok")
                else
                    resp:reply(500, "ERR", tostring(err))
                end
            end,
        }
        local t0 = os.clock()
        local done3, failed3 = 0, 0
        for _ = 1, CONC do
            coroutine.wrap(function()
                local res = fetch("http://127.0.0.1:" .. srv3.port .. "/", 30)
                if res and res.responseCode == 200 then
                    done3 = done3 + 1
                else
                    failed3 = failed3 + 1
                end
            end)()
        end
        local finished = wait_until(function() return done3 + failed3 >= CONC end, 60)
        local dt = os.clock() - t0
        check("concurrent worker load completes", finished,
              "done=" .. (done3 + failed3) .. "/" .. CONC)
        check("concurrent worker load all 200", failed3 == 0 and done3 == CONC,
              "ok=" .. done3 .. " fail=" .. failed3 .. " queries=" .. (CONC * PER_REQ))
        local distinct3 = 0
        for _ in pairs(hits) do distinct3 = distinct3 + 1 end
        check("concurrent load hit every worker", distinct3 == WORKERS,
              "distinct=" .. distinct3 .. "/" .. WORKERS)
        info("load", string.format("%d concurrent x %d queries, %.2fs",
                                   CONC, PER_REQ, dt))

        srv3.serv:close()
        srv3 = nil
        srv.serv:close()
        srv = nil
        collectgarbage("collect")
    end

    ------------------------------------------ 8) cross-worker misuse (opt-in)
    -- Documented contract: a connection (and its statements/cursors) has a
    -- worker affinity. Using it from a *different* worker is outside the
    -- contract, so this case is opt-in (LUAN_TEST_MARIADB_XWORKER=1) and only
    -- observes the behaviour (no hang, a definite outcome) — it never decides
    -- pass/fail, because this reply would also be written from the wrong
    -- (non-owner) thread.
    if os.getenv and os.getenv("LUAN_TEST_MARIADB_XWORKER") == "1" then
        local conn = connect(0)
        check("cross-worker probe: connection on worker 0 created", conn ~= nil)
        if conn then
            local observed = "pending"
            local srv = core.bind{
                host = "127.0.0.1", port = 0, worker = 1,
                onService = function(req, resp)
                    local ok, err = pcall(function()
                        return scalar(conn, "SELECT 7 AS v")
                    end)
                    observed = ok and "query ok" or ("error: " .. tostring(err))
                    resp:reply(200, "OK", tostring(observed))
                end,
            }
            local res = fetch("http://127.0.0.1:" .. srv.port .. "/", 10)
            info("cross-worker probe (worker0 conn used on worker1)", observed)
            if res and res.responseCode == 200 then
                info("cross-worker probe reply", res.body)
            else
                info("cross-worker probe reply", "none/timeout")
            end
            srv.serv:close()
            srv = nil
            pcall(function() conn:close() end)
            collectgarbage("collect")
        end
    end
end

-- ------------------------------------------------------------------ driver
fan.loop(function()
    local ok, err = pcall(run_all)
    if not ok then
        print("FAIL suite error: " .. tostring(err))
        failed = true
    end
    fan.loopbreak()
end)

print(failed and "RESULT FAIL" or "RESULT PASS")
os.exit(failed and 1 or 0)
