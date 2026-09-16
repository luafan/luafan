#!/usr/bin/env lua

-- test_lock_granularity.lua
--
-- Verifies HOW the global Lua lock is provided and what it actually serialises.
-- There are two build shapes (see docs/threading-model.md, "Build shapes"):
--
--   core-hook : the interpreter was built with the lua_lock hook
--               (tests/build_hooked_lua.sh, LuanMac's -DLUA_USER_H=luauser.h).
--               lua_resume() holds the mutex, so each resume is protected and
--               the core's cooperative yield points (luai_threadyield inside
--               checkGC) stay reachable. luafan installs NO resume wrapper.
--   wrapper   : stock interpreter (lua_lock is a no-op). luafan wraps FAN_RESUME
--               itself; the extra level sits OUTSIDE lua_resume and is never
--               released by GC yield points, so whatever a coroutine does until
--               its next real yield is one indivisible critical section.
--
-- What is checked:
--   1. fan.diag_lock_mode() reports a real mode (never "none"/"single" once a
--      worker pool is running), and matches LUAN_LOCK_MODE_EXPECT when set.
--   2. Blocking, non-Lua work in a worker callback does not hold the lock
--      (fan.diag_lock_sleep(sec, false) -> N callbacks overlap), while taking it
--      around that work serialises them (keep_lock=true). On a hooked core the
--      core itself drops the lock around the C call; with the wrapper the
--      callback has to drop it.
--   3. Two workers cannot run Lua at the same time: the global lock, not the
--      threads, serialises Lua work (two non-yielding loops take ~2 loops).
--   4. Granularity: a worker running an allocating pure-Lua loop must not freeze
--      the main thread in core-hook mode, and must freeze it in wrapper mode.
--   5. Neither shape leaks lock depth on the main thread.
--   6. The fan.loop() hand-off has a real level to release (workers_init()'s hold
--      on the calling thread -- a hooked core included, it is not a no-op there)
--      and restores it on exit.
--
-- Requires: -DLUAFAN_TESTING=ON (fan.diag_lock_sleep), >= 4 event workers
-- (LUAN_TEST_WORKERS, default 4) and fan.tcpd. Skips (exit 77) otherwise.
--
--   cd tests && LUAN_TEST_WORKERS=4 lua lua/test_lock_granularity.lua
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

local function skip(reason)
    print("SKIP " .. reason)
    os.exit(77)
end

-- ------------------------------------------------------------- prerequisites
if type(fan.diag_lock_mode) ~= 'function' then
    skip("fan.diag_lock_mode missing (build older than the lock-mode diagnostics)")
end
if type(fan.diag_lock_sleep) ~= 'function' then
    skip("fan.diag_lock_sleep missing (configure with -DLUAFAN_TESTING=ON)")
end

-- The pool has to exist before the mode can mean anything: fan.diag_lock_mode() is
-- decided at workers_init(). Bodies run on workers 2, 3, ... while the echo
-- listener stays on worker 1, so 4 workers (0..3) is the minimum that keeps every
-- hop of the round-trip off the main thread. LUAN_TEST_WORKERS sizes the pool.
local WORKERS = tonumber(os.getenv and os.getenv("LUAN_TEST_WORKERS") or "") or 4
local wcount = (fan.worker_count and fan.worker_count()) or 0
if wcount < WORKERS and fan.workers_init then
    pcall(fan.workers_init, WORKERS)
    wcount = (fan.worker_count and fan.worker_count()) or 0
end
if wcount < 4 then
    skip("needs >= 4 event workers with distinct owner/pair workers (worker_count="
             .. tostring(wcount) .. "); set LUAN_TEST_WORKERS")
end
WORKERS = wcount

local ok_tcpd, tcpd = pcall(require, 'fan.tcpd')
if not ok_tcpd or type(tcpd) ~= 'table' or type(tcpd.bind) ~= 'function' then
    skip("fan.tcpd unavailable (" .. tostring(tcpd) .. ")")
end

local MODE = fan.diag_lock_mode()
local HAVE_DEPTH = type(fan.diag_lock_depth) == 'function'
info("worker pool", WORKERS)
info("lock mode", MODE)
info("main-thread lock depth", HAVE_DEPTH and fan.diag_lock_depth() or "n/a")

check("diag_lock_mode reports a real lock implementation",
      MODE == "core-hook" or MODE == "wrapper", MODE)

local expect = os.getenv and os.getenv("LUAN_LOCK_MODE_EXPECT")
if expect and #expect > 0 then
    check("lock mode matches LUAN_LOCK_MODE_EXPECT=" .. expect, MODE == expect, MODE)
