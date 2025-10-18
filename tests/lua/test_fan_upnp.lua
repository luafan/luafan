#!/usr/bin/env lua

-- Test for fan.upnp module (UPnP port mapping for NAT traversal)
-- This module provides device discovery and port mapping functionality

local TestFramework = require('test_framework')
local fan = require "fan"

-- Mock config module if needed
package.preload['config'] = function()
    return {
        debug = false
    }
end

-- Try to load fan.upnp module
local upnp_available = false
local upnp

local ok, result = pcall(require, 'fan.upnp')
if ok then
    upnp = result
    upnp_available = true
    print("fan.upnp module loaded successfully")
else
    print("Error: fan.upnp module not available: " .. tostring(result))
    os.exit(1)
end

-- Create test suite
local suite = TestFramework.create_suite("fan.upnp Tests")

print("Testing fan.upnp module")

-- Test module structure
suite:test("module_structure", function()
    TestFramework.assert_not_nil(upnp)
    TestFramework.assert_type(upnp, "table")

    -- Check for essential functions
    TestFramework.assert_type(upnp.new, "function")
end)

-- Test basic UPnP object creation (timeout test - should not block indefinitely)
suite:test("upnp_object_creation", function()
    local start_time = fan.gettime()

    -- Create UPnP object with short timeout (use integer seconds to avoid format issues)
    local upnp_obj = upnp.new(1)  -- 1 second timeout

    local end_time = fan.gettime()
    local elapsed = end_time - start_time

    TestFramework.assert_not_nil(upnp_obj)
    TestFramework.assert_type(upnp_obj, "table")

    -- Should complete within reasonable time (allowing some overhead)
    TestFramework.assert_true(elapsed < 3.0)  -- Should not take more than 3 seconds
    TestFramework.assert_true(elapsed >= 0.5) -- Should take at least some time for network operation
end)

-- Test UPnP object structure
suite:test("upnp_object_structure", function()
    local upnp_obj = upnp.new(1)

    TestFramework.assert_not_nil(upnp_obj)
    TestFramework.assert_type(upnp_obj, "table")

    -- Check devices list exists
    TestFramework.assert_not_nil(upnp_obj.devices)
    TestFramework.assert_type(upnp_obj.devices, "table")

    -- Check AddPortMapping method exists
    TestFramework.assert_type(upnp_obj.AddPortMapping, "function")
end)

