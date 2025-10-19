fan.udpd
========
### `conn = udpd.new(arg:table)`
create a udp socket.

### `dest = udpd.make_dest(host:string, port:number[, evdns:evdns_object])`
create a dest:[UDP_AddrInfo](#udp_addr_info), can be used in `conn:send`, this api is non-blocking.

**Parameters:**
- `host`: Hostname or IP address to resolve
- `port`: Port number for the destination
- `evdns` (optional): Custom DNS resolver for hostname resolution. If not provided, uses system default DNS.

**Examples:**
```lua
local udpd = require('fan.udpd')
local evdns = require('fan.evdns')

-- Using system default DNS
local dest1 = udpd.make_dest("example.com", 53)

-- Using custom DNS resolver
local dns = evdns.create("8.8.8.8")
local dest2 = udpd.make_dest("example.com", 53, dns)

-- Using multiple DNS servers
local dns_multi = evdns.create({"8.8.8.8", "1.1.1.1"})
local dest3 = udpd.make_dest("api.example.com", 8080, dns_multi)
```

### `dests = udpd.make_dests(host:string, port:number[, evdns:evdns_object])`
create multiple dest objects from host:port string (returns all resolved addresses), this api is non-blocking.

**Parameters:**
- `host`: Hostname or IP address to resolve
- `port`: Port number for the destinations
- `evdns` (optional): Custom DNS resolver for hostname resolution. If not provided, uses system default DNS.

**Returns:**
- `dests`: Table containing all resolved UDP_AddrInfo objects for the hostname

**Examples:**
```lua
local udpd = require('fan.udpd')
local evdns = require('fan.evdns')

-- Get all resolved addresses using system DNS
local dests1 = udpd.make_dests("example.com", 53)

-- Get all resolved addresses using custom DNS
local dns = evdns.create({"1.1.1.1", "8.8.8.8"})
local dests2 = udpd.make_dests("example.com", 53, dns)

-- Use the destinations
if dests2 then
    for i, dest in ipairs(dests2) do
        print("Destination " .. i .. ": " .. dest:getIP() .. ":" .. dest:getPort())
    end
end
```

---------
keys in the `arg`:

* `onread: function?`

	callback on receive data. arg1 => data:string, arg2 => [UDP_AddrInfo](#udp_addr_info)

	If `callback_self_first=true`, signature becomes: `function(self, data:string, dest:UDP_AddrInfo)`

* `onsendready: function?`

	callback on ready to send new data after `send_req`. no arg.

	If `callback_self_first=true`, signature becomes: `function(self)`

* `host: string?`

	host to send message to.

* `port: integer?`

	port to send message to.

* `bind_host: string?`

	local bind host, default "0.0.0.0"

* `bind_port: integer`

	local bind port

* `callback_self_first: boolean?`

	When enabled (true), passes the connection object as the first parameter to all callbacks.
	This helps avoid circular references when callbacks need to access the connection object.
	Default: false (for backward compatibility).

	**Example:**
	```lua
	-- Traditional approach (may cause circular references)
	local conn = udpd.new({
		bind_port = 8080,
		onread = function(data, dest)
			conn:send("response", dest)  -- Captures 'conn' in closure
		end
	})

	-- With callback_self_first=true (avoids circular references)
	local conn = udpd.new({
		bind_port = 8080,
		callback_self_first = true,
		onread = function(self, data, dest)
			self:send("response", dest)  -- No closure capture needed
		end
	})
	```

---------
conn apis:
### `send(buf, addr?)`
send out data buf, if addr:[UDP_AddrInfo](#udp_addr_info) specified, use it as the destination address, otherwise, use the host:port when create this udp object.

### `send_req()`
request to send data, when output buffer is available, onsendready will be called.

### `getPort()`
get the udp local binding port.

### `close()`
cleanup udp reference.

### `rebind()`
rebind the same host/port.(e.g. resume back in mobile device, rebind port.)

UDP_ADDR_INFO
=============

### `getHost():string`
get the income message host.

### `getPort():integer`
get the income message port.

### `getIP():string`
get the IP address from the destination address. Returns the IP address as a string, supporting both IPv4 and IPv6 formats.

**Note:** For IP addresses, `getIP()` and `getHost()` return the same value. The `getIP()` method is provided for clarity when you specifically need the IP address without any potential hostname resolution confusion.

**Example:**
```lua
local udpd = require('fan.udpd')

-- Create a destination
local dest = udpd.make_dest("192.168.1.100", 8080)

-- Get different address components
local ip = dest:getIP()      -- "192.168.1.100"
local host = dest:getHost()  -- "192.168.1.100" (same as getIP for IP addresses)
local port = dest:getPort()  -- 8080

print("IP:", ip)     -- IP: 192.168.1.100
print("Host:", host) -- Host: 192.168.1.100
print("Port:", port) -- Port: 8080

-- Usage in UDP server callback (using modern callback_self_first pattern)
local server = udpd.new({
    bind_port = 8080,
    callback_self_first = true,  -- Recommended to avoid circular references
    onread = function(self, data, dest)
        local client_ip = dest:getIP()
        local client_port = dest:getPort()
        print(string.format("Received '%s' from %s:%d", data, client_ip, client_port))

        -- Echo back to the same client using self parameter
        self:send("Echo: " .. data, dest)
    end
})
```

## Custom DNS Usage Examples

### Basic Custom DNS with UDP

```lua
local udpd = require('fan.udpd')
local evdns = require('fan.evdns')

-- Create custom DNS resolver
local dns = evdns.create("8.8.8.8")

-- Create UDP connection
local conn = udpd.new({
    bind_port = 8080,
    callback_self_first = true,
    onread = function(self, data, sender)
        print("Received:", data)
    end
})

-- Create destination using custom DNS
local dest = udpd.make_dest("api.example.com", 53, dns)
if dest then
    conn:send("Hello Server", dest)
end
```

### DNS Load Balancing with Multiple Destinations

```lua
local udpd = require('fan.udpd')
local evdns = require('fan.evdns')

-- Use multiple DNS servers for redundancy
local dns = evdns.create({"1.1.1.1", "8.8.8.8", "9.9.9.9"})

-- Get all resolved addresses for load balancing
local dests = udpd.make_dests("cdn.example.com", 80, dns)

if dests and #dests > 0 then
    local conn = udpd.new({bind_port = 0})

    -- Round-robin through all destinations
    for i, dest in ipairs(dests) do
        local message = "Request #" .. i
        conn:send(message, dest)
        print("Sent to " .. dest:getIP() .. ":" .. dest:getPort())
    end
end
```

### DNS Testing and Benchmarking

```lua
local udpd = require('fan.udpd')
local evdns = require('fan.evdns')

-- Test different DNS providers
local dns_providers = {
    cloudflare = evdns.create("1.1.1.1"),
    google = evdns.create("8.8.8.8"),
    quad9 = evdns.create("9.9.9.9"),
    default = evdns.create()  -- System default
}

-- Function to test DNS resolution performance
local function test_dns_resolution(name, dns)
    local start_time = os.clock()

    local dest = udpd.make_dest("example.com", 53, dns)

    if dest then
        local end_time = os.clock()
        local duration = (end_time - start_time) * 1000
        print(string.format("%s DNS: %.2f ms -> %s", name, duration, dest:getIP()))
    else
        print(name .. " DNS: Resolution failed")
    end
end

-- Test all DNS providers
for name, dns in pairs(dns_providers) do
    test_dns_resolution(name, dns)
end
```

**See Also:** [`fan.evdns`](evdns.md) for detailed DNS configuration options and [`fan.tcpd`](tcpd.md) for TCP usage with custom DNS.
