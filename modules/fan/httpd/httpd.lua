local fan = require "fan"
local connector = require "fan.connector"
local http = require "fan.http"
local zlib = require "zlib"
local config = require "config"

-- Router System
local Router = {}
Router.__index = Router

function Router.new()
    return setmetatable({
        routes = {},
        middlewares = {},
        not_found_handler = nil
    }, Router)
end

function Router:add_route(method, path, handler)
    method = string.upper(method)
    if not self.routes[method] then
        self.routes[method] = {}
    end

    -- Convert path parameters (:param) to regex patterns
    local pattern = path
    local param_names = {}

    for name in path:gmatch(":([%w_]+)") do
        table.insert(param_names, name)
    end

    pattern = pattern:gsub(":([%w_]+)", "([^/]+)")
    pattern = "^" .. pattern .. "$"

    table.insert(self.routes[method], {
        original_path = path,
        pattern = pattern,
        param_names = param_names,
        handler = handler
    })
end

function Router:get(path, handler) self:add_route("GET", path, handler) end
function Router:post(path, handler) self:add_route("POST", path, handler) end
function Router:put(path, handler) self:add_route("PUT", path, handler) end
function Router:delete(path, handler) self:add_route("DELETE", path, handler) end

function Router:use(middleware)
    table.insert(self.middlewares, middleware)
end

function Router:match(method, path)
    local routes = self.routes[string.upper(method)]
    if not routes then return nil, nil end

    for _, route in ipairs(routes) do
        local matches = {path:match(route.pattern)}
        if #matches > 0 then
            local params = {}
            for i, name in ipairs(route.param_names) do
                params[name] = matches[i]
            end
            return route.handler, params
        end
    end
    return nil, nil
end

function Router:execute_middlewares(ctx, final_handler)
    local index = 1
    local middlewares = self.middlewares

    local function next()
        local middleware = middlewares[index]
        if middleware then
            index = index + 1
            middleware(ctx, next)
        else
            if final_handler then final_handler(ctx) end
        end
    end
    next()
end

function Router:handle(ctx)
    if ctx._http_error then
        ctx:reply(ctx._http_error.code, ctx._http_error.message, ctx._http_error.details or "")
        return
    end

    local handler, params = self:match(ctx.method, ctx.path)
    if handler then
        ctx.params = params
        self:execute_middlewares(ctx, handler)
    else
        if self.not_found_handler then
            self.not_found_handler(ctx)
        else
            ctx:reply(404, "Not Found", "The requested resource was not found.")
        end
    end
end

-- Response State Management
local ResponseState = {
    PENDING = "pending",
    STARTED = "started",
    COMPLETED = "completed",
    ERROR = "error"
}

local function validate_state_transition(current, new_state)
    local valid_transitions = {
        [ResponseState.PENDING] = {ResponseState.STARTED, ResponseState.COMPLETED, ResponseState.ERROR},
        [ResponseState.STARTED] = {ResponseState.COMPLETED, ResponseState.ERROR},
        [ResponseState.COMPLETED] = {},
        [ResponseState.ERROR] = {}
    }

    local allowed = valid_transitions[current or ResponseState.PENDING]
    if allowed then
        for _, state in ipairs(allowed) do
            if state == new_state then return true end
        end
    end
    return false
end

