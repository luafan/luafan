# fan.upnp

UPnP (Universal Plug and Play) client for automatic port mapping through Internet Gateway Devices (IGDs). This module allows applications to automatically configure port forwarding on compatible routers and NAT devices.

## Overview

The UPnP module discovers Internet Gateway Devices on the local network and can automatically create port mappings to make local services accessible from the internet. This is particularly useful for P2P applications, game servers, and other services that need external accessibility.

## Functions

### new(timeout_sec)

Discover UPnP devices on the network and create a UPnP client instance.

**Parameters:**
- `timeout_sec` (number): Discovery timeout in seconds

**Returns:**
- `upnp`: UPnP client instance with discovered devices

**Example:**
```lua
local upnp = require "fan.upnp"

-- Discover devices with 3 second timeout
local client = upnp.new(3)
print("Found", #client.devices, "UPnP devices")
```

## UPnP Client Methods

### client:AddPortMapping(ip, port, extport, protocol, description)

Create a port mapping on the Internet Gateway Device to forward external traffic to a local service.

**Parameters:**
- `ip` (string): Internal IP address to forward to
- `port` (number): Internal port number
- `extport` (number): External port number to map
- `protocol` (string): Protocol type ("tcp" or "udp")
- `description` (string, optional): Description for the mapping (defaults to "fan.upnp")

**Returns:**
- `success` (boolean): Whether the mapping was created successfully
- `response` (string): Response body from the device

**Example:**
```lua
local upnp = require "fan.upnp"

local client = upnp.new(5)
if #client.devices > 0 then
    local success, response = client:AddPortMapping(
        "192.168.1.100",  -- Internal IP
        8080,             -- Internal port
        8080,             -- External port
        "tcp",            -- Protocol
        "My Web Server"   -- Description
    )

    if success then
        print("Port mapping created successfully")
    else
        print("Failed to create port mapping:", response)
    end
else
    print("No UPnP devices found")
end
```

## Discovery Process

The discovery process works as follows:

1. **Multicast Search**: Sends SSDP M-SEARCH request to `239.255.255.250:1900`
2. **Device Response**: Collects responses from Internet Gateway Devices
3. **Service Discovery**: Fetches device description files to find WANIPConnection services
4. **Control URL Extraction**: Extracts control URLs for port mapping operations

## Usage Examples

### Basic Port Mapping

```lua
local upnp = require "fan.upnp"
local fan = require "fan"

-- Create a simple TCP server
local server = fan.tcpd.bind("0.0.0.0", 8080)

-- Discover UPnP devices and create port mapping
local client = upnp.new(3)
if #client.devices > 0 then
    local success = client:AddPortMapping("192.168.1.100", 8080, 8080, "tcp", "Test Server")
    if success then
        print("Server accessible from internet on port 8080")
    end
end

-- Handle connections
while true do
    local conn = server:accept()
    fan.loop(function()
        -- Handle connection...
        conn:close()
    end)
end
```

### Game Server with UPnP

```lua
local upnp = require "fan.upnp"
local fan = require "fan"

function setup_game_server(port)
    -- Get local IP (simplified - you'd want to detect this properly)
    local local_ip = "192.168.1.100"

    -- Discover UPnP devices
    local client = upnp.new(5)

    if #client.devices == 0 then
        print("No UPnP devices found - manual port forwarding required")
        return false
    end

    -- Try to map both TCP and UDP for game server
    local tcp_success = client:AddPortMapping(local_ip, port, port, "tcp", "Game Server TCP")
    local udp_success = client:AddPortMapping(local_ip, port, port, "udp", "Game Server UDP")

    if tcp_success and udp_success then
        print("Game server ports mapped successfully")
        print("Players can connect to your external IP on port", port)
        return true
    else
        print("Failed to map some ports - check router configuration")
        return false
    end
end

-- Setup and run game server
if setup_game_server(7777) then
    -- Start your game server here
    print("Game server ready for external connections")
end
```

### P2P Application

```lua
local upnp = require "fan.upnp"
local fan = require "fan"

function create_p2p_endpoint()
    -- Discover available port
    local port = 6789
    local local_ip = get_local_ip()  -- Your implementation

    -- Setup UPnP mapping
    local client = upnp.new(3)
    local external_accessible = false

    if #client.devices > 0 then
        local success = client:AddPortMapping(local_ip, port, port, "tcp", "P2P Node")
        if success then
            external_accessible = true
            print("P2P node accessible externally on port", port)
        end
    end

    -- Create server socket
    local server = fan.tcpd.bind("0.0.0.0", port)

    return {
        server = server,
        port = port,
        external_accessible = external_accessible
    }
end
```

## Supported Devices

The module specifically looks for devices implementing:
- `urn:schemas-upnp-org:device:InternetGatewayDevice:1`
- `urn:schemas-upnp-org:service:WANIPConnection:1`

This covers most home routers and internet gateways that support UPnP.

## Error Handling

Common failure scenarios:

- **No devices found**: Router doesn't support UPnP or UPnP is disabled
- **Mapping failed**: Port already mapped, insufficient permissions, or router restrictions
- **Timeout**: Network issues or slow device response

## Security Considerations

1. **UPnP Security Risk**: UPnP can be a security risk as it allows automatic port opening
2. **Port Conflicts**: Be prepared to handle port conflicts with existing mappings
3. **Cleanup**: Consider removing mappings when your application shuts down
4. **Validation**: Always validate that mappings were created successfully

## Implementation Notes

- Uses SOAP over HTTP for device communication
- Discovery uses UDP multicast to `239.255.255.250:1900`
- Timeouts are handled using coroutines and `fan.sleep()`
- Device descriptions are cached during discovery to avoid duplicate HTTP requests
- Only supports permanent mappings (lease duration set to 0)

## Limitations

- Only supports WANIPConnection service (most common)
- No support for removing port mappings (would need additional SOAP operations)
- No IPv6 support
- Assumes single Internet Gateway Device (uses first compatible device found)

## Best Practices

1. **Check for UPnP availability** before relying on automatic port mapping
2. **Provide fallback options** for manual port forwarding
3. **Use reasonable timeouts** to avoid hanging on unresponsive networks
4. **Handle mapping failures gracefully** with appropriate user feedback
5. **Test with different router brands** as UPnP implementations vary