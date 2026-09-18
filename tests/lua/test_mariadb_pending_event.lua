#!/usr/bin/env lua

-- Regression guard for MariaDB async wait cancellation (was a crash inducer).
-- Bug it used to induce: closing a connection while a query was yielding did NOT
-- cancel the pending DB_STATUS event, so the continuation ran after mysql_close()
-- had freed the connection internals (use-after-free / SIGSEGV).
-- Fix: mariadb_cancel_pending_waits() aborts every armed wait from the close
-- state machine (src/luamariadb.c, src/mariadb/luamariadb_close.c).
--
-- Strategy: start a slow query with SLEEP() in a coroutine, close the connection
-- mid-query, then assert that (a) nothing crashed and (b) the suspended execute()
-- was resumed with (nil, error) while close() ran -- i.e. the wait really was
-- cancelled rather than never armed. A signal now FAILS the suite.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("MariaDB pending event cancellation - regression guard")

-- Check mariadb availability
local mariadb
local ok, mod = pcall(require, 'fan.mariadb')
if not ok then
    print("fan.mariadb not available, skipping")
    local failed = TestFramework.run_suite(suite)
    os.exit(77)
end
mariadb = mod

local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

local CRASH_SCRIPT = [[
local fan = require "fan"
local mariadb = require "fan.mariadb"

local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

fan.loop(function()
    local conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user,
                                  DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)
    if not conn then
        print("MariaDB not reachable")
        fan.loopbreak()
        os.exit(77)
        return
    end

    pcall(function()
        conn:execute("CREATE TABLE IF NOT EXISTS crash_test (id INT PRIMARY KEY)")
        conn:execute("INSERT IGNORE INTO crash_test VALUES (1)")
    end)

    -- Run the slow query concurrently while the main coroutine keeps going.
    -- luafan has no fan.spawn(); concurrent Lua work is a coroutine started with
    -- coroutine.wrap (same pattern as the other race suites in this directory).
    -- The outcome is recorded so the run proves close() *aborted* the query,
    -- instead of only proving that nothing crashed.
    local query_done = false
    local query_result, query_err
    coroutine.wrap(function()
        query_result, query_err = conn:execute("SELECT SLEEP(5), id FROM crash_test")
        query_done = true
    end)()

    -- Let the query register its continuation event
    fan.sleep(0.3)

    -- The execute() must still be parked here, otherwise this run does not
    -- exercise the race at all and must not report success.
    local parked_before_close = not query_done

    -- Close the connection while that event is still pending. If the pending
    -- event is not cancelled, it fires after mysql_close() released the
    -- connection internals and its continuation calls mysql_real_query_cont()
    -- on freed memory -> CRASH.
    local close_returned = pcall(function() conn:close() end)

    -- Give a stale callback a chance to fire (with the fix it never fires)
    fan.sleep(1.0)

    print("parked_before_close=" .. tostring(parked_before_close))
    print("close_returned=" .. tostring(close_returned))
    print("query_done=" .. tostring(query_done))
    print("query_result=" .. tostring(query_result))
    print("query_err=" .. tostring(query_err))
    -- Aborted (not merely "no crash"): the suspended execute() was still parked
    -- when close() ran and came back with (nil, error) -- the error matters: a
    -- bare (nil, nil) would mean the call was resumed without a diagnosis.
    if parked_before_close and query_done and query_result == nil
        and query_err ~= nil then
        print("PENDING_QUERY_ABORTED_ON_CLOSE")
    end

    -- Cleanup
    pcall(function()
        local c2 = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user,
                                    DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)
        if c2 then
            c2:execute("DROP TABLE IF EXISTS crash_test")
            c2:close()
        end
    end)

    fan.loopbreak()
end)

os.exit(0)
]]

-- Same scenario through a *prepared statement*: the wait's coroutine reference is
-- held by the statement (STMT_CTX) instead of the connection, so aborting it
-- exercises the other unref branch of mariadb_cancel_pending_waits().
local STMT_SCRIPT = [[
local fan = require "fan"
local mariadb = require "fan.mariadb"

local DB_CONFIG = {
    host = "127.0.0.1",
    port = 3306,
    database = "test_db",
    user = "test_user",
    password = "test_password"
}

fan.loop(function()
    local conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user,
                                  DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)
    if not conn then
        print("MariaDB not reachable")
        fan.loopbreak()
        os.exit(77)
        return
    end

    pcall(function()
        conn:execute("CREATE TABLE IF NOT EXISTS crash_test (id INT PRIMARY KEY)")
        conn:execute("INSERT IGNORE INTO crash_test VALUES (1)")
    end)

    local stmt = conn:prepare("SELECT SLEEP(5), id FROM crash_test")
    if not stmt then
        print("prepare failed")
        fan.loopbreak()
        os.exit(0)
        return
    end

    local stmt_done = false
    local stmt_result, stmt_err
    coroutine.wrap(function()
        stmt_result, stmt_err = stmt:execute()
        stmt_done = true
    end)()

    fan.sleep(0.3)

    -- The execute() must still be parked here, otherwise this run does not
    -- exercise the race at all and must not report success.
    local parked_before_close = not stmt_done

    local close_returned = pcall(function() conn:close() end)

    fan.sleep(1.0)

    print("parked_before_close=" .. tostring(parked_before_close))
    print("close_returned=" .. tostring(close_returned))
    print("stmt_done=" .. tostring(stmt_done))
    print("stmt_result=" .. tostring(stmt_result))
    print("stmt_err=" .. tostring(stmt_err))
    if parked_before_close and stmt_done and stmt_result == nil
        and stmt_err ~= nil then
        print("PENDING_STMT_ABORTED_ON_CLOSE")
    end

    pcall(function()
        local c2 = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user,
                                    DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)
        if c2 then
            c2:execute("DROP TABLE IF EXISTS crash_test")
            c2:close()
        end
    end)

    fan.loopbreak()
end)

