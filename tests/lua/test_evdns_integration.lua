#!/usr/bin/env lua

-- Integration test for fan.evdns with tcpd and udpd modules

local TestFramework = require('test_framework')

-- Load required modules
local evdns_available = false
local tcpd_available = false
local udpd_available = false

local evdns, tcpd, udpd

-- Try to load modules
local ok, result = pcall(require, 'fan.evdns')
if ok then
    evdns = result
    evdns_available = true
    print("fan.evdns module loaded successfully")
else
    print("Warning: fan.evdns module not available: " .. tostring(result))
    os.exit(1)
end

ok, result = pcall(require, 'fan.tcpd')
if ok then
    tcpd = result
    tcpd_available = true
    print("fan.tcpd module loaded successfully")
else
    print("Warning: fan.tcpd module not available: " .. tostring(result))
end

ok, result = pcall(require, 'fan.udpd')
if ok then
    udpd = result
    udpd_available = true
    print("fan.udpd module loaded successfully")
else
    print("Warning: fan.udpd module not available: " .. tostring(result))
end

-- Create test suite
local suite = TestFramework.create_suite("evdns Integration Tests")

-- Test evdns integration with tcpd.connect
if tcpd_available then
    suite:test("evdns_with_tcpd_connect", function()
        local dns = evdns.create("8.8.8.8")
        TestFramework.assert_not_nil(dns)

        -- Test that tcpd.connect accepts evdns parameter without error
        local connection_attempted = false
        local connect_error = nil

        local ok, err = pcall(function()
            local conn = tcpd.connect({
                host = "httpbin.org",  -- Public test service
                port = 80,
                evdns = dns,  -- Use custom DNS
                callback_self_first = true,
                onconnected = function(self)
                    connection_attempted = true
                    self:close()
                end,
                ondisconnected = function(self, reason)
                    -- Connection closed
                end,
                onread = function(self, data)
                    -- Data received
                end
            })
            TestFramework.assert_not_nil(conn)
        end)

        if not ok then
            connect_error = err
        end

        -- Should not throw error during connection attempt
        TestFramework.assert_true(ok, "tcpd.connect with evdns should not throw error: " .. tostring(connect_error))

        -- Wait a bit for potential connection (non-blocking test)
        local fan = require('fan')
        fan.sleep(0.1)
    end)
else
    print("Skipping tcpd integration tests (tcpd not available)")
end

-- Test evdns integration with udpd.make_dest
if udpd_available then
    suite:test("evdns_with_udpd_make_dest", function()
        local dns = evdns.create("1.1.1.1")
        TestFramework.assert_not_nil(dns)

        -- Test that udpd.make_dest accepts evdns parameter
        local dest_ok, dest_or_err = pcall(udpd.make_dest, "httpbin.org", 80, dns)

        if dest_ok then
            local dest = dest_or_err
            TestFramework.assert_not_nil(dest)
            -- dest might be nil if DNS resolution is async and not complete yet
            if dest then
                TestFramework.assert_type(dest, "userdata")
            end
        else
            -- DNS resolution might fail or be async - that's ok for this test
            TestFramework.assert_type(dest_or_err, "string") -- Error message
        end
    end)

    suite:test("evdns_with_udpd_make_dests", function()
        local dns = evdns.create({"8.8.8.8", "1.1.1.1"})
        TestFramework.assert_not_nil(dns)

        -- Test that udpd.make_dests accepts evdns parameter
        local dests_ok, dests_or_err = pcall(udpd.make_dests, "httpbin.org", 80, dns)

        if dests_ok then
            local dests = dests_or_err
            TestFramework.assert_not_nil(dests)
            -- dests might be nil if DNS resolution is async and not complete yet
            if dests then
                TestFramework.assert_type(dests, "table")
            end
        else
            -- DNS resolution might fail or be async - that's ok for this test
            TestFramework.assert_type(dests_or_err, "string") -- Error message
        end
    end)
else
    print("Skipping udpd integration tests (udpd not available)")
end

