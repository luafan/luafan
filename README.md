# luafan

## Modules
fan
===
### `fan.loop(startup:function?)`
start the event loop, can pass startup function or nothing.

### `fan.loopbreak()`
break the event loop, make sure call this api after all the daemon service(tcpd.bind, udpd.bind, httpd.bind) has been garbage collected.

### `fan.sleep(sec:number)`
sleep for any seconds, e.g. 0.1 or 10

### `fan.data2hex(data:string)`
convert binary data to hex string.

### `fan.hex2data(data:string)`
convert hex string to binary data.

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

	stream output complete callback, no arg

* `ondisconnected: function?`

	ondisconnected callback, arg1 => reason:string

* `onconnected: function?`

	onconnected callback

* `ssl: boolean?`

	whether ssl connection, default false.

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
conn apis:

### `send(buf)`

send out data buf.

### `close()`

close connection, ondisconnected may not callback.

### `reconnect()`

reconnect to the destination.

### `pause_read()`

pause onread callback.

### `resume_read()`

resume onread callback.

---------
### `serv = tcpd.bind(arg:table)`

listening on tcp socket.

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
	

* `send_buffer_size: integer?`

	client connection send buffer size.


* `receive_buffer_size: integer?`

	client connection receive buffer size.

---------

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

---------

fan.httpd
=========

### `serv,port = httpd.bind(arg:table)`
create a simple http server.

---------
keys in the `arg`:

* `host: string?`

http service listening host, default "0.0.0.0"

* `port: integer?`

http service listening port, leave empty for random port that available.

* `onService`

on request callback, arg1 => [http_request](#http_request), arg2 => [http_response](#http_response)


* `cert`

ssl support, only available with libevent2.1.5+

* `key`

ssl support, only available with libevent2.1.5+






fan.http
========

fan.mariadb
===========

---------

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

---------


UDP_FROM_INFO
=============

### `getHost():string`
get the income message host.

### `getPort():integer`
get the income message port.

---------

HTTP_REQUEST
============

properties:
### `path:string`

### `query:string`

### `method:string`

### `params:table`

### `body:string`
income body.

### `headers:table`
format as `{singlekey = "value", multikey = {"value1", "value2"}}`

### `remoteip:string`

### `remoteport:integer`


---------

apis:
### `available():integer`
return available input stream length to read.

### `read()`
read data buf from input stream, return nil if no data.


HTTP_RESPONSE
=============
---------

apis:
### `addheader(k:string, v:string)`
add one response header.

### `reply(status_code:integer, msg:string, bodydata:string?)`
response to client.

### `reply_start(status_code:integer, msg:string)`

### `reply_chunk(data:string)`

### `reply_end()`

Samples
=======

Hello World
===========
```
local fan = require "fan"

local function main(time)
    while true do
        fan.sleep(time)
        print(time, os.time())
    end
end

main_co1 = coroutine.create(main)
print(coroutine.resume(main_co1, 2))
main_co2 = coroutine.create(main)
print(coroutine.resume(main_co2, 3))

fan.loop()
```

more samples under project samples folder.