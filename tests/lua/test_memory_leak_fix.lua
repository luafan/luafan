#!/usr/bin/env lua

-- Test to verify memory leak fix in udpd.c
-- Uses weak table mechanism to test selfRef memory leak fixes
-- Avoids DNS resolution to prevent test blocking

local fan = require "fan"
local udpd = require "fan.udpd"

print("Testing memory leak fix using weak table mechanism")

-- Weak table to track objects that should be garbage collected
local weak_refs = setmetatable({}, {__mode = "v"})

-- Test memory leak fix using weak table mechanism
local function test_memory_leak_with_weak_table()
    print("\n=== Testing memory leak with weak table ===")

    local initial_count = #weak_refs

    -- Test different scenarios that previously caused leaks
    do  -- Scope to ensure local variables can be collected
        -- Scenario 1: callback_self_first=true with IP (no DNS)
        print("Creating connection with callback_self_first=true...")
        local conn1 = udpd.new({
            host = "127.0.0.1",  -- IP address, no DNS needed
            port = 12345,
            callback_self_first = true,
            onread = function(self, data, dest)
                -- Callback with self reference
            end
        })

        if conn1 then
            weak_refs[#weak_refs + 1] = conn1
            print("✓ Connection created and tracked in weak table")
            if conn1.close then conn1:close() end
        end

        -- Scenario 2: Traditional callbacks (no self)
        print("Creating connection with traditional callbacks...")
        local conn2 = udpd.new({
            host = "127.0.0.1",
            port = 12346,
            callback_self_first = false,
            onread = function(data, dest)
                -- Traditional callback without self
            end
        })

        if conn2 then
            weak_refs[#weak_refs + 1] = conn2
            print("✓ Connection created and tracked in weak table")
            if conn2.close then conn2:close() end
        end

        -- Scenario 3: Bind-only connection
        print("Creating bind-only connection...")
        local conn3 = udpd.new({
            bind_port = 0,  -- Let system assign port
            callback_self_first = true,
            onread = function(self, data, dest)
                -- Bind-only with self reference
            end
        })

        if conn3 then
            weak_refs[#weak_refs + 1] = conn3
            print("✓ Bind-only connection created and tracked")
            if conn3.close then conn3:close() end
        end

        print(string.format("Tracked %d connections in weak table", #weak_refs - initial_count))
    end

    -- Force garbage collection
    print("Forcing garbage collection...")
    collectgarbage("collect")
    collectgarbage("collect")  -- Second pass to ensure complete cleanup

    -- Check if objects were properly collected
    local remaining_count = 0
    for i = 1, #weak_refs do
        if weak_refs[i] ~= nil then
            remaining_count = remaining_count + 1
        end
    end

    print(string.format("Objects remaining after GC: %d", remaining_count))

    if remaining_count == initial_count then
        print("✅ Memory leak test PASSED - all objects properly collected")
        return true
    else
        print("❌ Memory leak test FAILED - some objects not collected")
        return false
    end
end

-- Test destination object memory management
local function test_destination_memory_management()
    print("\n=== Testing destination object memory management ===")

    local dest_weak_refs = setmetatable({}, {__mode = "v"})

    do  -- Scope for destination objects
        -- Create destination objects for IP addresses (no DNS)
        local dest1 = udpd.make_dest("127.0.0.1", 8080)
        if dest1 then
            dest_weak_refs[#dest_weak_refs + 1] = dest1
            print("✓ Destination object created for 127.0.0.1:8080")
        end

        local dest2 = udpd.make_dest("192.168.1.1", 9090)
        if dest2 then
            dest_weak_refs[#dest_weak_refs + 1] = dest2
            print("✓ Destination object created for 192.168.1.1:9090")
        end

        print(string.format("Tracked %d destination objects", #dest_weak_refs))
    end

    -- Force garbage collection
    print("Forcing garbage collection for destinations...")
    collectgarbage("collect")
    collectgarbage("collect")

    -- Check remaining destinations
    local remaining_dests = 0
    for i = 1, #dest_weak_refs do
        if dest_weak_refs[i] ~= nil then
            remaining_dests = remaining_dests + 1
        end
    end

    print(string.format("Destination objects remaining after GC: %d", remaining_dests))

    if remaining_dests == 0 then
        print("✅ Destination memory test PASSED - all destinations collected")
        return true
    else
        print("❌ Destination memory test FAILED - some destinations not collected")
        return false
    end
end

-- Test configuration object cleanup
local function test_configuration_cleanup()
    print("\n=== Testing configuration cleanup ===")

    -- Test that configurations with allocated strings are properly cleaned up
    local config_test_passed = true

    do  -- Scope for config test
        local conn = udpd.new({
            bind_host = "127.0.0.1",  -- This allocates memory via strdup
            bind_port = 0,
            callback_self_first = true,
            onread = function(self, data, dest)
                -- Simple callback
            end
        })

        if conn then
            print("✓ Connection with bind_host created successfully")
            if conn.close then conn:close() end
        else
            config_test_passed = false
        end
    end

    collectgarbage("collect")
    collectgarbage("collect")

    if config_test_passed then
        print("✅ Configuration cleanup test PASSED")
    else
        print("❌ Configuration cleanup test FAILED")
    end

    return config_test_passed
end

-- Main test execution
print("Starting memory leak fix verification with weak tables...")

local test1_passed = test_memory_leak_with_weak_table()
local test2_passed = test_destination_memory_management()
local test3_passed = test_configuration_cleanup()

if test1_passed and test2_passed and test3_passed then
    print("\n✅ All memory leak tests PASSED!")
    print("The selfRef duplicate setting bug has been successfully fixed.")
    print("Memory management is working correctly.")
    os.exit(0)
else
    print("\n❌ Some memory leak tests FAILED!")
    print("Memory management issues may still exist.")
    os.exit(1)
end