os.exit(0)
]]

-- Shared child-process harness for the two crashers above. `script` runs in a
-- fresh interpreter; `marker` is what the child prints when close() aborted its
-- pending wait. A crash (signal or ASan abort) fails the test.
local function run_abort_guard(script, marker, what)
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(script)
    f:close()

    local cmd = TestFramework.child_lua_command(tmpfile, 20) .. "; echo $?"
    local handle = io.popen(cmd)
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    local exit_code = tonumber(output:match("(%d+)%s*$"))

    if exit_code == 77 then
        TestFramework.skip_test("MariaDB not reachable")
        return
    end

    -- The child runs through the shell to capture its status ("; echo $?"), so
    -- handle:close() always reports a normal exit: a crash arrives as 128+signal
    -- in exit_code (139 = SIGSEGV, 134 = SIGABRT) rather than as exit_type
    -- "signal", which is why it must be classified here too.
    local crashed = exit_code == 139 or exit_code == 134
        or exit_code == 11 or exit_code == 6
    -- An ASan build aborts with exit code 1 rather than 128+signal, so its crash
    -- is only visible in the captured output.
    local asan_crash = output:find("AddressSanitizer", 1, true) ~= nil
        and (output:find("SEGV", 1, true) ~= nil
             or output:find("use-after-free", 1, true) ~= nil
             or output:find("ABORTING", 1, true) ~= nil)
    local signal_code = code
    if crashed then
        signal_code = exit_code >= 128 and (exit_code - 128) or exit_code
    elseif asan_crash then
        crashed = true
        signal_code = 11
    end

    if (not success and exit_type == "signal") or crashed then
        print(string.format("Subprocess killed by signal %d", signal_code))
        if signal_code == 11 or signal_code == 6 then
            -- This used to be the BUG-CONFIRMED branch (the suite documented the
            -- defect by inducing it). The cancellation fix is in, so a signal here
            -- is a regression and must fail the suite.
            print("✗ REGRESSION: MariaDB pending wait use-after-free is back "
                  .. "(SIGSEGV/SIGABRT)")
            print("  Likely: *_cont callback accessed freed DB_STATUS/ctx after conn:close()")
            error(string.format(
                "mariadb_cancel_pending_waits() did not abort the pending %s "
                .. "(signal %d). Output:\n%s", what, signal_code, output))
        else
            error(string.format("Unexpected signal %d", signal_code))
        end
    elseif exit_code == 0 then
        -- A clean exit alone is not enough: it must also be visible that close()
        -- aborted the in-flight operation instead of the event never having been
        -- armed. The child prints `marker` only when the suspended call came back
        -- with (nil, error) during close(), i.e. when
        -- mariadb_cancel_pending_waits() resumed it.
        if output:find(marker, 1, true) then
            print(string.format(
                "Subprocess completed and close() aborted the pending %s (bug FIXED)", what))
            TestFramework.assert_true(true)
        else
            error(string.format(
                "No crash, but close() did not abort the pending %s -- the "
                .. "cancellation path did not run, or the event was never armed. "
                .. "Output:\n%s", what, output))
        end
    else
        error(string.format("Unexpected exit: code=%s, output:\n%s", tostring(exit_code), output))
    end
end

suite:test("subprocess_mariadb_pending_event_uaf", function()
    run_abort_guard(CRASH_SCRIPT, "PENDING_QUERY_ABORTED_ON_CLOSE", "query")
end)

-- Same race for a *prepared statement*, whose wait holds its coroutine reference
-- on the statement rather than on the connection: it covers the other branch of
-- mariadb_cancel_pending_waits().
suite:test("subprocess_mariadb_pending_stmt_execute_uaf", function()
    run_abort_guard(STMT_SCRIPT, "PENDING_STMT_ABORTED_ON_CLOSE", "statement execute")
end)

-- In-process connectivity test
suite:test("mariadb_connectivity_check", function()
    local conn
    local ok2, err = pcall(function()
        conn = mariadb.connect(DB_CONFIG.database, DB_CONFIG.user,
                               DB_CONFIG.password, DB_CONFIG.host, DB_CONFIG.port)
    end)
    if not ok2 or not conn then
        TestFramework.skip_test("MariaDB not reachable (start: cd tests && ./docker-setup.sh start)")
        return
    end
    -- Assert the handle is actually usable, not just that connect() returned:
    -- an unconditional assert_true(true) passes even if close() raises.
    local closed, close_err = pcall(function() conn:close() end)
    TestFramework.assert_true(closed,
        "close() on a freshly connected handle must succeed: " .. tostring(close_err))
end)

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    fan.loopbreak()
    os.exit(failed > 0 and 1 or 0)
end)
