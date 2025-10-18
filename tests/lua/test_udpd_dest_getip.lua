#!/usr/bin/env lua

-- Test for UDP destination getIP() method
-- Tests the new getIP() method added to LUA_UDPD_DEST_TYPE

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
local suite = TestFramework.create_suite("UDP Destination getIP() Tests")

print("Testing UDP destination getIP() method")

-- Skip all tests if udpd not available
if not udpd_available then
    print("Skipping UDP getIP tests - udpd module not available")
    local failures = TestFramework.run_suite(suite)
    os.exit(failures > 0 and 1 or 0)
end

-- Test UDP destination creation and getIP method
suite:test("udp_dest_getip_basic", function()
    -- Test destination creation
    local dest = udpd.make_dest("127.0.0.1", 12345)
    TestFramework.assert_not_nil(dest)
    TestFramework.assert_type(dest, "userdata")

    -- Test getIP method exists
    TestFramework.assert_type(dest.getIP, "function")

    -- Test getIP returns correct IP
    local ip = dest:getIP()
    TestFramework.assert_not_nil(ip)
    TestFramework.assert_type(ip, "string")
    TestFramework.assert_equal("127.0.0.1", ip)

    print("Basic getIP() functionality verified")
end)

-- Test getIP with different IP addresses
suite:test("udp_dest_getip_ipv4_addresses", function()
    local test_ips = {"127.0.0.1", "192.168.1.1", "8.8.8.8", "0.0.0.0"}

    for _, test_ip in ipairs(test_ips) do
        local dest = udpd.make_dest(test_ip, 80)
        TestFramework.assert_not_nil(dest)

        local ip = dest:getIP()
        TestFramework.assert_not_nil(ip)
        TestFramework.assert_type(ip, "string")
        TestFramework.assert_equal(test_ip, ip)
    end

    print("IPv4 address getIP() tests passed")
end)

-- Test comparison between getIP and getHost for IP addresses
suite:test("udp_dest_getip_vs_gethost", function()
    local dest = udpd.make_dest("192.168.1.100", 8080)
    TestFramework.assert_not_nil(dest)

    local ip = dest:getIP()
    local host = dest:getHost()

    TestFramework.assert_not_nil(ip)
    TestFramework.assert_not_nil(host)
    TestFramework.assert_type(ip, "string")
    TestFramework.assert_type(host, "string")

    -- For IP addresses, getIP() and getHost() should return the same value
    TestFramework.assert_equal(host, ip)

    print("getIP() vs getHost() comparison passed")
end)

-- Test getIP method integration with other destination methods
suite:test("udp_dest_getip_method_integration", function()
    local dest = udpd.make_dest("203.0.113.1", 12345)
    TestFramework.assert_not_nil(dest)

    -- Test that getIP method is available
    TestFramework.assert_type(dest.getIP, "function")

    -- Test that getIP returns expected result
    local ip = dest:getIP()
    TestFramework.assert_not_nil(ip)
    TestFramework.assert_type(ip, "string")
    TestFramework.assert_equal("203.0.113.1", ip)

    -- Test that other methods still work
    TestFramework.assert_type(dest.getHost, "function")
    TestFramework.assert_type(dest.getPort, "function")

    local host = dest:getHost()
    local port = dest:getPort()
    TestFramework.assert_equal("203.0.113.1", host)
    TestFramework.assert_equal(12345, port)

    -- Test string representation still works
    local str = tostring(dest)
    TestFramework.assert_not_nil(str)
    TestFramework.assert_type(str, "string")
    TestFramework.assert_true(string.find(str, "203.0.113.1"))

    print("getIP() method integration verified")
end)

-- Test getIP method consistency
suite:test("udp_dest_getip_consistency", function()
    local dest = udpd.make_dest("10.0.0.1", 443)
    TestFramework.assert_not_nil(dest)

    -- Call getIP multiple times
    local ip1 = dest:getIP()
    local ip2 = dest:getIP()
    local ip3 = dest:getIP()

    TestFramework.assert_not_nil(ip1)
    TestFramework.assert_not_nil(ip2)
    TestFramework.assert_not_nil(ip3)

    -- Should return consistent results
    TestFramework.assert_equal(ip1, ip2)
    TestFramework.assert_equal(ip2, ip3)
    TestFramework.assert_equal("10.0.0.1", ip1)

    print("getIP() consistency verified")
end)

-- Test getIP with port information
suite:test("udp_dest_getip_with_port", function()
    local dest = udpd.make_dest("172.16.0.1", 9999)
    TestFramework.assert_not_nil(dest)

    -- Test that getIP only returns IP, not port
    local ip = dest:getIP()
    local port = dest:getPort()

    TestFramework.assert_equal("172.16.0.1", ip)
    TestFramework.assert_equal(9999, port)

    -- Verify IP doesn't contain port information
    TestFramework.assert_false(string.find(ip, ":"))

    print("getIP() port separation verified")
end)

-- Test getIP error handling
suite:test("udp_dest_getip_error_handling", function()
    -- Test with valid destination
    local dest = udpd.make_dest("127.0.0.1", 80)
    TestFramework.assert_not_nil(dest)

    -- getIP should not throw errors
    local ok, result = pcall(function() return dest:getIP() end)
    TestFramework.assert_true(ok)
    TestFramework.assert_not_nil(result)
    TestFramework.assert_type(result, "string")

    print("getIP() error handling verified")
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)