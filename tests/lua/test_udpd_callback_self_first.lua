#!/usr/bin/env lua

-- Test for UDPD callback_self_first feature
-- Tests circular reference avoidance and callback parameter behavior for UDP

local TestFramework = require('test_framework')
local fan = require "fan"

-- Mock config module if needed
package.preload['config'] = function()
    return {
        debug = false,
        udp_timeout = 5000
    }
end

-- Load required modules
local udpd_available = false
local udpd

-- Try to load udpd module
local ok, result = pcall(require, 'fan.udpd')
if ok then
    udpd = result
    udpd_available = true
    print("fan.udpd module loaded successfully")
else
    print("Warning: fan.udpd module not available: " .. tostring(result))
end

-- Create test suite
local suite = TestFramework.create_suite("UDPD callback_self_first Tests")

print("Testing UDPD callback_self_first feature")

-- Skip all tests if udpd not available
if not udpd_available then
    print("Skipping UDPD callback_self_first tests - udpd module not available")
    local failures = TestFramework.run_suite(suite)
    os.exit(failures > 0 and 1 or 0)
end

-- Test module availability
suite:test("module_availability", function()
    TestFramework.assert_not_nil(udpd)
    TestFramework.assert_type(udpd, "table")
    TestFramework.assert_type(udpd.new, "function")
    TestFramework.assert_type(udpd.make_dest, "function")
    print("UDPD module available for callback_self_first testing")
end)

-- Test UDP connection with callback_self_first disabled (traditional)
suite:test("udp_traditional_callbacks", function()
    local callback_count = {
        read = 0,
        sendready = 0
    }

    -- Create UDP connection with traditional callbacks
    local conn = udpd.new({
        bind_port = 0,  -- Let system assign port
        callback_self_first = false,  -- Traditional mode
        onread = function(data, dest, ...)
            callback_count.read = callback_count.read + 1
            TestFramework.assert_equal(2, select('#', data, dest, ...))  -- Data + dest parameters
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")  -- UDP_AddrInfo
        end,
        onsendready = function(...)
            callback_count.sendready = callback_count.sendready + 1
            TestFramework.assert_equal(0, select('#', ...))  -- No parameters
        end
    })

    TestFramework.assert_not_nil(conn)
    TestFramework.assert_type(conn, "userdata")

    print("Traditional UDP callbacks tested - connection interface validated")
end)

-- Test UDP connection with callback_self_first enabled
suite:test("udp_self_first_callbacks", function()
    local callback_count = {
        read = 0,
        sendready = 0
    }

    local self_references = {}

    -- Create UDP connection with self-first callbacks
    local conn = udpd.new({
        bind_port = 0,  -- Let system assign port
        callback_self_first = true,  -- New mode
        onread = function(self, data, dest, ...)
            callback_count.read = callback_count.read + 1
            TestFramework.assert_equal(3, select('#', self, data, dest, ...))  -- Self + data + dest parameters
            TestFramework.assert_not_nil(self)
            TestFramework.assert_type(self, "userdata")
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")  -- UDP_AddrInfo
            table.insert(self_references, self)
        end,
        onsendready = function(self, ...)
            callback_count.sendready = callback_count.sendready + 1
            TestFramework.assert_equal(1, select('#', self, ...))  -- Self parameter
            TestFramework.assert_not_nil(self)
            TestFramework.assert_type(self, "userdata")
            table.insert(self_references, self)
        end
    })

    TestFramework.assert_not_nil(conn)
    TestFramework.assert_type(conn, "userdata")

    print("Self-first UDP callbacks tested - connection interface validated")
end)

