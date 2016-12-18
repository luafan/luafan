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
