fan.httpd
=========

### `serv_info_table = httpd.bind(arg:table)`
create a simple http server, `serv_info_table` contains `host` and `port`.

---------
keys in `serv_info_table`

* `serv` the server instance.
    * `rebind()` rebind the same host/port.(e.g. resume back in mobile device, rebind port.)
* `host` bind host.
* `port` bind port.

---------
keys in the `arg`:

* `host: string?`

http service listening host, default "0.0.0.0"

* `port: integer?`

http service listening port, leave empty for random port that available.

* `onService`

on request callback, arg1 => [http_request](#http_request), arg2 => [http_response](#http_response)


* `cert`

ssl support, if using core `config.httpd_using_core`, this property only available with libevent2.1.5+

* `key`

ssl support, if using core `config.httpd_using_core`, this property only available with libevent2.1.5+

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

apis:
### `available():integer`
return available input stream length to read.

### `read()`
read data buf from input stream, return nil if no data.

HTTP_RESPONSE
=============

apis:
### `addheader(k:string, v:string)`
add one response header.

### `reply(status_code:integer, msg:string, bodydata:string?)`
response to client.

### `reply_start(status_code:integer, msg:string)`

### `reply_chunk(data:string)`

### `reply_end()`