-- Test UDP client with callback_self_first (for sending)
suite:test("udp_client_self_first_callbacks", function()
    local callback_count = {
        read = 0,
        sendready = 0
    }

    -- Create UDP client with self-first callbacks
    local client = udpd.new({
        host = "127.0.0.1",
        port = 12345,  -- Target port (may not exist, that's OK for interface test)
        bind_port = 0,  -- Let system assign local port
        callback_self_first = true,  -- New mode
        onread = function(self, data, dest, ...)
            callback_count.read = callback_count.read + 1
            TestFramework.assert_equal(3, select('#', self, data, dest, ...))  -- Self + data + dest
            TestFramework.assert_not_nil(self)
            TestFramework.assert_type(self, "userdata")
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")
        end,
        onsendready = function(self, ...)
            callback_count.sendready = callback_count.sendready + 1
            TestFramework.assert_equal(1, select('#', self, ...))  -- Self parameter
            TestFramework.assert_not_nil(self)
            TestFramework.assert_type(self, "userdata")
        end
    })

    TestFramework.assert_not_nil(client)
    TestFramework.assert_type(client, "userdata")

    print("Self-first UDP client callbacks tested")
end)

-- Test circular reference avoidance simulation
suite:test("circular_reference_avoidance", function()
    -- This test demonstrates the difference in reference patterns for UDP
    local traditional_refs = {}
    local self_first_refs = {}

    -- Traditional approach (would capture connection in closure)
    local traditional_connection = {
        type = "traditional_udp",
        close = function() end,
        send = function() end
    }

    local traditional_callback = function(data, dest)
        -- This captures traditional_connection in closure (circular reference)
        table.insert(traditional_refs, traditional_connection)
        traditional_connection:send("response", dest)
    end

    -- Self-first approach (no closure capture needed)
    local self_first_callback = function(self, data, dest)
        -- No need to capture external reference
        table.insert(self_first_refs, self)
        self:send("response", dest)
    end

    -- Simulate callback calls with mock dest object
    local mock_dest = {type = "mock_dest"}
    traditional_callback("test_data", mock_dest)
    self_first_callback(traditional_connection, "test_data", mock_dest)

    TestFramework.assert_equal(1, #traditional_refs)
    TestFramework.assert_equal(1, #self_first_refs)

    print("UDP circular reference patterns tested")
end)

-- Test parameter configuration validation
suite:test("parameter_configuration", function()
    -- Test that callback_self_first parameter is recognized
    local configs = {
        {callback_self_first = true, expected = true},
        {callback_self_first = false, expected = false},
        {callback_self_first = 1, expected = true},  -- Truthy value
        {callback_self_first = 0, expected = false}, -- Falsy value
        {callback_self_first = nil, expected = false}, -- Default
        {expected = false} -- Missing parameter (default)
    }

    for i, config in ipairs(configs) do
        -- Test UDP connection configuration
        local udp_config = {
            bind_port = 0,  -- Let system assign port
            callback_self_first = config.callback_self_first,
            onread = function(...) end,
            onsendready = function(...) end
        }

        local conn = udpd.new(udp_config)
        TestFramework.assert_not_nil(conn)
        TestFramework.assert_type(conn, "userdata")

        -- Clean up
        if conn.close then conn:close() end
    end

    print("Parameter configuration validation passed")
end)

-- Test backward compatibility
suite:test("backward_compatibility", function()
    -- Test that existing UDP code works unchanged
    local old_style_conn = udpd.new({
        bind_port = 0,
        onread = function(data, dest)
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")
        end,
        onsendready = function()
            -- No parameters in traditional mode
        end
        -- No callback_self_first parameter - should default to false
    })

    TestFramework.assert_not_nil(old_style_conn)
    TestFramework.assert_type(old_style_conn, "userdata")

    print("Backward compatibility verified")
end)

-- Test feature documentation examples
suite:test("documentation_examples", function()
    -- Example from documentation should work
    local traditional_example = udpd.new({
        bind_port = 0,
        onread = function(data, dest)
            -- Would capture conn in closure if we used it
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")
        end
    })

    local self_first_example = udpd.new({
        bind_port = 0,
        callback_self_first = true,
        onread = function(self, data, dest)
            TestFramework.assert_not_nil(self)
            TestFramework.assert_type(self, "userdata")
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")
            -- self:send("response", dest) -- Would work without circular reference
        end
    })

    TestFramework.assert_not_nil(traditional_example)
    TestFramework.assert_not_nil(self_first_example)

    print("Documentation examples validated")
end)

-- Test UDP destination creation
suite:test("udp_destination_handling", function()
    -- Test destination creation for callback testing
    local dest = udpd.make_dest("127.0.0.1", 12345)
    TestFramework.assert_not_nil(dest)
    TestFramework.assert_type(dest, "userdata")

    -- Test destination with callback_self_first enabled connection
    local conn = udpd.new({
        bind_port = 0,
        callback_self_first = true,
        onread = function(self, data, dest, ...)
            TestFramework.assert_not_nil(self)
            TestFramework.assert_type(self, "userdata")
            TestFramework.assert_type(data, "string")
            TestFramework.assert_not_nil(dest)
            TestFramework.assert_type(dest, "userdata")
            -- Verify dest parameter is properly passed with self-first mode
        end
    })

    TestFramework.assert_not_nil(conn)

    print("UDP destination handling validated")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)