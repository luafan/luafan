fan.udpd
========
### `conn = udpd.new(arg:table)`
create a udp socket.

---------
keys in the `arg`:

* `onread: function?`

	callback on receive data. arg1 => data:string, arg2 => [UDP_FromInfo](#udp_from_info)

* `onsendready: function?`

	callback on data sent. no arg.

* `host: string?`

	host to send message to.

* `port: integer?`

	port to send message to.

* `bind_host: string?`

	local bind host, default "0.0.0.0"

* `bind_port: integer`

	local bind port

---------
conn apis:
### `send(buf)`
send out data buf.

### `send_req()`
request to send data, when output buffer is available, onsendready will be called.

UDP_FROM_INFO
=============

### `getHost():string`
get the income message host.

### `getPort():integer`
get the income message port.
