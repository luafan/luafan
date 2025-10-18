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
