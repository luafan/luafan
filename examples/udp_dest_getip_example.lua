#!/usr/bin/env lua

-- Example demonstrating the new getIP() method for UDP destination objects
-- This example shows how to use the getIP() method to extract IP addresses
-- from UDP destination objects in various scenarios.

local fan = require('fan')
local udpd = require('fan.udpd')

print("UDP Destination getIP() Method Example")
print("=====================================")

fan.loop(function()
    -- Example 1: Basic getIP() usage with make_dest
    print("\n1. Basic getIP() usage:")
    local dest1 = udpd.make_dest("192.168.1.100", 8080)
    print(string.format("   Host: %s", dest1:getHost()))
    print(string.format("   Port: %d", dest1:getPort()))
    print(string.format("   IP:   %s", dest1:getIP()))  -- New method!

    -- Example 2: Different IP addresses
    print("\n2. Testing different IP addresses:")
    local test_ips = {
        {"127.0.0.1", 80},
        {"10.0.0.1", 443},
        {"8.8.8.8", 53},
        {"0.0.0.0", 12345}
    }

    for i, ip_port in ipairs(test_ips) do
        local dest = udpd.make_dest(ip_port[1], ip_port[2])
        print(string.format("   %s:%d -> IP: %s",
              ip_port[1], ip_port[2], dest:getIP()))
    end

    -- Example 3: UDP Server demonstrating getIP() in callback
    print("\n3. UDP Server with getIP() in callback:")

    local server = udpd.new({
        bind_port = 0,  -- Let system assign port
        callback_self_first = true,  -- Use modern callback pattern
        onread = function(self, data, dest)
            local client_ip = dest:getIP()
            local client_host = dest:getHost()
            local client_port = dest:getPort()

            print(string.format("   Received '%s' from IP: %s (host: %s, port: %d)",
                  data, client_ip, client_host, client_port))

            -- Verify getIP() and getHost() return same value for IP addresses
            if client_ip == client_host then
                print("   ✓ getIP() and getHost() match for IP address")
            else
                print("   ✗ getIP() and getHost() differ (unexpected)")
            end

            -- Echo back the client's IP using self parameter
            self:send(string.format("Your IP is: %s", client_ip), dest)
        end
    })

    local server_port = server:getPort()
    print(string.format("   Server listening on port: %d", server_port))

    -- Create client to test the server
    fan.sleep(0.1)  -- Small delay

    local client = udpd.new({
        callback_self_first = true  -- Use modern callback pattern
    })
    -- Send test data to the server
    local server_dest = udpd.make_dest("127.0.0.1", server_port)
    client:send("Hello from client!", server_dest)

    -- Example 4: Multiple destinations comparison
    print("\n4. Comparing multiple destinations:")
    local dest_a = udpd.make_dest("192.168.1.1", 80)
    local dest_b = udpd.make_dest("192.168.1.2", 80)
    local dest_c = udpd.make_dest("192.168.1.1", 8080)

    print(string.format("   dest_a IP: %s", dest_a:getIP()))
    print(string.format("   dest_b IP: %s", dest_b:getIP()))
    print(string.format("   dest_c IP: %s", dest_c:getIP()))

    -- Example 5: Method consistency test
    print("\n5. Method consistency test:")
    local dest = udpd.make_dest("203.0.113.1", 9999)

    -- Call getIP() multiple times
    local ip1 = dest:getIP()
    local ip2 = dest:getIP()
    local ip3 = dest:getIP()

    if ip1 == ip2 and ip2 == ip3 then
        print(string.format("   ✓ getIP() returns consistent results: %s", ip1))
    else
        print("   ✗ getIP() returns inconsistent results")
    end

    -- Clean up and exit
    fan.sleep(0.5)  -- Allow any pending operations to complete
    print("\nExample completed successfully!")
    fan.loopbreak()
end)