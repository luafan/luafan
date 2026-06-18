#!/usr/bin/env lua

-- Tests for fan.reliable_udp — chain_mt, init_conn, constants, apt_mt.
-- Does NOT require an event loop; tests data structures in isolation.

local TestFramework = require('test_framework')

package.preload['config'] = function()
    return { debug = false }
end

local reliable_udp = require 'fan.reliable_udp'

local suite = TestFramework.create_suite("Reliable UDP transport")

-- Test: module exports
suite:test("module_exports", function()
    TestFramework.assert_type(reliable_udp, "table")
    TestFramework.assert_type(reliable_udp.init_conn, "function")
    TestFramework.assert_type(reliable_udp.new_chain, "function")
    TestFramework.assert_type(reliable_udp.apt_mt, "table")
    TestFramework.assert_type(reliable_udp.chain_mt, "table")
    TestFramework.assert_type(reliable_udp.session_cache, "table")
end)

-- Test: constants are sane numbers
suite:test("constants", function()
    TestFramework.assert_type(reliable_udp.MTU, "number")
    TestFramework.assert_true(reliable_udp.MTU > 0)

    TestFramework.assert_type(reliable_udp.HEAD_SIZE, "number")
    TestFramework.assert_true(reliable_udp.HEAD_SIZE > 0)

    TestFramework.assert_type(reliable_udp.BODY_SIZE, "number")
    TestFramework.assert_true(reliable_udp.BODY_SIZE > 0)

    -- HEAD_SIZE + BODY_SIZE == MTU
    TestFramework.assert_equal(reliable_udp.MTU, reliable_udp.HEAD_SIZE + reliable_udp.BODY_SIZE)

    TestFramework.assert_type(reliable_udp.MAX_PAYLOAD_SIZE, "number")
    TestFramework.assert_true(reliable_udp.MAX_PAYLOAD_SIZE > reliable_udp.BODY_SIZE)

    TestFramework.assert_type(reliable_udp.TIMEOUT, "number")
    TestFramework.assert_true(reliable_udp.TIMEOUT > 0)

    TestFramework.assert_type(reliable_udp.WAITING_COUNT, "number")
    TestFramework.assert_true(reliable_udp.WAITING_COUNT > 0)

    TestFramework.assert_type(reliable_udp.NONE_PAIRED_WAITING_COUNT, "number")
    TestFramework.assert_true(reliable_udp.NONE_PAIRED_WAITING_COUNT > 0)

    TestFramework.assert_type(reliable_udp.UDP_WINDOW_SIZE, "number")
    TestFramework.assert_true(reliable_udp.UDP_WINDOW_SIZE > 0)

    TestFramework.assert_type(reliable_udp.CHECK_TIMEOUT_DURATION, "number")
    TestFramework.assert_true(reliable_udp.CHECK_TIMEOUT_DURATION > 0)

    TestFramework.assert_type(reliable_udp.MAX_OUTPUT_INDEX, "number")

    TestFramework.assert_type(reliable_udp.WINDOW_CTRL, "number")

    TestFramework.assert_type(reliable_udp.window_ctrl_head_set, "string")
    TestFramework.assert_type(reliable_udp.window_ctrl_head_req, "string")
end)

-- Test: new_chain creates empty chain
suite:test("new_chain_empty", function()
    local chain = reliable_udp.new_chain()
    TestFramework.assert_not_nil(chain)
    TestFramework.assert_equal(false, chain:hasmore())
    TestFramework.assert_equal(0, chain.size)
    TestFramework.assert_nil(chain._head)
    TestFramework.assert_nil(chain._tail)
end)

-- Helper: create a mock apt for pop() window checks
local function make_mock_apt()
    local t = {}
    reliable_udp.init_conn(t)
    return t
end

-- Helper: create a mock package that can go through pop() window checks
local function make_package(data, apt)
    apt = apt or make_mock_apt()
    return {
        data = data,
        apt = apt,
        output_index = apt._output_index,
        head = string.pack("<I4I2I2", 0, 1, 1),
    }
end

