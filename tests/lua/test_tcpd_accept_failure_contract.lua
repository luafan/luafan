#!/usr/bin/env lua

-- test_tcpd_accept_failure_contract.lua
--
-- Validates the failed-accept contract of tcpd.bind (docs/api/tcpd.md
-- "onaccept"): when the kernel hands over a connection that luafan cannot set
-- up locally, the onaccept callback must still be invoked with the arity of the
-- success path and a nil connection:
--
--   callback_self_first = true  -> (server, nil)
--   callback_self_first = false -> (nil)
--
-- A bare nil in callee position is a bug: lua_resume() takes the callee from
-- the slot directly BELOW the argument block, so leaving the accept userdata
-- (or any extra slot) between the callee and the arguments makes Lua call the
-- wrong value -- the historical failure was
--   Error: attempt to call a <tcpd.accept %s %d> value
-- and, with the accept userdata already dropped, a 1-argument resume handed the
-- user callback (nil) so `apt:bind{...}` raised
--   attempt to index a nil value (local 'apt').
--
-- The failure path is not reachable on demand from Lua, so it is exercised with
-- a scratch build that forces it (same lever as test_httpd_async_teardown.lua
-- scenario E):
--
--   cmake -S . -B /tmp/inject -DCMAKE_C_FLAGS=-DTCPD_ACCEPT_FAIL_INJECT_EVERY=1
--   LUA_CPATH='/tmp/inject/?.so;;' lua lua/test_tcpd_accept_failure_contract.lua
--
-- Without the injection no accept fails and this file exits 77 (SKIP), so it is
-- safe to run in an ordinary regression sweep.
--
-- Exit codes: 0 = pass, 1 = fail, 77 = skipped.

local fan = require 'fan'
local tcpd = require 'fan.tcpd'

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

local CLIENTS = 3

-- Collect one server's accepts: arity and argument types.
local function accepts_of(self_first, out, label)
    local srv = tcpd.bind({
        host = "127.0.0.1", port = 0,
        callback_self_first = self_first,
        onaccept = function(...)
            local argc = select('#', ...)
            local first, second = ...
            out.calls = out.calls + 1
            out.arity[argc] = (out.arity[argc] or 0) + 1
            if self_first then
                if first == nil then out.self_nil = out.self_nil + 1 end
                if second == nil then
                    out.failed = out.failed + 1
                else
                    out.bound = out.bound + 1
                    second:bind{
                        onread = function(conn, buf) conn:send(buf) end,
                        ondisconnected = function() end,
                    }
                end
            else
                -- self-less signature: the connection is the first argument
                if first == nil then
                    out.failed = out.failed + 1
                else
                    out.bound = out.bound + 1
                    first:bind{
                        onread = function(conn, buf) conn:send(buf) end,
                        ondisconnected = function() end,
                    }
                end
            end
        end,
    })
    if not srv or not srv:localinfo() then
        return nil, label .. ": tcpd.bind failed"
    end
    return srv, nil
end

local function run()
    local with_self =
        { calls = 0, bound = 0, failed = 0, self_nil = 0, arity = {}, disc = 0 }
    local no_self =
        { calls = 0, bound = 0, failed = 0, self_nil = 0, arity = {}, disc = 0 }

    local srv_a, err_a = accepts_of(true, with_self, "self-first server")
    local srv_b, err_b = accepts_of(false, no_self, "self-less server")
    check("both servers bound", srv_a ~= nil and srv_b ~= nil,
          tostring(err_a) .. " / " .. tostring(err_b))
    if not (srv_a and srv_b) then return end

    local held = {}
    local function connect_to(port)
        local conn = tcpd.connect({
            host = "127.0.0.1", port = port,
            onread = function() end,
            ondisconnected = function() end,
        })
        if conn then held[#held + 1] = conn end
    end
    local pa = srv_a:localinfo().port
    local pb = srv_b:localinfo().port
    for _ = 1, CLIENTS do
        connect_to(pa)
        connect_to(pb)
    end
    fan.sleep(0.5)

    info("self-first accepts", with_self.calls)
    info("self-first failed accepts", with_self.failed)
    info("self-less accepts", no_self.calls)
    info("self-less failed accepts", no_self.failed)

    if with_self.failed == 0 and no_self.failed == 0 then
        print("SKIP no failed accept observed (build without TCPD_ACCEPT_FAIL_INJECT_EVERY)")
        for _, c in ipairs(held) do pcall(function() c:close() end) end
        pcall(function() srv_a:close() end)
        pcall(function() srv_b:close() end)
        os.exit(77)
    end

    -- self-first: arity must stay 2, self must be the server object
    check("self-first: every accept delivered arity 2",
          (with_self.arity[2] or 0) == with_self.calls,
          "arity1=" .. tostring(with_self.arity[1] or 0)
          .. " arity2=" .. tostring(with_self.arity[2] or 0)
          .. " calls=" .. with_self.calls)
    check("self-first: server object never nil", with_self.self_nil == 0,
          with_self.self_nil .. " call(s)")
    check("self-first: failed accepts passed a nil connection",
          with_self.failed == with_self.calls,
          "failed=" .. with_self.failed .. " calls=" .. with_self.calls)

    -- self-less: arity must stay 1 and the single argument must be nil
    check("self-less: every accept delivered arity 1",
          (no_self.arity[1] or 0) == no_self.calls,
          "arity1=" .. tostring(no_self.arity[1] or 0)
          .. " arity2=" .. tostring(no_self.arity[2] or 0)
          .. " calls=" .. no_self.calls)
    check("self-less: failed accepts passed a nil connection",
          no_self.failed == no_self.calls,
          "failed=" .. no_self.failed .. " calls=" .. no_self.calls)

    for _, c in ipairs(held) do pcall(function() c:close() end) end
    pcall(function() srv_a:close() end)
    pcall(function() srv_b:close() end)
end

fan.loop(function()
    local ok, err = pcall(run)
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
