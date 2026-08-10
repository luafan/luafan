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

	stream input callback. Default signature: `function(self, buffer_in:string)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function(buffer_in:string)`.

* `onsendready: function?`

	callback on ready to send new data (stream output complete). Default signature: `function(self)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function()`.

* `ondisconnected: function?`

	ondisconnected callback. Default signature: `function(self, reason:string)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function(reason:string)`.

* `onconnected: function?`

	onconnected callback. Default signature: `function(self)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function()`.

* `ssl: boolean?`

	whether ssl connection, default false.

* `ssl_host: string?`

	tls extension host name for ssl handshake, use `host` if this field is not set.

* `cainfo: string?`

	file path to a CA bundle (PEM). If both `cainfo` and `capath` are omitted,
	tcpd loads OpenSSL default verify paths, then common system bundles
	(`cacert.pem` / `cert.pem` in cwd, `/etc/ssl/cert.pem`, distro CA bundles, etc.).

* `capath: string?`

	directory of hashed CA certs (OpenSSL `CApath`)

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

	Passes the connection object as the first parameter to all callbacks, which avoids
	circular references when callbacks need to access the connection object.
	**Default: true.** Set `false` explicitly to use the legacy signatures without `self`.

	**Example:**
	```lua
	-- Default (callback_self_first = true): connection is the first arg
	local conn = tcpd.connect({
		host = "example.com", port = 80,
		onread = function(self, data)
			self:send("response")  -- No closure capture needed
		end
	})

	-- Legacy signatures (opt out explicitly)
	local conn
	conn = tcpd.connect({
		host = "example.com", port = 80,
		callback_self_first = false,
		onread = function(data)
			conn:send("response")  -- Captures 'conn' in closure
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

	new client connection callback. Default signature (with `callback_self_first=true`):
	`function(self, accept:`[accept_connection](#acceptconnection)`)` — `self` is the
	server object, `accept` is the new client connection.

	If `callback_self_first=false` is set explicitly, signature becomes:
	`function(accept:`[accept_connection](#acceptconnection)`)`.

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

	Passes the connection object as the first parameter to the accept connection
	callbacks (`onread`, `onsendready`, `ondisconnected`). Inherited from the server's
	setting when not specified on the accept connection.
	**Default: true.** Set `false` explicitly to use the legacy signatures without `self`.

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

	on read message from client callback. Default signature: `function(self, databuf:string)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function(databuf:string)`.

* `onsendready: function?`

	on send ready callback. Default signature: `function(self)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function()`.

* `ondisconnected`

	on client disconnected callback. Default signature: `function(self, reason:string)`.

	If `callback_self_first=false` is set explicitly, signature becomes: `function(reason:string)`.