-- Test devices discovery results
suite:test("device_discovery_structure", function()
    local upnp_obj = upnp.new(1)

    -- Devices should be a table (empty or with entries)
    TestFramework.assert_type(upnp_obj.devices, "table")

    -- Check device structure if any devices were found
    for _, device in ipairs(upnp_obj.devices) do
        TestFramework.assert_type(device, "table")
        TestFramework.assert_type(device.st, "string")
        TestFramework.assert_type(device.url, "string")

        -- Should have WANIPConnection service type
        TestFramework.assert_equal("urn:schemas-upnp-org:service:WANIPConnection:1", device.st)

        -- URL should be properly formatted
        TestFramework.assert_true(#device.url > 0)

        -- Server field may or may not be present
        if device.server then
            TestFramework.assert_type(device.server, "string")
        end
    end
end)

-- Test AddPortMapping method interface (without actual device)
suite:test("add_port_mapping_interface", function()
    local upnp_obj = upnp.new(1)

    -- Test method exists
    TestFramework.assert_type(upnp_obj.AddPortMapping, "function")

    -- Test parameter validation (should handle missing parameters gracefully)
    local ok1, err1 = pcall(function()
        return upnp_obj:AddPortMapping()  -- No parameters
    end)
    -- Should error due to assertion for required parameters
    TestFramework.assert_false(ok1)

    local ok2, err2 = pcall(function()
        return upnp_obj:AddPortMapping("192.168.1.100")  -- Insufficient parameters
    end)
    TestFramework.assert_false(ok2)

    local ok3, err3 = pcall(function()
        return upnp_obj:AddPortMapping("192.168.1.100", 8080)  -- Still insufficient
    end)
    TestFramework.assert_false(ok3)
end)

-- Test AddPortMapping with complete parameters (no actual device expected)
suite:test("add_port_mapping_no_device", function()
    local upnp_obj = upnp.new(1)

    -- With no UPnP devices found, AddPortMapping should return nil (no successful mapping)
    local success, body = upnp_obj:AddPortMapping("192.168.1.100", 8080, 8080, "TCP", "Test Mapping")

    -- Should return without error, but likely no success since no devices
    TestFramework.assert_nil(success)  -- Function returns early if no devices
end)

-- Test different timeout values
suite:test("different_timeout_values", function()
    -- Test minimal timeout
    local start1 = fan.gettime()
    local upnp_obj1 = upnp.new(1)  -- 1 second timeout
    local end1 = fan.gettime()

    TestFramework.assert_not_nil(upnp_obj1)
    TestFramework.assert_true((end1 - start1) < 3.0)  -- Should complete quickly

    -- Test slightly longer timeout
    local start2 = fan.gettime()
    local upnp_obj2 = upnp.new(2)   -- 2 second timeout
    local end2 = fan.gettime()

    TestFramework.assert_not_nil(upnp_obj2)
    TestFramework.assert_true((end2 - start2) < 5.0)  -- Should complete within reasonable time
end)

-- Test protocol parameter handling
suite:test("protocol_parameter_handling", function()
    local upnp_obj = upnp.new(1)

    -- Test different protocol values (though no actual mapping will occur without devices)
    local protocols = {"TCP", "UDP", "tcp", "udp"}

    for _, protocol in ipairs(protocols) do
        local ok, err = pcall(function()
            return upnp_obj:AddPortMapping("192.168.1.100", 8080, 8080, protocol, "Test")
        end)
        TestFramework.assert_true(ok)  -- Should not error on valid parameters
    end
end)

-- Test description parameter (optional)
suite:test("description_parameter", function()
    local upnp_obj = upnp.new(1)

    -- Test with explicit description
    local ok1, err1 = pcall(function()
        return upnp_obj:AddPortMapping("192.168.1.100", 8080, 8080, "TCP", "Custom Description")
    end)
    TestFramework.assert_true(ok1)

    -- Test without description (should use default)
    local ok2, err2 = pcall(function()
        return upnp_obj:AddPortMapping("192.168.1.100", 8080, 8080, "TCP")
    end)
    TestFramework.assert_true(ok2)
end)

-- Test IP address validation (through interface)
suite:test("ip_address_interface", function()
    local upnp_obj = upnp.new(1)

    local test_ips = {
        "192.168.1.100",
        "10.0.0.1",
        "172.16.0.1",
        "127.0.0.1"
    }

    for _, ip in ipairs(test_ips) do
        local ok, err = pcall(function()
            return upnp_obj:AddPortMapping(ip, 8080, 8080, "TCP", "Test")
        end)
        TestFramework.assert_true(ok)  -- Should accept valid IP format
    end
end)

-- Test port number handling
suite:test("port_number_interface", function()
    local upnp_obj = upnp.new(1)

    local test_ports = {
        80, 443, 8080, 3000, 65535
    }

    for _, port in ipairs(test_ports) do
        local ok, err = pcall(function()
            return upnp_obj:AddPortMapping("192.168.1.100", port, port, "TCP", "Test")
        end)
        TestFramework.assert_true(ok)  -- Should accept valid port numbers
    end
end)

-- Test concurrent discovery (multiple objects)
suite:test("concurrent_discovery", function()
    local start_time = fan.gettime()

    -- Create multiple UPnP objects concurrently (in the async context)
    local upnp_obj1 = upnp.new(1)
    local upnp_obj2 = upnp.new(1)

    local end_time = fan.gettime()

    TestFramework.assert_not_nil(upnp_obj1)
    TestFramework.assert_not_nil(upnp_obj2)

    -- Both should have devices arrays
    TestFramework.assert_type(upnp_obj1.devices, "table")
    TestFramework.assert_type(upnp_obj2.devices, "table")

    -- Should complete in reasonable time (not blocking indefinitely)
    TestFramework.assert_true((end_time - start_time) < 5.0)
end)

-- Test error resilience (network unavailable scenarios)
suite:test("error_resilience", function()
    -- This test ensures the module handles network errors gracefully
    local upnp_obj = upnp.new(1)  -- 1 second timeout

    TestFramework.assert_not_nil(upnp_obj)
    TestFramework.assert_type(upnp_obj.devices, "table")

    -- Even with network issues, should return valid object structure
    TestFramework.assert_type(upnp_obj.AddPortMapping, "function")
end)

-- Test service type constants and XML parsing behavior
suite:test("service_type_handling", function()
    local upnp_obj = upnp.new(1)

    -- Check that devices, if any, have correct service type
    for _, device in ipairs(upnp_obj.devices) do
        -- Should match WANIPConnection service type
        TestFramework.assert_equal("urn:schemas-upnp-org:service:WANIPConnection:1", device.st)

        -- URL should be properly constructed
        TestFramework.assert_type(device.url, "string")
        TestFramework.assert_true(string.find(device.url, "http") ~= nil or
                                string.find(device.url, ":") ~= nil)  -- Should be URL-like format
    end
end)

-- Run the test suite
local failures = TestFramework.run_suite(suite)

-- Exit with appropriate code
os.exit(failures > 0 and 1 or 0)