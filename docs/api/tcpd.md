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

* `onsendready: function?`

	callback on ready to send new data (stream output complete), no arg

* `ondisconnected: function?`

	ondisconnected callback, arg1 => reason:string

* `onconnected: function?`

	onconnected callback

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

* `onsendready: function?`

	on send ready callback, no arg.

* `ondisconnected`

	on client disconnected callback, arg1 => reason:string
