# luafan

## Non-blocking Modules
* [fan](#fan) common module.
* [fan.tcpd](#fantcpd) tcp protocol module.
* [fan.udpd](#fanudpd) udp protocol module.
* [fan.fifo](#fanfifo) fifo pipe module.
* [fan.httpd](#fanhttpd) httpd webserver module.
* [fan.http](#fanhttp) http request module.
* [fan.mariadb](#fanmariadb) mariadb client module.

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

fan.fifo
========
### `conn = fifo.connect(arg:table)`

connect to fifo file, create a new one if not exist, [Samples](#fifosample)

---------
keys in the `arg`:

* `name: string`

	fifo file name/path.
	
* `mode: integer`

	fifo file permission, default 0600.

* `rwmode: string`

	fifo read/write mode, can be "r" "w" "rw", default "r"

* `onread: function`

	stream input callback, available if `rwmode` is "r", arg1 => buffer_in:string

* `onsendready: function`

	stream output complete callback, available if `rwmode` is "w", no arg
	
* `ondisconnected: function`

	reader disconnected callback, available if `rwmode` is "w", arg1 => reason:string
	
---------
conn apis:

### `send_req()`
request to send data, if possiable, `onsendready` will callback.

### `send(data:string)`
do send data inside `onsendready`

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

---------

fan.http
========
### `escape(str:string)`
call to curl_escape

### `unescape(str:string)`
call to curl_unescape

### `cookiejar(path:string)`
set the default cookiejar path

### `cainfo(path:string)`
set the default cainfo path

### `capath(path:string)`
set the default capath path

### `get(arg:table or string)`
### `post(arg:table or string)`
### `put(arg:table or string)`
### `head(arg:table or string)`
### `update(arg:table or string)`
### `delete(arg:table or string)`

---------

if arg is string, use it as `url` key in the following arg table.

---------
keys in the `arg`:

* `url: string`
 
	the url path string

* `headers: table?`

	the request headers, format `{key = value, key2 = value2}`

* `verbose: boolean?`

	save all request message to file named verbose.log
	
* `dns_servers: string?`

	customize the dns servers, format `1.2.3.4,2.3.4.5`, refer to `CURLOPT_DNS_SERVERS`
	
* `onprogress: function?`

	progress callback, arg1 => dltotal:integer, arg2 => dlnow:integer, arg3 => ultotal:integer, arg4 => ulnow:integer, refer to `CURLOPT_PROGRESSFUNCTION`

* `timeout: number?`

	low speed timeout, less than 0 in 1min, refer to `CURLOPT_LOW_SPEED_TIME`
	
* `conntimeout: number?`

	connection timeout, `CURLOPT_TIMEOUT`
	
* `ssl_verifypeer: integer?`, refer to `CURLOPT_SSL_VERIFYPEER`

* `ssl_verifyhost: integer?`, refer to `CURLOPT_SSL_VERIFYHOST`

* `sslcert: string?`, refer to `CURLOPT_SSLCERT`

* `sslcertpasswd: string?`, refer to `CURLOPT_SSLCERTPASSWD`

* `sslcerttype: string?`, refer to `CURLOPT_SSLCERTTYPE`

* `sslkey: string?`, refer to `CURLOPT_SSLKEY`

* `sslkeypasswd: string?`, refer to `CURLOPT_SSLKEYPASSWD`

* `sslkeytype: string?`, refer to `CURLOPT_SSLKEYTYPE`

* `cainfo: string?`, refer to `CURLOPT_CAINFO`

* `capath: string?`, refer to `CURLOPT_CAPATH`

* `proxytunnel: integer?`, refer to `CURLOPT_HTTPPROXYTUNNEL`

* `proxy: string?`, refer to `CURLOPT_PROXY`, we did support http proxy only.

* `proxyport: integer?`, refer to `CURLOPT_PROXYPORT`

* `onsend: function?`

	callback for data upload, refer to `CURLOPT_READFUNCTION`
	
* `body: string?`

	full body for data to be sent, available if onsend not defined.

* `onreceive: function?`

	callback on receive data from remote, if not set, get the response body from reponsetable.body, arg1 => data:string
	
* `onheader: function?`

	callback on receive the header part, arg1 => header:table
	
* `oncomplete: function?`

	callback on http request complete, the http operation return nothing, if not set, the http operation return the responsetable only the request completed. arg1 => responsetable:table

* `forbid_reuse: integer?` forbidden connection reuse on http/1.1, refer to `CURLOPT_FORBID_REUSE`

* `cookiejar: string?`

	cookiejar used by this request, refer to `CURLOPT_COOKIEJAR`,`CURLOPT_COOKIEFILE`

---------

### responsetable

---------
keys in the responsetable:

* `responseCode: integer`

	response code of this request, available on `onheader` callback also.

* `body: string`

	response body, available on `onreceive` callback not set.
	
* `headers: table`

	response headers, format as `{singlekey = "value", multikey = {"value1", "value2"}}`

* `cookies: table`

	list of cookies.
	
* `error: string`

	error during request, must check this to make sure http request fully complete.


fan.mariadb
===========

### `conn = connect(dbname:string, username:string, password:string, host:string?, port:integer?)`

connect to mariadb server.

---------
 `conn` apis

* `close()` close connecion
* `ping()` mariadb ping
* `escape()` escape string as mariadb format.
* `execute(sql:string)` execute a sql script, return [cursor](#cursor)
* `setcharset(charset:string)` set connection charset.
* `prepare(sql:string)` prepare a sql [prepared statement](#preparedstatement).
* `commit()` available for innodb only.
* `rollback()` available for innodb only.
* `autocommit()` available for innodb only.
* `getlastautoid()` get the last autoincrement id created by this connection.

---------

PreparedStatement
=================

### `close()`
close statement.
### `bind_param(...)`
bind statement parameters.
### `execute()`
execute this statement
### `fetch()`
get the result row, return `success:boolean, field1, field2, ...`

Cursor
======
### `close()`
close cursor.

### `getcolnames()`
return the list of column names.

### `getcoltypes()`
return the list of column types.

### `fetch()`
get the result row, return table, format as `{columnkey = value, ...}`

### `numrows()`
get the row count.

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

There are 3 spec in luarocks, luafan/luafanlite/luafanmicro

* `luafan` contains `fifo` `tcpd` `udpd` `http` `httpd` `mariadb` depends `libevent curl openssl mariadb`



* `luafanlite` contains `fifo` `tcpd` `udpd` `http` `httpd` depends `libevent curl openssl`



* `luafanmicro` contains `fifo` `tcpd` `udpd` `httpd` depends `libevent`


Hello World
===========
1. complie latest libevent or install libevent-dev in your machine, e.g. for ubuntu, `sudo apt-get install libevent-dev`

2. `luarocks install luafanmicro`

3. save sample lua as "hello.lua", then run with `luajit hello.lua` or  `lua5.3 hello.lua` (module from luarocks only support luajit or lua5.2+)

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

FIFO Sample
===========

```
local fan = require "fan"
local fifo = require "fan.fifo"

p1 = fifo.connect{
    name = "fan.fifo",
    rwmode = "r",
    onread = function(buf)
        print("onread", #(buf), buf)
    end
}
fan.loop()
```
`shell# echo test > fan.fifo`

Issues
======
1. Ctrl+C may crash with "Segmentation fault", the reason is Ctrl+C break the loop, but there are daemon running, not a big issue, and will be fix later.


more samples under project samples folder.