fan.udpd
========
### `conn = udpd.new(arg:table)`
create a udp socket.

### `dest = udpd.make_dest(host:string, port:number)`
create a dest:[UDP_AddrInfo](#udp_addr_info), can be used in `conn:send`, this api is non-blocking.

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