end

-- ------------------------------------------------------------------- helpers
local TICK = 0.01
local tcpd_srv = nil
local tcpd_port = nil

-- fan.gettime() returns (seconds, microseconds) as two integers, so it cannot be
-- subtracted directly: fold it into one float here.
local function now()
    local s, us = fan.gettime()
    return s + us / 1e6
end

local function bind_echo()
    local srv = tcpd.bind({
        host = "127.0.0.1", port = 0,
        worker = 1,                      -- server side on worker 1
        callback_self_first = true,
        onaccept = function(_, apt)
            apt:bind{
                callback_self_first = true,
                onread = function(conn, buf) conn:send(buf) end,
                ondisconnected = function() end,
            }
        end,
    })
    if not srv then return nil end
    local p = srv:localinfo().port
    if type(p) ~= "number" or p <= 0 then return nil end
    tcpd_srv, tcpd_port = srv, p
    return srv
end

if not bind_echo() then
    skip("could not bind a worker-pinned echo listener")
end

-- Runs `body(i)` inside the client-side onread of `n` connections pinned to
-- distinct workers (2, 3, ...) while the main coroutine samples itself every
-- TICK seconds. Returns elapsed, the largest sampling gap, and the completion
-- count. The server echo runs on worker 1, so no hop of the round-trip needs
-- the main thread.
local function worker_round(n, body)
    local done = 0
    local conns = {}
    local t0 = now()
    local last = t0
    local max_gap = 0

    for i = 1, n do
        local conn = tcpd.connect({
            host = "127.0.0.1", port = tcpd_port,
            worker = 1 + i,              -- bodies on workers 2, 3, ...
            callback_self_first = true,
            onread = function(_, _buf)
                body(i)
                done = done + 1
            end,
            ondisconnected = function() end,
        })
        if conn then
            table.insert(conns, conn)
            conn:send("go")
        end
    end

    local deadline = now() + 15
    while done < #conns and now() < deadline do
        fan.sleep(TICK)
        local t = now()
        local gap = t - last
        if gap > max_gap then max_gap = gap end
        last = t
    end
    local elapsed = now() - t0

    for _, conn in ipairs(conns) do
        pcall(function() conn:close() end)
    end

    return elapsed, max_gap, done
end

-- A pure-Lua loop that allocates, so checkGC() (the only place where the core
-- hands the lock over between real yields) runs inside it. No C call, no yield.
local function busy_lua(iterations)
    local acc = 0
    for i = 1, iterations do
        local t = { i, i + 1, i + 2, i + 3 }
        acc = acc + (t[1] % 7)
    end
    return acc
end

-- Its non-allocating counterpart: no table churn, so no GC step and therefore no
-- yield point at all -- a worker running this holds the lock for the whole loop.
-- That is the case the two build shapes differ on most; see the spin check.
local function spin_lua(iterations)
    local acc = 0
    for i = 1, iterations do
        acc = acc + (i % 7)
    end
    return acc
end

-- Cost of one iteration of `fn`. os.clock() (CPU time, microsecond range) rather
-- than fan.gettime(): a few thousand table-building iterations can finish inside
-- one fan.gettime() tick, and a bogus zero would make the loop sections
-- meaningless. Doubles n until the sample is actually measurable (the cap is high
-- enough for the non-allocating spin loop, whose iterations are ~50x cheaper).
local function calibrate(fn)
    local n = 20000
    while true do
        local t0 = os.clock()
        fn(n)
        local dt = os.clock() - t0
        if dt >= 0.01 or n >= 40000000 then
            if dt <= 0 then dt = 1e-6 end
            return dt / n
        end
        n = n * 2
    end
end

local US_BUSY = calibrate(busy_lua)
local US_SPIN = calibrate(spin_lua)

local function iterations_for(us_per_iter, sec)
    return math.max(1000, math.floor(sec / us_per_iter))
end

-- Loop sizes for ~0.3s (allocating) and ~0.4s (non-allocating) per body.
local ITERS_BUSY = iterations_for(US_BUSY, 0.3)
local ITERS_SPIN = iterations_for(US_SPIN, 0.4)
local SPIN_SEC = 0.4
info("busy-loop cost", string.format("%.2f us/iteration (allocating) / %.3f us (spin)",
                                     US_BUSY * 1e6, US_SPIN * 1e6))

-- ------------------------------------------------------------------ the work
local SLEEP = 0.4