-- Test: chain push/pop FIFO order
suite:test("chain_push_pop_fifo", function()
    local chain = reliable_udp.new_chain()

    chain:push(make_package("a"))
    chain:push(make_package("b"))
    chain:push(make_package("c"))

    TestFramework.assert_equal(3, chain.size)
    TestFramework.assert_true(chain:hasmore())

    local p1 = chain:pop()
    TestFramework.assert_equal("a", p1.data)

    local p2 = chain:pop()
    TestFramework.assert_equal("b", p2.data)

    local p3 = chain:pop()
    TestFramework.assert_equal("c", p3.data)

    TestFramework.assert_equal(0, chain.size)
    TestFramework.assert_false(chain:hasmore())
    TestFramework.assert_nil(chain:pop())
end)

-- Test: chain inserthead reverses order
suite:test("chain_inserthead_order", function()
    local chain = reliable_udp.new_chain()
    local apt = make_mock_apt()

    chain:inserthead(make_package("a", apt))
    chain:inserthead(make_package("b", apt))
    chain:inserthead(make_package("c", apt))

    TestFramework.assert_equal(3, chain.size)

    -- inserthead puts at front, so order is c, b, a
    local p1 = chain:pop()
    TestFramework.assert_equal("c", p1.data)

    local p2 = chain:pop()
    TestFramework.assert_equal("b", p2.data)

    local p3 = chain:pop()
    TestFramework.assert_equal("a", p3.data)
end)

-- Test: chain mixed push/inserthead
suite:test("chain_mixed_push_inserthead", function()
    local chain = reliable_udp.new_chain()
    local apt = make_mock_apt()

    chain:push(make_package("first", apt))
    chain:inserthead(make_package("zero", apt))
    chain:push(make_package("second", apt))

    TestFramework.assert_equal(3, chain.size)

    -- zero (inserthead), first (push), second (push)
    TestFramework.assert_equal("zero", chain:pop().data)
    TestFramework.assert_equal("first", chain:pop().data)
    TestFramework.assert_equal("second", chain:pop().data)
end)

-- Test: chain pop on empty returns nil
suite:test("chain_pop_empty", function()
    local chain = reliable_udp.new_chain()
    TestFramework.assert_nil(chain:pop())
end)

-- Test: chain inserthead on empty
suite:test("chain_inserthead_empty", function()
    local chain = reliable_udp.new_chain()
    chain:inserthead(make_package("only"))
    TestFramework.assert_equal(1, chain.size)
    TestFramework.assert_equal("only", chain:pop().data)
    TestFramework.assert_nil(chain:pop())
end)

-- Test: chain size tracking
suite:test("chain_size_tracking", function()
    local chain = reliable_udp.new_chain()
    local apt = make_mock_apt()
    TestFramework.assert_equal(0, chain.size)

    chain:push(make_package("a", apt))
    TestFramework.assert_equal(1, chain.size)

    chain:push(make_package("b", apt))
    TestFramework.assert_equal(2, chain.size)

    chain:inserthead(make_package("x", apt))
    TestFramework.assert_equal(3, chain.size)

    chain:pop()
    TestFramework.assert_equal(2, chain.size)

    chain:pop()
    TestFramework.assert_equal(1, chain.size)

    chain:pop()
    TestFramework.assert_equal(0, chain.size)
end)

-- Test: chain pop with ack-bypass packages (skips window check)
suite:test("chain_pop_ack_bypass", function()
    local chain = reliable_udp.new_chain()

    -- Packages with ack=true bypass the window check in pop()
    chain:push({data = "pkg1", ack = true})
    chain:push({data = "pkg2", ack = true})
    chain:push({data = "pkg3", ack = true})

    TestFramework.assert_equal("pkg1", chain:pop().data)
    TestFramework.assert_equal("pkg2", chain:pop().data)
    TestFramework.assert_equal("pkg3", chain:pop().data)
end)

