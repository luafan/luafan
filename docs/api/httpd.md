fan.httpd
=========

### `serv_info_table = httpd.bind(arg:table)`
Create an HTTP server with advanced routing, security, and performance features. Returns a `serv_info_table` with server instance and connection details.

---------
keys in `serv_info_table`

* `serv` the server instance.
    * `rebind()` rebind the same host/port.(e.g. resume back in mobile device, rebind port.)
* `host` bind host.
* `port` bind port.
* `router` Router instance for advanced routing.

### **NEW: Router Methods**
* `get(path, handler)` Add GET route with optional path parameters (`:param`)
* `post(path, handler)` Add POST route with optional path parameters
* `put(path, handler)` Add PUT route with optional path parameters
* `delete(path, handler)` Add DELETE route with optional path parameters
* `use(middleware)` Add middleware function to request pipeline
* `set_not_found(handler)` Set custom 404 handler
* `get_stats()` Get performance statistics

### **NEW: Security & Performance Features**
* Built-in rate limiting (configurable via config)
* Automatic security headers (X-XSS-Protection, X-Frame-Options, etc.)
* Intelligent gzip compression based on content type and size
* HTTP/1.1 Keep-Alive support with connection reuse
* Performance monitoring with real-time statistics
* RFC 7230/7231 compliant HTTP parsing

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
**ENHANCED**: URL parameters from query string AND form data. With new router, also includes path parameters (e.g., `/user/:id` → `params.id`)

### `body:string`
Request body content. **NEW**: Automatically parsed and validated against configured limits.

### `headers:table`
**ENHANCED**: RFC 7230 compliant header parsing. Format: `{singlekey = "value", multikey = {"value1", "value2"}}`

### `remoteip:string`
**ENHANCED**: Used for rate limiting and security monitoring.

### `remoteport:integer`

### **NEW PROPERTIES:**
### `version:string`
HTTP version (1.0 or 1.1)

### `content_length:integer`
**ENHANCED**: Strict RFC validation with configurable limits

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

## **NEW: Advanced Features**

### Response State Management
The HTTP response now uses a state machine with proper validation:
- `PENDING` → `STARTED` → `COMPLETED`
- Prevents invalid state transitions
- Ensures proper HTTP protocol compliance

### Security Features
- **Rate Limiting**: Automatic per-IP request limiting
- **Security Headers**: X-XSS-Protection, X-Frame-Options, X-Content-Type-Options
- **Input Validation**: Strict RFC 7230 header parsing
- **Content Limits**: Configurable request size limits

### Performance Features
- **Intelligent Gzip**: Only compresses suitable content types and sizes
- **Keep-Alive**: HTTP/1.1 connection reuse
- **Performance Stats**: Real-time request metrics

## Configuration Options

Add these to your `config.lua`:

```lua
config = {
    -- Content limits
    max_content_length = 10485760,        -- 10MB default
    max_header_value_size = 8192,         -- 8KB per header
    max_uri_length = 2048,                -- 2KB URI limit

    -- Compression
    gzip_min_size = 1024,                 -- Min size for compression

    -- Security
    enable_rate_limiting = true,          -- Enable rate limiting
    rate_limit_requests = 100,            -- Requests per window
    rate_limit_window = 60,               -- Window in seconds
    enable_security_middleware = true,    -- Security headers
    security_headers = {                  -- Custom headers
        ["Strict-Transport-Security"] = "max-age=31536000"
    },

    -- Performance
    enable_performance_monitoring = true, -- Enable stats
    connection_timeout = 30,              -- Keep-alive timeout
}
```

## Example Usage

### Basic Server with Router
```lua
local httpd = require "modules.fan.httpd.httpd"
local server = httpd.bind({host = "0.0.0.0", port = 8080})

-- Add routes
server:get("/", function(ctx)
    ctx:reply(200, "OK", "Hello World!")
end)

server:get("/user/:id", function(ctx)
    local user_id = ctx.params.id
    ctx:reply(200, "OK", "User: " .. user_id)
end)

-- Add middleware
server:use(function(ctx, next)
    print("Request:", ctx.method, ctx.path)
    next()
end)

-- Custom 404
server:set_not_found(function(ctx)
    ctx:reply(404, "Not Found", "Page not found")
end)

fan.loop()
```

### Performance Monitoring
```lua
local server = httpd.bind({host = "0.0.0.0", port = 8080})

server:get("/stats", function(ctx)
    local stats = server:get_stats()
    ctx:addheader("Content-Type", "application/json")
    ctx:reply(200, "OK", json.encode(stats))
end)
```