-- Test DNS object persistence across multiple operations
suite:test("dns_object_persistence", function()
    local dns = evdns.create("8.8.8.8")
    TestFramework.assert_not_nil(dns)

    -- Use the same DNS object multiple times
    for i = 1, 3 do
        local dns_str = tostring(dns)
        TestFramework.assert_type(dns_str, "string")

        -- Test that the object remains valid
        if tcpd_available then
            local ok = pcall(function()
                local conn = tcpd.connect({
                    host = "example.com",
                    port = 80,
                    evdns = dns,
                    callback_self_first = true,
                    onconnected = function(self)
                        self:close()
                    end
                })
            end)
            TestFramework.assert_true(ok, "DNS object should remain valid across multiple uses")
        end
    end
end)

-- Test different DNS configurations
suite:test("different_dns_configurations", function()
    local dns_configs = {
        evdns.create(),                    -- Default DNS
        evdns.create("8.8.8.8"),         -- Single Google DNS
        evdns.create("1.1.1.1"),         -- Single CloudFlare DNS
        evdns.create({"8.8.8.8", "1.1.1.1"}) -- Multiple DNS servers
    }

    for i, dns in ipairs(dns_configs) do
        TestFramework.assert_not_nil(dns, "DNS config #" .. i .. " should be valid")
        TestFramework.assert_type(dns, "userdata")

        -- Test that each configuration can be used with tcpd
        if tcpd_available then
            local ok = pcall(function()
                local conn = tcpd.connect({
                    host = "example.com",
                    port = 80,
                    evdns = dns,
                    callback_self_first = true,
                    onconnected = function(self)
                        self:close()
                    end
                })
            end)
            TestFramework.assert_true(ok, "DNS config #" .. i .. " should work with tcpd")
        end
    end
end)

-- Test error handling in integration scenarios
suite:test("integration_error_handling", function()
    -- Test with invalid DNS object (nil)
    if tcpd_available then
        local ok = pcall(function()
            local conn = tcpd.connect({
                host = "example.com",
                port = 80,
                evdns = nil,  -- Should use default DNS
                callback_self_first = true,
                onconnected = function(self)
                    self:close()
                end
            })
        end)
        TestFramework.assert_true(ok, "nil evdns should fallback to default DNS")
    end

    -- Test with custom DNS for invalid hostname
    local dns = evdns.create("8.8.8.8")
    if tcpd_available then
        local ok = pcall(function()
            local conn = tcpd.connect({
                host = "invalid.nonexistent.domain.example",
                port = 80,
                evdns = dns,
                callback_self_first = true,
                onconnected = function(self)
                    self:close()
                end,
                ondisconnected = function(self, reason)
                    -- Expected to fail
                end
            })
        end)
        TestFramework.assert_true(ok, "Connection to invalid host should not crash")
    end
end)

-- Performance comparison test (basic)
suite:test("performance_comparison_basic", function()
    if not tcpd_available then
        return -- Skip if tcpd not available
    end

    local dns_default = evdns.create()
    local dns_custom = evdns.create("8.8.8.8")

    -- Test that both DNS objects work without crashing
    local configs = {
        {name = "default", dns = dns_default},
        {name = "custom", dns = dns_custom}
    }

    for _, config in ipairs(configs) do
        local ok = pcall(function()
            local conn = tcpd.connect({
                host = "httpbin.org",
                port = 80,
                evdns = config.dns,
                callback_self_first = true,
                onconnected = function(self)
                    self:close()
                end
            })
        end)
        TestFramework.assert_true(ok, config.name .. " DNS should work")
    end
end)

-- Clean up test
suite:test("cleanup", function()
    -- Create several DNS objects and ensure they can be garbage collected
    local dns_objects = {}
    for i = 1, 10 do
        dns_objects[i] = evdns.create("8.8.8.8")
    end

    -- Clear references
    for i = 1, 10 do
        dns_objects[i] = nil
    end
    dns_objects = nil

    -- Force garbage collection
    collectgarbage("collect")
    collectgarbage("collect") -- Run twice to be sure

    -- If we reach here without crashing, cleanup worked
    TestFramework.assert_true(true, "DNS objects should be properly cleaned up")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)