local function readheader(ctx, input)
    while not ctx.header_complete do
        local line, breakflag = input:readline()
        if not line then
            break
        elseif not breakflag then
            return false, #line + 2
        else
            if #(line) == 0 then
                ctx.header_complete = true

                -- RFC 7230: HTTP/1.1 requires Host header
                if ctx.version == "1.1" and not ctx.headers["host"] then
                    ctx._http_error = {code = 400, message = "Bad Request", details = "Missing Host header"}
                end
            else
                if ctx.first_line then
                    -- RFC 7230: header-field = field-name ":" OWS field-value OWS
                    local k, v = string.match(line, "^([!#$%%&'*+%-%.0-9A-Z^_`a-z|~]+):[ \t]*(.*)")
                    if not k or not v then
                        -- Record invalid headers but continue processing
                        ctx.invalid_headers = (ctx.invalid_headers or 0) + 1
                        if ctx.invalid_headers > 100 then -- RFC 7230: reasonable limit
                            ctx._http_error = {code = 400, message = "Bad Request", details = "Too many invalid headers"}
                            return true
                        end
                        -- Skip this line but continue processing
                    else
                        -- RFC 7230: Remove trailing whitespace from field value
                        v = v:gsub("[ \t]+$", "")

                        -- Header field name is case-insensitive
                        k = string.lower(k)

                        -- Check for oversized header value
                        if #v > 8192 then -- Reasonable limit
                            ctx._http_error = {code = 400, message = "Bad Request", details = "Header value too large"}
                            return true
                        end

                        local old = ctx.headers[k]
                        if old then
                            if type(old) == "table" then
                                table.insert(old, v)
                            else
                                ctx.headers[k] = {old, v}
                            end
                        else
                            ctx.headers[k] = v
                        end
                    end
                else
                    -- RFC 7230: Request-Line = Method SP Request-URI SP HTTP-Version CRLF
                    ctx.method, ctx.path, ctx.version = string.match(line, "^([A-Z]+) ([^ ]+) HTTP/([0-9]+%.[0-9]+)$")
                    if not ctx.method or not ctx.path or not ctx.version then
                        -- RFC 7230: Invalid request line should result in 400 Bad Request
                        return false, "400 Bad Request"
                    end

                    -- Validate HTTP version (only 1.0 and 1.1 supported)
                    if ctx.version ~= "1.0" and ctx.version ~= "1.1" then
                        ctx._http_error = {code = 505, message = "HTTP Version Not Supported"}
                        return true -- Continue to process error
                    end

                    -- RFC 7230: URI length validation (reasonable limit)
                    if #ctx.path > 2048 then
                        ctx._http_error = {code = 414, message = "URI Too Long"}
                        return true -- Continue to process error
                    end
                    local a, b = string.find(ctx.path, "?", 1, true)
                    if a and b then
                        ctx.query = string.sub(ctx.path, b + 1)
                        ctx.path = string.sub(ctx.path, 1, a - 1)
                    end
                    ctx.first_line = true
                end
            end
        end
    end

    return true
end

local context_mt = {}

function context_reply_fillheader(t, ctx, code, message)
    table.insert(t, string.format("HTTP/%s %d %s\r\n", ctx.version or "1.1", code, message))

    -- Add Keep-Alive headers before other headers
    ctx:set_keep_alive_headers()

    for i, v in ipairs(ctx.out_headers) do
        table.insert(t, string.format("%s: %s\r\n", v.key, v.value))
    end

    if not ctx._content_type_set then
        table.insert(t, string.format("Content-Type: %s\r\n", ctx.content_type))
    end

    if not ctx._no_gzip and not ctx._content_encoding_set and not ctx._content_length_set then
        local accept = ctx.headers["accept-encoding"]
        if accept and type(accept) == "string" and string.find(accept, "gzip") then
            table.insert(t, "Content-Encoding: gzip\r\n")
            ctx._gzip_body = true
        end
    end
end

function context_mt:_ready_for_reply()
    local t = {}

    while self:available() > 0 do
        local buff = self:read()
        table.insert(t, buff)
    end

    self.body = table.concat(t)
end

function context_mt:transition_state(new_state)
    local current = self.reply_status or ResponseState.PENDING
    if not validate_state_transition(current, new_state) then
        error(string.format("Invalid state transition: %s -> %s", current, new_state))
    end
    self.reply_status = new_state
end