-- Test: init_conn sets up connection state
suite:test("init_conn_state", function()
    local t = {}
    reliable_udp.init_conn(t)

    -- Should have apt_mt as metatable
    TestFramework.assert_equal(reliable_udp.apt_mt, getmetatable(t))

    -- Output chain should be set up
    TestFramework.assert_not_nil(t._output_chain)
    TestFramework.assert_equal(0, t._output_chain.size)

    -- Counters should be zero
    TestFramework.assert_equal(0, t.output_chain_count)
    TestFramework.assert_equal(0, t._output_wait_count)
    TestFramework.assert_equal(0, t.incoming_bytes_total)
    TestFramework.assert_equal(0, t.outgoing_bytes_total)
    TestFramework.assert_equal(0, t.udp_send_total)
    TestFramework.assert_equal(0, t.udp_receive_total)
    TestFramework.assert_equal(0, t.udp_resend_total)
    TestFramework.assert_equal(0, t.udp_drop_total)
    TestFramework.assert_equal(1, t.reuse)

    -- Maps should be empty tables
    TestFramework.assert_type(t._output_wait_ack, "table")
    TestFramework.assert_type(t._output_package_parts_map, "table")
    TestFramework.assert_type(t._incoming_map, "table")
    TestFramework.assert_type(t._recv_window_holes, "table")
    TestFramework.assert_type(t._send_window_holes, "table")

    -- Window and index should be numbers
    TestFramework.assert_type(t._output_index, "number")
    TestFramework.assert_type(t._send_window, "number")
end)

-- Test: init_conn with _parent shares parent's chain
suite:test("init_conn_with_parent", function()
    local parent = {}
    reliable_udp.init_conn(parent)
    -- In bind(), _main_output_chain is set on the server object,
    -- separate from init_conn's _output_chain
    parent._main_output_chain = reliable_udp.new_chain()

    local child = { _parent = parent }
    reliable_udp.init_conn(child)

    -- Child should share parent's main output chain
    TestFramework.assert_equal(parent._main_output_chain, child._output_chain)
    TestFramework.assert_not_nil(child._suspend_list)
end)

-- Test: apt_mt has expected methods
suite:test("apt_mt_methods", function()
    local mt = reliable_udp.apt_mt
    TestFramework.assert_type(mt.send, "function")
    TestFramework.assert_type(mt.send_package, "function")
    TestFramework.assert_type(mt._send_package, "function")
    TestFramework.assert_type(mt.send_req, "function")
    TestFramework.assert_type(mt._moretosend, "function")
    TestFramework.assert_type(mt._mark_send_completed, "function")
    TestFramework.assert_type(mt._apply_send_window, "function")
    TestFramework.assert_type(mt._apply_recv_window, "function")
    TestFramework.assert_type(mt._onack, "function")
    TestFramework.assert_type(mt._onread, "function")
    TestFramework.assert_type(mt._send, "function")
    TestFramework.assert_type(mt._check_timeout, "function")
    TestFramework.assert_type(mt.cleanup, "function")
    TestFramework.assert_type(mt.close, "function")
end)

-- Test: init_conn with _parent uses suspend_list
suite:test("init_conn_parent_suspend_list", function()
    local parent = {}
    reliable_udp.init_conn(parent)
    parent._main_output_chain = reliable_udp.new_chain()

    local child = { _parent = parent }
    reliable_udp.init_conn(child)

    -- Child's _suspend_list should be an empty table
    TestFramework.assert_type(child._suspend_list, "table")
    -- Child's _output_chain should be parent's main chain
    TestFramework.assert_equal(parent._main_output_chain, child._output_chain)
end)

-- Test: chain large batch push/pop
suite:test("chain_large_batch", function()
    local chain = reliable_udp.new_chain()
    local apt = make_mock_apt()
    local count = 1000

    for i = 1, count do
        chain:push(make_package(i, apt))
    end

    TestFramework.assert_equal(count, chain.size)

    for i = 1, count do
        local p = chain:pop()
        TestFramework.assert_not_nil(p)
        TestFramework.assert_equal(i, p.data)
    end

    TestFramework.assert_equal(0, chain.size)
    TestFramework.assert_false(chain:hasmore())
end)

-- Test: _moretosend on fresh connection
suite:test("moretosend_fresh", function()
    local t = {}
    reliable_udp.init_conn(t)
    -- Fresh connection has nothing to send
    TestFramework.assert_false(t:_moretosend())
end)

-- Run
local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
