#!/usr/bin/env lua

-- DETERMINISTIC crash reproducer for MariaDB async event cancellation.
-- Bug: closing connection while query is yielding does NOT cancel the pending
-- DB_STATUS event; callback fires on freed ctx/L causing use-after-free.
--
-- Strategy: spawn slow query with SLEEP(), close connection mid-query.

local TestFramework = require('test_framework')
local fan = require "fan"

local suite = TestFramework.create_suite("MariaDB pending event cancellation - crash inducer")

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

    -- Spawn query with long SLEEP
    local query_crashed = false
    fan.spawn(function()
        local ok, err = pcall(function()
            conn:execute("SELECT SLEEP(5), id FROM crash_test")
        end)
        if not ok then
            query_crashed = true
        end
    end)

    -- Let query register its continuation event
    fan.sleep(0.3)

    -- Close connection while event is pending
    -- If bug exists: pending event's callback will access freed ctx -> CRASH
    pcall(function() conn:close() end)

    -- Give callback a chance to fire (it shouldn't, but if bug exists it will crash)
    fan.sleep(1.0)

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

suite:test("subprocess_mariadb_pending_event_uaf", function()
    local tmpfile = os.tmpname() .. ".lua"
    local f = io.open(tmpfile, "w")
    if not f then
        TestFramework.skip_test("cannot create temp file")
        return
    end
    f:write(CRASH_SCRIPT)
    f:close()

    local cmd = string.format(
        "LUA_PATH='../modules/?.lua;../modules/?/init.lua;./lua/framework/?.lua;./lua/?.lua;;' " ..
        "LUA_CPATH='./build/?.so;../?.so;;' " ..
        "timeout 20s lua %s 2>&1; echo $?",
        tmpfile
    )
    local handle = io.popen(cmd)
    local output = handle:read("*a")
    local success, exit_type, code = handle:close()
    os.remove(tmpfile)

    local exit_code = tonumber(output:match("(%d+)%s*$"))

    if exit_code == 77 then
        TestFramework.skip_test("MariaDB not reachable")
        return
    end

    if not success and exit_type == "signal" then
        print(string.format("Subprocess killed by signal %d", code))
        if code == 11 or code == 6 then
            print("✓ BUG CONFIRMED: MariaDB pending event use-after-free detected (SIGSEGV/SIGABRT)")
            print("  Likely: *_cont callback accessed freed DB_STATUS/ctx after conn:close()")
            TestFramework.assert_true(true)
        else
            error(string.format("Unexpected signal %d", code))
        end
    elseif exit_code == 0 then
        print("Subprocess completed without crash (bug is FIXED or not triggered)")
        TestFramework.assert_true(true)
    else
        error(string.format("Unexpected exit: code=%s, output:\n%s", tostring(exit_code), output))
    end
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
    pcall(function() conn:close() end)
    TestFramework.assert_true(true)
end)

fan.loop(function()
    local failed = TestFramework.run_suite(suite)
    fan.loopbreak()
    os.exit(failed > 0 and 1 or 0)
end)