function context_mt:reply_start(code, message)
    if self.reply_status then
        return
    end

    self:_ready_for_reply()
    self:transition_state(ResponseState.STARTED)

    local t = {}
    context_reply_fillheader(t, self, code, message)

    table.insert(t, "Transfer-Encoding: chunked\r\n")
    table.insert(t, "\r\n")

    if self._gzip_body then
        local apt = self.apt
        -- RFC 7230: Avoid nested coroutines to prevent resource leaks
        self._gzip_outputstream =
            zlib.deflate(
            {
                write = function(stream, data)
                    -- Direct send without wrapping in coroutine
                    apt:send(string.format("%X\r\n%s\r\n", #data, data))
                end
            },
            9,
            nil,
            15 + 16
        )
    end

    self.apt:send(table.concat(t))
end

function context_mt:reply_chunk(data)
    if not self.reply_status or not data or #(data) == 0 then
        return
    end

    if self._gzip_body then
        self._gzip_outputstream:write(data)
        self._gzip_outputstream:flush()
    else
        local output = string.format("%X\r\n%s\r\n", #data, data)
        self.apt:send(output)
    end
end

function context_mt:reply_end()
    if self.reply_status ~= ResponseState.STARTED then
        return
    end
    self:transition_state(ResponseState.COMPLETED)

    if self._gzip_outputstream then
        self._gzip_outputstream:close()
        self._gzip_outputstream = nil
    end

    self.apt:send("0\r\n\r\n")
end

function context_mt:available()
    return self.content_length - self.read_offset
end

function context_mt:read()
    if self:available() <= 0 then
        return nil
    end

    local input = self.apt:receive()
    if input then
        local buff = input:GetBytes()
        self.read_offset = self.read_offset + #(buff)
        return buff
    end
end

function context_mt:reply(code, message, body)
    if self.reply_status then
        return
    end
    self:transition_state(ResponseState.COMPLETED)

    self:_ready_for_reply()

    local t = {}
    local compressed = false

    if body and #body > 0 then
        body, compressed = self:compress_body(body)
        if compressed then
            self:addheader("Content-Encoding", "gzip")
            self._content_encoding_set = true
        end
    else
        self._no_gzip = true
    end

    context_reply_fillheader(t, self, code, message)

    if not self._content_length_set then
        table.insert(t, string.format("Content-Length: %d\r\n", body and #body or 0))
    end

    table.insert(t, "\r\n")

    if body then
        table.insert(t, body)
    end

    self.apt:send(table.concat(t))
end

function context_mt:addheader(k, v)
    local lk = k:lower()
    if lk == "content-type" then
        self._content_type_set = true
    elseif lk == "content-length" then
        self._content_length_set = true
    elseif lk == "content-encoding" then
        self._content_encoding_set = true
    end
    table.insert(self.out_headers, {key = k, value = v})
end

-- Keep-Alive Support
function context_mt:should_keep_alive()
    -- HTTP/1.1 defaults to keep-alive
    if tonumber(self.version) >= 1.1 then
        local connection = self.headers["connection"]
        return not connection or connection:lower() ~= "close"
    end

    -- HTTP/1.0 requires explicit Connection: keep-alive
    local connection = self.headers["connection"]
    return connection and connection:lower() == "keep-alive"
end

function context_mt:set_keep_alive_headers()
    if self:should_keep_alive() then
        -- Only add keep-alive header for HTTP/1.0
        if tonumber(self.version) < 1.1 then
            self:addheader("Connection", "keep-alive")
            -- Add timeout information
            local timeout = config.connection_timeout or 30
            self:addheader("Keep-Alive", string.format("timeout=%d, max=100", timeout))
        end
    else
        self:addheader("Connection", "close")
    end
end

-- Enhanced Gzip Compression
function context_mt:should_compress(body)
    if not body or #body == 0 then
        return false
    end

    -- Check client support
    local accept = self.headers["accept-encoding"]
    if not accept or not accept:find("gzip") then
        return false
    end

    -- Only compress larger content (configurable threshold)
    local min_size = config.gzip_min_size or 1024
    if #body < min_size then
        return false
    end

    -- Check content type for compression suitability
    local content_type = self.content_type or "text/plain"
    local compressible_types = {
        "text/", "application/json", "application/javascript",
        "application/xml", "application/xhtml+xml"
    }

    local should_compress = false
    for _, type_prefix in ipairs(compressible_types) do
        if content_type:find(type_prefix, 1, true) then
            should_compress = true
            break
        end
    end

    return should_compress
end

function context_mt:compress_body(body)
    if not self:should_compress(body) then
        return body, false
    end

    local compressed = zlib.compress(body, nil, nil, 31)
    if compressed and #compressed < #body * 0.9 then -- Only use if 10%+ reduction
        return compressed, true
    end

    return body, false
end

-- Security and Rate Limiting
local SecurityLimits = {
    MAX_HEADER_SIZE = 8192,      -- 8KB per header
    MAX_HEADERS_COUNT = 100,     -- Maximum headers per request
    MAX_REQUEST_SIZE = 10485760, -- 10MB default request size
    MAX_URI_LENGTH = 2048,       -- 2KB URI length
    REQUEST_TIMEOUT = 30         -- 30 seconds request timeout
}

-- Rate limiting storage (simple in-memory)
local rate_limit_storage = {}
local rate_limit_cleanup_time = 0

local function cleanup_rate_limit_storage()
    local now = os.time()
    if now - rate_limit_cleanup_time > 60 then -- Cleanup every minute
        local cutoff = now - 300 -- Keep 5 minutes of data
        for ip, data in pairs(rate_limit_storage) do
            if data.last_request < cutoff then
                rate_limit_storage[ip] = nil
            end
        end
        rate_limit_cleanup_time = now
    end
end

function context_mt:check_rate_limit()
    if not config.enable_rate_limiting then
        return true
    end

    cleanup_rate_limit_storage()

    local client_ip = self:remoteip()
    local now = os.time()
    local max_requests = config.rate_limit_requests or 100
    local window_seconds = config.rate_limit_window or 60

    local client_data = rate_limit_storage[client_ip]
    if not client_data then
        rate_limit_storage[client_ip] = {
            requests = 1,
            first_request = now,
            last_request = now
        }
        return true
    end

    -- Reset window if expired
    if now - client_data.first_request >= window_seconds then
        client_data.requests = 1
        client_data.first_request = now
        client_data.last_request = now
        return true
    end

    client_data.requests = client_data.requests + 1
    client_data.last_request = now

    if client_data.requests > max_requests then
        self._http_error = {
            code = 429,
            message = "Too Many Requests",
            details = "Rate limit exceeded"
        }
        return false
    end

    return true
end

-- Security validation middleware
local function security_middleware(ctx, next)
    -- Rate limiting check
    if not ctx:check_rate_limit() then
        return -- Error already set in context
    end

    -- Request size validation (already handled in content_length parsing)
    -- Additional security headers
    ctx:addheader("X-Content-Type-Options", "nosniff")
    ctx:addheader("X-Frame-Options", "DENY")
    ctx:addheader("X-XSS-Protection", "1; mode=block")

    if config.security_headers then
        for header, value in pairs(config.security_headers) do
            ctx:addheader(header, value)
        end
    end

    next()
end

-- Performance Monitoring
local performance_stats = {
    requests_total = 0,
    requests_by_method = {},
    requests_by_status = {},
    response_times = {},
    errors_total = 0,
    bytes_sent = 0,
    start_time = os.time()
}

local function update_performance_stats(ctx, status_code, response_time, bytes_sent)
    performance_stats.requests_total = performance_stats.requests_total + 1

    -- Track by method
    local method = ctx.method or "UNKNOWN"
    performance_stats.requests_by_method[method] = (performance_stats.requests_by_method[method] or 0) + 1

    -- Track by status code
    performance_stats.requests_by_status[status_code] = (performance_stats.requests_by_status[status_code] or 0) + 1

    -- Track response times (simple moving average of last 100 requests)
    if response_time then
        table.insert(performance_stats.response_times, response_time)
        if #performance_stats.response_times > 100 then
            table.remove(performance_stats.response_times, 1)
        end
    end

    -- Track errors
    if status_code >= 400 then
        performance_stats.errors_total = performance_stats.errors_total + 1
    end

    -- Track bytes sent
    if bytes_sent then
        performance_stats.bytes_sent = performance_stats.bytes_sent + bytes_sent
    end
end

function context_mt:get_performance_stats()
    local uptime = os.time() - performance_stats.start_time
    local avg_response_time = 0

    if #performance_stats.response_times > 0 then
        local total = 0
        for _, time in ipairs(performance_stats.response_times) do
            total = total + time
        end
        avg_response_time = total / #performance_stats.response_times
    end

    return {
        uptime_seconds = uptime,
        requests_total = performance_stats.requests_total,
        requests_per_second = uptime > 0 and (performance_stats.requests_total / uptime) or 0,
        requests_by_method = performance_stats.requests_by_method,
        requests_by_status = performance_stats.requests_by_status,
        average_response_time_ms = avg_response_time,
        errors_total = performance_stats.errors_total,
        errors_rate = performance_stats.requests_total > 0 and (performance_stats.errors_total / performance_stats.requests_total) or 0,
        bytes_sent_total = performance_stats.bytes_sent
    }
end

-- Performance monitoring middleware
local function performance_middleware(ctx, next)
    local start_time = fan.gettime and fan.gettime() or (os.clock() * 1000)

    -- Store original reply function to intercept response
    local original_reply = ctx.reply
    ctx.reply = function(self, code, message, body)
        local end_time = fan.gettime and fan.gettime() or (os.clock() * 1000)
        local response_time = end_time - start_time
        local bytes_sent = body and #body or 0

        update_performance_stats(ctx, code, response_time, bytes_sent)

        return original_reply(self, code, message, body)
    end

    next()
end

local function context_index_body(ctx)
    local total = ctx.content_length
    if total == 0 then
        return ""
    end

    local t = {}
    while true do
        local buff = ctx:read()
        if buff then
            table.insert(t, buff)
        else
            break
        end
    end

    if ctx:available() > 0 then
        ctx.broken_body = table.concat(t)
        return nil
    end

    return table.concat(t)
end

local function context_index_remoteinfo(ctx)
    return ctx.apt.conn:remoteinfo()
end

local function context_index_remoteip(ctx)
    return context_index_remoteinfo(ctx).ip
end

local function context_index_remoteport(ctx)
    return context_index_remoteinfo(ctx).port
end

local function context_index_params_unpack_kv(t, kv)
    local c, d = string.find(kv, "=", 1, true)
    if c and d then
        t[http.unescape(string.sub(kv, 1, c - 1))] = http.unescape(string.sub(kv, d + 1))
    else
        t[http.unescape(kv)] = ""
    end
end

local function context_index_params(ctx)
    local t = {}

    local str = ctx.query or ""

    if ctx.headers["content-type"] == "application/x-www-form-urlencoded" then
        str = str .. "&" .. ctx.body
    end

    local offset = 1

    while true do
        local a, b = string.find(str, "&", offset, true)
        if a and b then
            local kv = string.sub(str, offset, a - 1)
            context_index_params_unpack_kv(t, kv)
            offset = b + 1
        else
            local kv = string.sub(str, offset)
            context_index_params_unpack_kv(t, kv)
            break
        end
    end

    return t
end

local function context_index_content_length(ctx)
    local content_length = ctx.headers["content-length"]
    if content_length and type(content_length) == "string" then
        -- RFC 7230: Content-Length = 1*DIGIT
        -- Must be exactly digits, no leading zeros except "0" itself
        local num = content_length:match("^(0|[1-9][0-9]*)$")
        if num then
            local len = tonumber(num)
            if len >= 0 then
                -- Check against configured limit (default 10MB if not set)
                local max_limit = config.max_content_length or 10485760
                if len > max_limit then
                    -- RFC 7231: 413 Content Too Large
                    ctx._http_error = {code = 413, message = "Content Too Large"}
                    return 0
                end
                return len
            end
        end
        -- RFC 7230: Invalid Content-Length format should result in 400 Bad Request
        ctx._http_error = {code = 400, message = "Bad Request", details = "Invalid Content-Length header"}
        return 0
    else
        return 0
    end
end

local context_mt_index_map = {
    ["body"] = context_index_body,
    ["remoteip"] = context_index_remoteip,
    ["remoteport"] = context_index_remoteport,
    ["remoteinfo"] = context_index_remoteinfo,
    ["params"] = context_index_params,
    ["content_length"] = context_index_content_length
}

context_mt.__index = function(ctx, key)
    if context_mt[key] then
        return context_mt[key]
    elseif context_mt_index_map[key] then
        local v = context_mt_index_map[key](ctx)
        ctx[key] = v
        return v
    end
end

local headers_mt = {}
headers_mt.__index = function(headers, key)
    local lk = key:lower()
    local obj = rawget(headers, lk)
    headers[key] = obj
    return obj
end

local function onaccept(apt, onservice)
    local context

    while true do
        if not context or context.reply_status == ResponseState.COMPLETED then
            -- recreate context if previous has been replied.
            context = {
                headers = {},
                out_headers = {},
                apt = apt,
                content_type = "text/plain; charset=utf-8",
                read_offset = 0
            }
            setmetatable(context.headers, headers_mt)
            setmetatable(context, context_mt)
        end

        local input = apt:receive()
        if not input then
            break
        end

        if not context.header_complete then
            local status, expect = readheader(context, input)
            if not status then
                if expect then
                    input = apt:receive(expect)
                else
                    apt:close()
                    return
                end
            end
        end

        if context.header_complete then
            -- Check for HTTP protocol violations before calling service
            if context._http_error then
                local err = context._http_error
                print(string.format("HTTP Protocol Error: %d %s", err.code, err.message))
                context:reply(err.code, err.message, err.details or "")
                if tonumber(context.version) < 1.1 then
                    apt:close()
                end
                context._http_error = nil -- Clear error
                goto continue
            end

            local st, msg = pcall(onservice, context, context)

            if not st then
                -- Generate error ID for tracking
                local error_id = string.format("ERR_%d_%s", os.time(),
                                             string.sub(tostring(msg):gsub("[^%w]", ""), 1, 8))
                print(string.format("[%s] Service Exception: %s", error_id, msg))

                if not context.reply_status then
                    -- Return generic error message, not internal details
                    context:reply(500, "Internal Server Error",
                                 string.format("Error ID: %s", error_id))
                elseif context.reply_status == ResponseState.STARTED then
                    context:reply_chunk(string.format("Error ID: %s", error_id))
                    context:reply_end()
                end

                -- Give client time to receive response before closing
                fan.sleep(0.1)
                apt:close()
                break
            end

            -- Use Keep-Alive logic instead of version check
            if not context:should_keep_alive() then
                apt:close()
                break
            end

            ::continue::
        end
    end
end

local function bind(arg)
    local onservice = arg and arg.onService
    local host = arg and arg.host or "0.0.0.0"
    local port = arg and arg.port or 0

    -- Use connector.bind to create the server
    local serv_result = connector.bind(string.format("tcp://%s:%d", host, port))

    -- Create the object with actual bound information
    local obj = {
        host = serv_result.host or host,
        port = serv_result.port or port,
        serv = serv_result,
        router = Router.new() -- Add router instance
    }

    -- Add default security and performance middleware
    if config.enable_security_middleware ~= false then
        obj.router:use(security_middleware)
    end

    if config.enable_performance_monitoring ~= false then
        obj.router:use(performance_middleware)
    end

    -- Add router methods to server object
    obj.get = function(self, path, handler) self.router:get(path, handler) end
    obj.post = function(self, path, handler) self.router:post(path, handler) end
    obj.put = function(self, path, handler) self.router:put(path, handler) end
    obj.delete = function(self, path, handler) self.router:delete(path, handler) end
    obj.use = function(self, middleware) self.router:use(middleware) end
    obj.set_not_found = function(self, handler) self.router.not_found_handler = handler end

    -- Add performance stats endpoint
    obj.get_stats = function(self) return context_mt.get_performance_stats() end

    -- Set up the onaccept handler
    obj.serv.onaccept = function(apt)
        local service_handler = onservice or function(ctx)
            -- Use router if no custom onservice provided
            obj.router:handle(ctx)
        end
        onaccept(apt, service_handler)
    end

    return obj
end

return {
    bind = bind,
    Router = Router,
    ResponseState = ResponseState,
    SecurityLimits = SecurityLimits,
    security_middleware = security_middleware,
    performance_middleware = performance_middleware
}
