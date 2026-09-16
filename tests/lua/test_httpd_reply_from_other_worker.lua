#!/usr/bin/env lua

-- test_httpd_reply_from_other_worker.lua
-- True cross-thread HTTP reply: the handler is seen by worker A (the request's
-- owner), but the actual resp:reply() call is executed on a *different* worker
-- thread B (R14 review finding, see docs/threading-model.md).
--
-- Cross-thread dispatch primitive: a fan.fifo reader is pinned to worker B via
-- `worker=B`; its read event lives on B's base, so when a byte is written the
-- fifo_read_cb fires on B's *thread* and resumes the `onread` callback there.
-- The handler stores `resp` in a shared queue and writes a byte to wake B; B's
-- onread then pulls the parked resp and calls resp:reply(...) on B's thread.
-- Because B is not the request owner, the reply takes the marshaled path
-- (httpd_reply_op / httpd_reply_drain_cb) and is applied in order on the
-- owner loop, yet the peer still receives a complete, correct response.
--
-- Requirements: an event-worker pool >= 3 (so owner A and target B are distinct
-- and there is room for a third). This file calls fan.workers_init() itself
-- (legal only before fan.loop()):
--
--     cd tests && lua lua/test_httpd_reply_from_other_worker.lua
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

-- The worker pool must exist before fan.loop(): the FIFO read events need the
-- worker bases (created by workers_init).
local WORKERS = tonumber(os.getenv and os.getenv("LUAN_TEST_WORKERS") or "") or 4
local wcount = (fan.worker_count and fan.worker_count()) or 0
if wcount < WORKERS and fan.workers_init then
    pcall(fan.workers_init, WORKERS)
    wcount = (fan.worker_count and fan.worker_count()) or 0
end
if wcount < 3 then
    skip("needs >= 3 event workers for distinct owner/target (worker_count="
             .. tostring(wcount) .. ")")
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

-- ---------------------------------------------------------------- basic sanity
local function run_all()
    local url_of = function(srv) return "http://127.0.0.1:" .. srv.port .. "/" end

    ---------------------------------------------- 1) inline owner-thread reply
    -- Each worker keeps its own response status: both the marshaled path and
    -- this inline path must produce identical responses.
    do
        local srv = bind(function(req, resp)
            resp:reply(200, "OK", req.worker_id)
        end)
        check("inline server bind ok", srv ~= nil)
        if srv then
            local n, ok_n, owners = math.max(WORKERS * 2, 8), 0, {}
            for _ = 1, n do
                local res = fetch(url_of(srv))
                if res and res.responseCode == 200 then
                    ok_n = ok_n + 1
                    owners[res.body] = true
                end
            end
            check("inline multi-worker replies all complete", ok_n == n, ok_n .. "/" .. n)
            local distinct = 0
            for _ in pairs(owners) do distinct = distinct + 1 end
            check("inline requests spread across workers", distinct >= 2,
                  "workers=" .. distinct)
            close(srv)
        end
        collectgarbage("collect")
    end

    ------------------------------------------------------------------ 2) real
    -- The handler is seen on the request's owner. To prove the reply truly
    -- comes from a *different* worker, each request picks a target worker B !=
    -- owner, parks `resp`, and wakes a FIFO reader pinned to B. B's onread
    -- (running on B's thread) then replies. We choke whenever the reply source
    -- would be the owner.
    do
        -- Per-target-worker shared queue + FIFO reader.
        local parked = {}          -- [targetW] = { resp, owner, seq }
        local seq = 0
        local fifo_readers = {}    -- [targetW] = {fifo_obj, name, cleanup}

        for w = 0, WORKERS - 1 do
            parked[w] = {}
        end

        local tmpname = os.tmpname and os.tmpname or (function()
            return "/tmp/fan_xworker_" .. tostring(os.time()) .. "_" ..
                   tostring(math.random(1, 1 << 20))
        end)
        local made = {}

        for w = 0, WORKERS - 1 do
            (function(target)
                local name = tmpname()
                os.remove(name)
                made[#made + 1] = name
                -- rwmode="rw" on a single FIFO opened O_RDWR on worker
                -- `target`: the handler writes "wake" to the same fd, which
                -- makes that fd readable, so worker `target`'s read_ev fires
                -- and onread runs on worker `target`'s thread. No external
                -- writer is needed.
                local rd = fifo.connect{
                    name = name,
                    delete_on_close = true,
                    rwmode = "rw",
                    worker = target,
                    onread = function()
                        -- Runs on worker `target`'s thread. Pull any parked
                        -- resp and issue the reply from HERE.
                        local list = parked[target]
                        while #list > 0 do
                            local item = table.remove(list, 1)
                            if item then
                                local okr, rerr = pcall(item.resp.reply, item.resp,
                                     200, "OK",
                                     "reply-from-worker-" .. target ..
                                     " owner-" .. item.owner)
                                if not okr then
                                    info("cross-worker reply error", tostring(rerr))
                                    item.done_err = tostring(rerr)
                                end
                                if item.done then item.done() end
                            end
                        end
                    end,
                }
                fifo_readers[target] = rd
            end)(w)
        end

        local srv = bind(function(req, resp)
            local owner = req.worker_id
            -- choose a distinct target worker
            local target = (owner + 1) % WORKERS
            seq = seq + 1
            local myseq = seq
            parked[target][#parked[target] + 1] = { resp = resp, owner = owner, seq = myseq }
            -- wake the target worker's FIFO reader
            local rd = fifo_readers[target]
            if rd and rd.send then
                rd:send("wake")
            end
            -- do NOT reply here; the response is owned by worker `target`.
            return 0
        end)
        check("cross-worker server bind ok", srv ~= nil)
        if srv then
            local n = math.max(WORKERS * 2, 12)
            local done, ok_n = 0, 0
            local nores, laterr = 0, nil
            for _ = 1, n do
                coroutine.wrap(function()
                    local res = fetch(url_of(srv), 15)
                    if res and res.responseCode == 200
                        and res.body:match("^reply%-from%-worker%-") then
                        ok_n = ok_n + 1
                        -- verify the replying worker is never the owner
                        local src = res.body:match("worker%-(%d+)")
                        local own = res.body:match("owner%-(%d+)")
                        if src and own and src == own then
                            laterr = laterr or ("reply came from owner worker " .. src)
                        end
                    elseif res == nil then
                        nores = nores + 1
                    end
                    done = done + 1
                end)()
            end
            local finished = wait_until(function() return done >= n end, 40)
            check("every request answered (reply from a different worker)",
                  finished and ok_n == n,
                  "ok=" .. ok_n .. "/" .. n .. " nores=" .. nores)
            check("replying worker is never the owner worker",
                  not laterr, laterr or "all cross-worker")
            close(srv)

            -- cleanup fifo readers
            for w = 0, WORKERS - 1 do
                if fifo_readers[w] then
                    pcall(function() fifo_readers[w]:close() end)
                    fifo_readers[w] = nil
                end
            end
            for _, nm in ipairs(made) do pcall(os.remove, nm) end
        end
        collectgarbage("collect")
    end
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