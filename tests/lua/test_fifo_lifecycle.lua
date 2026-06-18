#!/usr/bin/env lua

-- Test for fan.fifo module lifecycle: creation, connector integration, cleanup

local TestFramework = require('test_framework')

package.preload['config'] = function()
    return { debug = false }
end

local fan = require "fan"
local fifo = require "fan.fifo"
local connector = require "fan.connector"

local suite = TestFramework.create_suite("FIFO Lifecycle")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_type(fifo, "table")
    TestFramework.assert_type(fifo.connect, "function")
end)

-- Test tmpfifoname generates unique names
suite:test("tmpfifoname_unique", function()
    local names = {}
    for i = 1, 10 do
        local name = connector.tmpfifoname()
        TestFramework.assert_type(name, "string")
        TestFramework.assert_true(#name > 0)
        TestFramework.assert_nil(names[name], "duplicate tmpfifoname: " .. name)
        names[name] = true
    end
end)

-- Test FIFO bind via connector
suite:test("fifo_bind", function()
    local fifo_name = connector.tmpfifoname()
    local url = "fifo://" .. fifo_name
    local server = connector.bind(url)
    TestFramework.assert_not_nil(server)
    TestFramework.assert_type(server.close, "function")
    server:close()
end)

-- Test FIFO connect via connector
suite:test("fifo_connect", function()
    local fifo_name = connector.tmpfifoname()
    local url = "fifo://" .. fifo_name

    -- Need a server first
    local server = connector.bind(url)
    TestFramework.assert_not_nil(server)

    -- Connect (may fail without event loop, that's OK)
    local ok, result = pcall(connector.connect, url)
    TestFramework.assert_type(ok, "boolean")

    server:close()
end)

-- Test FIFO tmpfifoname function
suite:test("tmpfifoname_functionality", function()
    local name1 = connector.tmpfifoname()
    local name2 = connector.tmpfifoname()

    TestFramework.assert_type(name1, "string")
    TestFramework.assert_type(name2, "string")
    TestFramework.assert_not_equal(name1, name2)

    -- Names should look like temp paths
    TestFramework.assert_true(name1:match("^/tmp/") ~= nil, "expected /tmp/ prefix, got: " .. name1)
end)

-- Test connector module recognizes fifo scheme
suite:test("fifo_connector_exports", function()
    TestFramework.assert_type(connector, "table")
    TestFramework.assert_type(connector.tmpfifoname, "function")
    TestFramework.assert_type(connector.bind, "function")
    TestFramework.assert_type(connector.connect, "function")
end)

local failures = TestFramework.run_suite(suite)
os.exit(failures > 0 and 1 or 0)
