#!/usr/bin/env lua

-- test_mariadb_pending_owner.lua
-- Close-drain ownership: a connection whose async wait is parked on a *foreign*
-- worker's event base must still be cancellable from the closing thread.
--
-- Contract under test (docs/threading-model.md, R19-R22):
--   * every armed wait records the worker base that owns its libevent event;
--   * `conn:close()` runs `mariadb_cancel_pending_waits()` on the closing
--     thread, which must not run a continuation (and `event_free()` its event)
--     for a wait that belongs to another base -- it hands the cancellation back
--     to that base and waits for the claim to be released
--     (`wait_for_foreign_continuations()`);
--   * the parked call still comes back with (nil, error), and the close must
--     return instead of waiting for the SLEEP() to finish or blocking forever
--     on a claim that was never released.
--
-- What this covers that test_mariadb_pending_event.lua does not: that guard
-- closes the connection from the same thread that armed the wait (the
-- `event_mgr_is_current_owner()` path). Here the wait is armed on worker 1's
-- base while the close runs on the main thread, i.e. the hand-off path
-- (`event_mgr_worker_once_internal()`), repeated to catch a claim/in-flight leak
-- (a leaked claim makes the *next* close hang).
--
-- Requirements:
--   * luafan built WITH src/mariadb and a reachable MariaDB
--     (cd tests && ./docker-setup.sh start);
--   * an event-worker pool: this file calls fan.workers_init() itself, which is
--     only legal before fan.loop(), so it enters fan.loop() on its own:
--
--       cd tests && LUAN_TEST_WORKERS=4 lua lua/test_mariadb_pending_owner.lua
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

-- The worker pool must exist before fan.loop(): the connection's wait event is
-- registered on the base of its worker affinity, which workers_init() creates.
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

local function connect(worker)
    return mariadb.connect(DB.database, DB.user, DB.password, DB.host, DB.port, worker)
end

-- Park one slow query on the connection's base, then close the connection from
-- *this* (main) thread. Returns a table describing what happened.
local function park_then_close(worker, sleep_secs)
    local conn, cerr = connect(worker)
    if not conn then
        return nil, "connect: " .. tostring(cerr)
    end

    local done, result, err = false, nil, nil
    coroutine.wrap(function()
        result, err = conn:execute("SELECT SLEEP(" .. sleep_secs .. "), id FROM crash_test")
        done = true
    end)()

    -- Let the query arm its wait event on the connection's base (and let that
    -- base's loop dispatch it if it fires early -- the R22 window).
    fan.sleep(0.3)

    local parked_before_close = not done
    local t0 = os.clock()
    local closed, close_err = pcall(function() conn:close() end)
    local elapsed = os.clock() - t0

    -- A continuation that was never cancelled would fire after the close here.
    fan.sleep(0.2)

    return {
        parked = parked_before_close,
        closed = closed,
        close_err = close_err,
        done = done,
        result = result,
        err = err,
        elapsed = elapsed,
    }
end

-- The cancellation contract is the same whichever thread ran it: the parked
-- call was still suspended when close() started and came back with (nil, error),
-- and the close did not wait for the server-side SLEEP() to finish.
local function check_aborted(label, r, sleep_secs)
    if r == nil then
        check(label .. ": connection created", false, select(2, r))
        return
    end
    check(label .. ": query was parked before close", r.parked)
    check(label .. ": close() returned", r.closed, r.close_err)
    check(label .. ": close() aborted the parked query",
          r.done and r.result == nil and type(r.err) == "string",
          "done=" .. tostring(r.done) .. " res=" .. tostring(r.result) ..
          " err=" .. tostring(r.err))
    check(label .. ": close() did not wait for SLEEP(" .. sleep_secs .. ")",
          r.elapsed < (sleep_secs / 2),
          string.format("%.2fs", r.elapsed))
end

-- ------------------------------------------------------------------ driver
fan.loop(function()
    local probe = connect(-1)
    if not probe then
        print("SKIP MariaDB not reachable")
        fan.loopbreak()
        os.exit(77)
        return
    end
    pcall(function()
        probe:execute("CREATE TABLE IF NOT EXISTS crash_test (id INT PRIMARY KEY)")
        probe:execute("INSERT IGNORE INTO crash_test VALUES (1)")
    end)
    probe:close()

    ------------------------------------------- 1) foreign owner (worker base)
    -- The wait is armed on worker 1's base; close() runs on the main thread, so
    -- the drain has to hand the cancellation back to worker 1. Repeated: the
    -- close after a leaked claim would block in wait_for_foreign_continuations().
    local ROUNDS = 3
    local foreign_ok = 0
    for i = 1, ROUNDS do
        local r = park_then_close(1, 3)
        check_aborted("foreign owner round " .. i, r, 3)
        if r and r.closed and r.done and r.result == nil then
            foreign_ok = foreign_ok + 1
        end
    end
    info("foreign-owner cancellations", foreign_ok .. "/" .. ROUNDS)

    ------------------------------------------- 2) owner thread (main base)
    -- Affinity -1 keeps the wait on the main base, so the same close takes the
    -- inline path (`event_mgr_is_current_owner()`).
    check_aborted("owner thread", park_then_close(-1, 3), 3)

    ------------------------------------------- 3) nothing left armed
    -- A fresh connection must still serve queries, and its close must not wait
    -- on an in-flight count some earlier close failed to release.
    do
        local conn = connect(0)
        check("fresh connection after the cancellations", conn ~= nil)
        if conn then
            local cursor, cerr = conn:execute("SELECT 7 AS v")
            local value
            if cursor and type(cursor) == "userdata" then
                local row = cursor:fetch()
                cursor:close()
                if type(row) == "table" then
                    for _, v in pairs(row) do value = v end
                end
            end
            check("fresh connection serves a query", value == 7,
                  "v=" .. tostring(value) .. " err=" .. tostring(cerr))
            local closed, close_err = pcall(function() conn:close() end)
            check("fresh connection closes", closed, close_err)
        end
    end

    pcall(function()
        local cleanup = connect(-1)
        if cleanup then
            cleanup:execute("DROP TABLE IF EXISTS crash_test")
            cleanup:close()
        end
    end)

    fan.loopbreak()
end)

print(failed and "RESULT FAIL" or "RESULT PASS")
os.exit(failed and 1 or 0)
