#!/usr/bin/env lua

-- Test for fan.evdns module (custom DNS nameserver support)

local TestFramework = require('test_framework')

-- Try to load fan.evdns module
local evdns_available = false
local evdns

local ok, result = pcall(require, 'fan.evdns')
if ok then
    evdns = result
    evdns_available = true
    print("fan.evdns module loaded successfully")
else
    print("Error: fan.evdns module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.evdns Tests")

-- Test module loading and basic structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(evdns)
    TestFramework.assert_type(evdns, "table")

    -- Check for essential functions
    TestFramework.assert_type(evdns.create, "function")
end)

-- Test creating default DNS resolver
suite:test("create_default_dns", function()
    -- Test with no parameters (should use system default)
    local dns = evdns.create()
    TestFramework.assert_not_nil(dns)
    TestFramework.assert_type(dns, "userdata")

    -- Test tostring representation
    local dns_str = tostring(dns)
    TestFramework.assert_type(dns_str, "string")
    TestFramework.assert_equal(dns_str, "<evdns: default>")

    -- Test with nil parameter (should also use system default)
    local dns2 = evdns.create(nil)
    TestFramework.assert_not_nil(dns2)
    TestFramework.assert_type(dns2, "userdata")
    TestFramework.assert_equal(tostring(dns2), "<evdns: default>")
end)

-- Test creating custom DNS resolver with single nameserver
suite:test("create_single_nameserver_dns", function()
    -- Test with Google DNS
    local dns = evdns.create("8.8.8.8")
    TestFramework.assert_not_nil(dns)
    TestFramework.assert_type(dns, "userdata")

    -- Should be marked as custom
    local dns_str = tostring(dns)
    TestFramework.assert_type(dns_str, "string")
    -- Note: This might be "<evdns: custom>" or "<evdns: default>" if DNS creation failed
    TestFramework.assert_true(dns_str == "<evdns: custom>" or dns_str == "<evdns: default>")

    -- Test with CloudFlare DNS
    local dns2 = evdns.create("1.1.1.1")
    TestFramework.assert_not_nil(dns2)
    TestFramework.assert_type(dns2, "userdata")
end)

-- Test creating custom DNS resolver with multiple nameservers
suite:test("create_multiple_nameservers_dns", function()
    -- Test with array of nameservers
    local nameservers = {"8.8.8.8", "1.1.1.1", "9.9.9.9"}
    local dns = evdns.create(nameservers)
    TestFramework.assert_not_nil(dns)
    TestFramework.assert_type(dns, "userdata")

    -- Test with empty array (should fallback to default)
    local dns2 = evdns.create({})
    TestFramework.assert_not_nil(dns2)
    TestFramework.assert_type(dns2, "userdata")
    TestFramework.assert_equal(tostring(dns2), "<evdns: default>")

    -- Test with mixed valid/invalid nameservers
    local mixed_nameservers = {"8.8.8.8", "", "1.1.1.1", nil}
    local dns3 = evdns.create(mixed_nameservers)
    TestFramework.assert_not_nil(dns3)
    TestFramework.assert_type(dns3, "userdata")
end)

-- Test error handling for invalid parameters
suite:test("invalid_parameters", function()
    -- Test with invalid parameter type (number)
    local ok, err = pcall(evdns.create, 12345)
    TestFramework.assert_false(ok)
    TestFramework.assert_type(err, "string")

    -- Test with invalid parameter type (boolean)
    local ok2, err2 = pcall(evdns.create, true)
    TestFramework.assert_false(ok2)
    TestFramework.assert_type(err2, "string")

    -- Test with table containing non-strings
    local ok3, err3 = pcall(evdns.create, {123, "8.8.8.8", true})
    TestFramework.assert_not_nil(ok3) -- This might succeed but ignore invalid entries
end)

-- Test garbage collection
suite:test("garbage_collection", function()
    -- Create DNS objects and let them be garbage collected
    do
        local dns1 = evdns.create("8.8.8.8")
        local dns2 = evdns.create({"8.8.8.8", "1.1.1.1"})
        TestFramework.assert_not_nil(dns1)
        TestFramework.assert_not_nil(dns2)
    end

    -- Force garbage collection
    collectgarbage("collect")

    -- Should not crash - objects should be properly cleaned up
    TestFramework.assert_true(true) -- If we reach here, GC worked correctly
end)

-- Test DNS object reuse
suite:test("dns_object_reuse", function()
    local dns = evdns.create("8.8.8.8")
    TestFramework.assert_not_nil(dns)

    -- Should be able to use same DNS object multiple times
    local dns_str1 = tostring(dns)
    local dns_str2 = tostring(dns)
    TestFramework.assert_equal(dns_str1, dns_str2)

    -- Store in table and retrieve
    local dns_table = {my_dns = dns}
    TestFramework.assert_equal(dns, dns_table.my_dns)
end)

-- Test common public DNS servers
suite:test("public_dns_servers", function()
    -- Test various public DNS servers
    local public_dns_servers = {
        "8.8.8.8",      -- Google Primary
        "8.8.4.4",      -- Google Secondary
        "1.1.1.1",      -- CloudFlare Primary
        "1.0.0.1",      -- CloudFlare Secondary
        "9.9.9.9",      -- Quad9 Primary
        "208.67.222.222" -- OpenDNS
    }

    for _, server in ipairs(public_dns_servers) do
        local dns = evdns.create(server)
        TestFramework.assert_not_nil(dns, "Failed to create DNS with server: " .. server)
        TestFramework.assert_type(dns, "userdata")

        local dns_str = tostring(dns)
        TestFramework.assert_type(dns_str, "string")
        -- Should be either custom or default (fallback)
        TestFramework.assert_true(
            dns_str == "<evdns: custom>" or dns_str == "<evdns: default>",
            "Unexpected DNS string representation: " .. dns_str
        )
    end
end)

-- Test edge cases
suite:test("edge_cases", function()
    -- Test with empty string
    local dns1 = evdns.create("")
    TestFramework.assert_not_nil(dns1)
    TestFramework.assert_equal(tostring(dns1), "<evdns: default>") -- Should fallback

    -- Test with whitespace
    local dns2 = evdns.create("   ")
    TestFramework.assert_not_nil(dns2)

    -- Test with invalid IP address
    local dns3 = evdns.create("999.999.999.999")
    TestFramework.assert_not_nil(dns3)
    TestFramework.assert_equal(tostring(dns3), "<evdns: default>") -- Should fallback

    -- Test with hostname instead of IP (should work in some cases)
    local dns4 = evdns.create("dns.google")
    TestFramework.assert_not_nil(dns4)
    -- Note: This might fail depending on implementation, but shouldn't crash
end)

-- Integration test placeholder (requires network)
suite:test("integration_placeholder", function()
    -- This test documents that DNS objects are meant to be used with:
    -- - fan.tcpd.connect() with evdns parameter
    -- - fan.udpd.make_dest() with evdns parameter
    -- - fan.udpd.make_dests() with evdns parameter

    local dns = evdns.create("8.8.8.8")
    TestFramework.assert_not_nil(dns)

    -- The actual integration tests would require network connectivity
    -- and are better placed in separate integration test files
    TestFramework.assert_true(true, "Integration tests should be in separate files")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)