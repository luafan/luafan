fan.evdns
==========

The `fan.evdns` module provides custom DNS nameserver support for LuaFan applications. It allows you to create DNS resolver instances with specific nameservers, which can be used by TCP and UDP connections for hostname resolution.

## Overview

By default, LuaFan uses the system's DNS configuration for hostname resolution. The `evdns` module allows you to override this behavior by creating custom DNS resolver instances with specific nameservers. This is useful for:

- Using custom DNS servers (e.g., CloudFlare 1.1.1.1, Google 8.8.8.8)
- DNS over HTTPS/TLS scenarios
- Testing with controlled DNS environments
- Working around DNS censorship or filtering

## API Reference

### `dns = evdns.create([nameservers])`

Creates a new DNS resolver instance.

**Parameters:**
- `nameservers` (optional): DNS nameserver configuration
  - `string`: Single nameserver IP address (e.g., "8.8.8.8")
  - `table`: Array of nameserver IP addresses (e.g., {"8.8.8.8", "1.1.1.1"})
  - `nil`: Uses system default DNS configuration

**Returns:**
- `dns`: EVDNS resolver object that can be passed to TCP/UDP operations

**Examples:**

```lua
local evdns = require('fan.evdns')

-- Use system default DNS
local dns_default = evdns.create()

-- Use single custom nameserver
local dns_google = evdns.create("8.8.8.8")

-- Use multiple custom nameservers
local dns_multiple = evdns.create({"8.8.8.8", "1.1.1.1", "208.67.222.222"})
```

### DNS Object Methods

The DNS object returned by `evdns.create()` doesn't expose direct methods to Lua code. Instead, it's designed to be passed as a parameter to other LuaFan modules that perform hostname resolution.

**String Representation:**
- `tostring(dns)` returns `"<evdns: default>"` for system DNS or `"<evdns: custom>"` for custom nameservers

## Usage with Other Modules

### TCP Connections (`fan.tcpd`)

Pass the DNS object as the `evdns` parameter to `tcpd.connect()`:

```lua
local tcpd = require('fan.tcpd')
local evdns = require('fan.evdns')

-- Create custom DNS resolver
local dns = evdns.create("8.8.8.8")

-- Use custom DNS for TCP connection
local conn = tcpd.connect({
    host = "example.com",
    port = 80,
    evdns = dns,  -- Use custom DNS resolver
    onconnected = function()
        print("Connected using custom DNS")
    end
})
```

### UDP Destinations (`fan.udpd`)

Pass the DNS object as the third parameter to `make_dest()` and `make_dests()`:

```lua
local udpd = require('fan.udpd')
local evdns = require('fan.evdns')

-- Create custom DNS resolver
local dns = evdns.create({"8.8.8.8", "1.1.1.1"})

-- Create single UDP destination with custom DNS
local dest = udpd.make_dest("example.com", 53, dns)

-- Create multiple UDP destinations with custom DNS
local dests = udpd.make_dests("example.com", 53, dns)
```

## Implementation Notes

### Memory Management

- DNS objects are automatically garbage collected when no longer referenced
- Custom DNS resolvers are properly cleaned up on garbage collection
- System default DNS resolvers are shared and not freed during garbage collection

### Performance Considerations

- Creating custom DNS resolvers has a small overhead
- DNS resolution performance depends on the chosen nameservers
- Multiple nameservers provide redundancy but may increase resolution time
- Consider caching DNS objects if used frequently

### Nameserver Format

- Only IPv4 addresses are currently supported
- Nameserver addresses must be valid IP addresses (not hostnames)
- Invalid nameservers are silently skipped
- If all nameservers are invalid, falls back to system default DNS

### Error Handling

- Invalid nameserver parameters cause Lua errors
- DNS resolution failures are handled by the calling module (tcpd/udpd)
- Failed custom DNS creation falls back to system default DNS

## Common Use Cases

### Using Public DNS Servers

```lua
local evdns = require('fan.evdns')

-- CloudFlare DNS
local cloudflare_dns = evdns.create("1.1.1.1")

-- Google DNS
local google_dns = evdns.create("8.8.8.8")

-- Quad9 DNS
local quad9_dns = evdns.create("9.9.9.9")

-- Multiple public DNS servers for redundancy
local public_dns = evdns.create({
    "1.1.1.1",      -- CloudFlare primary
    "1.0.0.1",      -- CloudFlare secondary
    "8.8.8.8",      -- Google primary
    "8.8.4.4"       -- Google secondary
})
```

### Custom DNS for Different Services

```lua
local evdns = require('fan.evdns')
local tcpd = require('fan.tcpd')

-- Different DNS resolvers for different purposes
local internal_dns = evdns.create("192.168.1.1")  -- Internal network DNS
local external_dns = evdns.create("8.8.8.8")      -- External public DNS

-- Connect to internal service using internal DNS
local internal_conn = tcpd.connect({
    host = "internal.company.com",
    port = 8080,
    evdns = internal_dns
})

-- Connect to external service using public DNS
local external_conn = tcpd.connect({
    host = "api.external.com",
    port = 443,
    ssl = true,
    evdns = external_dns
})
```

### DNS Testing and Development

```lua
local evdns = require('fan.evdns')

-- Test with different DNS servers
local test_servers = {
    cloudflare = evdns.create("1.1.1.1"),
    google = evdns.create("8.8.8.8"),
    quad9 = evdns.create("9.9.9.9")
}

-- Function to test DNS resolution speed
local function test_dns_performance(dns_name, dns_obj)
    local start_time = fan.get_time()

    local conn = tcpd.connect({
        host = "example.com",
        port = 80,
        evdns = dns_obj,
        onconnected = function()
            local end_time = fan.get_time()
            print(string.format("%s DNS: %.2fms", dns_name, (end_time - start_time) * 1000))
        end
    })
end

-- Test all DNS servers
for name, dns in pairs(test_servers) do
    test_dns_performance(name, dns)
end
```

## See Also

- [`fan.tcpd`](tcpd.md) - TCP client/server with custom DNS support
- [`fan.udpd`](udpd.md) - UDP client/server with custom DNS support
- [`fan.http`](http.md) - HTTP client with custom DNS support (via tcpd)