local function run_work()
    -- 1. Blocking, non-Lua work must give the lock up: two workers sleeping in C
    --    overlap, so the round takes one SLEEP, not two.
    local elapsed_release, _, done_release =
        worker_round(2, function() fan.diag_lock_sleep(SLEEP, false) end)
    check("both worker callbacks completed (lock released)", done_release == 2, done_release)
    check("C sleep without the lock runs in parallel (~" .. SLEEP .. "s, not " .. (2 * SLEEP) .. "s)",
          elapsed_release < SLEEP * 1.8, string.format("%.3fs", elapsed_release))

    -- 2. Taking the lock around the same blocking work serialises the callbacks:
    --    the other worker's resume has to wait for the mutex.
    local elapsed_hold, _, done_hold =
        worker_round(2, function() fan.diag_lock_sleep(SLEEP, true) end)
    check("both worker callbacks completed (lock held)", done_hold == 2, done_hold)
    check("C sleep while holding the lock serialises (~" .. (2 * SLEEP) .. "s)",
          elapsed_hold > SLEEP * 1.8, string.format("%.3fs", elapsed_hold))

    -- 3. Same with Lua, not C: a worker cannot run Lua while another worker holds
    --    the lock, so two non-yielding Lua loops take ~2 loops. This is the
    --    invariant both shapes must provide; only the *yield point* in between
    --    (and hence how long a critical section lasts) differs.
    local elapsed_spin, _, done_spin =
        worker_round(2, function() spin_lua(ITERS_SPIN) end)
    check("both worker callbacks completed (lua spin)", done_spin == 2, done_spin)
    check("simple lock: Lua work on two workers is serialised (~" .. (2 * SPIN_SEC) .. "s)",
          elapsed_spin > SPIN_SEC * 1.5, string.format("%.3fs", elapsed_spin))

    -- 4. Granularity of a pure-Lua worker loop, measured as the longest gap the
    --    main thread saw between its own scheduling ticks.
    local _, max_gap, done_busy = worker_round(2, function()
        busy_lua(ITERS_BUSY)
    end)
    check("both worker callbacks completed (lua loop)", done_busy == 2, done_busy)
    if MODE == "core-hook" then
        check("core-hook: a busy Lua worker leaves the main thread runnable (max gap < 0.15s)",
              max_gap < 0.15, string.format("%.3fs", max_gap))
    else
        check("wrapper: a busy Lua worker blocks the main thread for the whole loop (> 0.2s)",
              max_gap > 0.2, string.format("%.3fs", max_gap))
    end

    -- 5. No depth leak: the main thread must end where it started.
    if HAVE_DEPTH then
        local before = fan.diag_lock_depth()
        worker_round(1, function() fan.diag_lock_sleep(0.05, false) end)
        local after = fan.diag_lock_depth()
        check("main-thread lock depth unchanged by worker activity", before == after,
              tostring(before) .. " -> " .. tostring(after))
    end
end

-- 6. The fan.loop() hand-off has something to hand over, on BOTH shapes. Its
--    level is the extra one workers_init() took on the calling thread -- not a
--    resume level, which is exactly why a hooked core needs it as much as a
--    stock one: parking the loop while still holding it would block every worker
--    callback in LockMainState forever (which shows up below as checks 1-3
--    timing out, never passing). The exit check proves LuaLockResumeAfterLoop()
--    put the level back, so the enclosing resume's trailing unlock stays
--    balanced.
local entry_depth = HAVE_DEPTH and fan.diag_lock_depth() or nil
if HAVE_DEPTH then
    check("worker pool holds a lock level on the main thread before fan.loop()",
          entry_depth >= 1, entry_depth)
end

-- fan.sleep() -- and every other waiting fan API -- needs a coroutine, so the
-- measurements run inside the loop. Mode, pool and listener are already settled
-- above and stay outside; this also means the suite leaves nothing running.
fan.loop(function()
    local ok, err = pcall(run_work)
    if not ok then
        print("FAIL suite error: " .. tostring(err))
        failed = true
    end
    fan.loopbreak()
end)

if HAVE_DEPTH then
    local exit_depth = fan.diag_lock_depth()
    check("lock depth restored after fan.loop()", exit_depth == entry_depth,
          tostring(entry_depth) .. " -> " .. tostring(exit_depth))
end

if tcpd_srv and tcpd_srv.close then
    pcall(function() tcpd_srv:close() end)
end

print(failed and "RESULT: FAIL" or "RESULT: PASS")
os.exit(failed and 1 or 0)
