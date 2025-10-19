fan.tcpd
========
### `conn = tcpd.connect(arg:table)`

connect to remote tcp server.

---------
keys in the `arg`:

* `host: string`

	host to connect.

* `port: integer`

	port to connect.

* `onread: function?`

	stream input callback, arg1 => buffer_in:string

	If `callback_self_first=true`, signature becomes: `function(self, buffer_in:string)`

* `onsendready: function?`

	callback on ready to send new data (stream output complete), no arg

	If `callback_self_first=true`, signature becomes: `function(self)`

* `ondisconnected: function?`

	ondisconnected callback, arg1 => reason:string

	If `callback_self_first=true`, signature becomes: `function(self, reason:string)`

* `onconnected: function?`

	onconnected callback

	If `callback_self_first=true`, signature becomes: `function(self)`

* `ssl: boolean?`

	whether ssl connection, default false.

* `ssl_host: string?`

	tls extension host name for ssl handshake, use `host` if this field is not set.

* `cainfo: string?`

	file path to cacert.pem

* `capath: string?`

	directory path of cacert.pem

* `ssl_verifyhost: integer?`

	verify server certificate's host. 1 verify, 0 ignore.

* `ssl_verifypeer: integer?`

	verify ssl certificate, 1 verify, 0 ignore.

* `pkcs12.path: string?`

	path to pkcs12 file, option available only if cainfo/capath has not been set. This option can be used to connect to apple's APNS server.

* `pkcs12.password: string?`

	password of pkcs12 file.

* `read_timeout: number?`

	connection's read timeout.

* `write_timeout: number?`

	connection's write timeout.

* `callback_self_first: boolean?`

	When enabled (true), passes the connection object as the first parameter to all callbacks.
	This helps avoid circular references when callbacks need to access the connection object.
	Default: false (for backward compatibility).

	**Example:**
	```lua
	-- Traditional approach (may cause circular references)
	local conn = tcpd.connect({
		host = "example.com", port = 80,
		onread = function(data)
			conn:send("response")  -- Captures 'conn' in closure
		end
	})

	-- With callback_self_first=true (avoids circular references)
	local conn = tcpd.connect({
		host = "example.com", port = 80,
		callback_self_first = true,
		onread = function(self, data)
			self:send("response")  -- No closure capture needed
		end
	})
	```

* `evdns: evdns_object?`

	Custom DNS resolver for hostname resolution. If not provided, uses system default DNS configuration.
	This allows you to specify custom nameservers for DNS lookups.

	**Creating EVDNS objects:**
	```lua
	local evdns = require('fan.evdns')

	-- Use system default DNS
	local dns_default = evdns.create()

	-- Use custom single nameserver
	local dns_google = evdns.create("8.8.8.8")

	-- Use multiple custom nameservers
	local dns_multi = evdns.create({"8.8.8.8", "1.1.1.1"})
	```

	**Usage with tcpd.connect:**
	```lua
	local evdns = require('fan.evdns')
	local tcpd = require('fan.tcpd')

	-- Create custom DNS resolver
	local dns = evdns.create("8.8.8.8")

	-- Connect using custom DNS
	local conn = tcpd.connect({
		host = "api.example.com",
		port = 443,
		ssl = true,
		evdns = dns,  -- Use custom DNS for hostname resolution
		callback_self_first = true,
		onconnected = function(self)
			print("Connected using custom DNS resolver")
		end
	})
	```

	**Use Cases:**
	- Using public DNS servers (CloudFlare, Google, Quad9)
	- Bypassing DNS filtering or censorship
	- Testing with specific DNS configurations
	- Corporate environments with custom DNS servers
	- Performance optimization with faster DNS servers

	**See Also:** [`fan.evdns`](evdns.md) for detailed DNS configuration options.

---------
`conn` apis:

### `send(buf)`

send out data buf.

### `close()`

close connection, ondisconnected may not callback.

### `reconnect()`

reconnect to the destination.

### `pause_read()`

pause `onread` callback.

### `resume_read()`

resume `onread` callback.

---------
### `serv = tcpd.bind(arg:table)`

listening on tcp socket.

---------
`serv` apis

* `close()` shutdown the server.
* `rebind()` rebind the same host/port.(e.g. resume back in mobile device, rebind port.)

---------
keys in the `arg`:

* `host: string?`

	bind host, if not set, bind to "0.0.0.0"

* `port: integer?`

	port to listen, if not set, use random port which is available.

* `onaccept: function`

	new client connection callback, arg1 => [accept_connection](#acceptconnection)

* `ssl: boolean?`

	listening as ssl server, default false.

* `cert: string?`

	ssl cert file path.

* `key: string?`

	ssl key file path.

* `onsslhostname: function`

	ssl hostname (servername extension) callback, arg1 => hostname:string

* `send_buffer_size: integer?`

	client connection send buffer size.


* `receive_buffer_size: integer?`

	client connection receive buffer size.

* `callback_self_first: boolean?`

	When enabled (true), passes the connection object as the first parameter to server callbacks.
	This applies to accept connection callbacks (`onread`, `onsendready`, `ondisconnected`).
	Default: false (for backward compatibility).

AcceptConnection
================
### `send(buf)`
send data buf to client.

### `close()`
close client connection.

### `flush()`
flush data to client.

### `remoteinfo()`
return the client connection info table.
`{ip = "1.2.3.4", port = 1234}`

### `pause_read()`

pause `onread` (from bind) callback.

### `resume_read()`

resume `onread` (from bind) callback.

### `bind(arg:table)`
working on single connection.

---------
keys in the `arg`:

* `onread: function?`

	on read message from client callback, arg1 => databuf:string

	If server's `callback_self_first=true`, signature becomes: `function(self, databuf:string)`

* `onsendready: function?`

	on send ready callback, no arg.

	If server's `callback_self_first=true`, signature becomes: `function(self)`

* `ondisconnected`

	on client disconnected callback, arg1 => reason:string

	If server's `callback_self_first=true`, signature becomes: `function(self, reason:string)`
