local fan = require "fan"
local connector = require "fan.connector"
local http = require "fan.http"
local zlib = require "zlib"

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
            else
                if ctx.first_line then
                    local k, v = string.match(line, "([^:]+):[ ]*(.*)")
                    if not k or not v then
                        return false
                    end

                    k = string.lower(k)
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
                else
                    ctx.method, ctx.path, ctx.version = string.match(line, "([A-Z]+) ([^ ]+) HTTP/([0-9.]+)")
                    if not ctx.method or not ctx.path or not ctx.version then
                        return false
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
    table.insert(t, string.format("HTTP/1.1 %d %s\r\n", code, message))

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

function context_mt:reply_start(code, message)
    if self.reply_status then
        return
    end

    self:_ready_for_reply()

    self.reply_status = "start"

    local t = {}
    context_reply_fillheader(t, self, code, message)

    table.insert(t, "Transfer-Encoding: chunked\r\n")
    table.insert(t, "\r\n")

    if self._gzip_body then
        local apt = self.apt
        self._gzip_outputstream =
            zlib.deflate(
            {
                write = function(self, data)
                    coroutine.wrap(
                        function()
                            apt:send(string.format("%X\r\n%s\r\n", #data, data))
                        end
                    )()
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
    if self.reply_status ~= "start" then
        return
    end
    self.reply_status = "end"

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
    self.reply_status = "end"

    self:_ready_for_reply()

    local t = {}
    if not body or #(body) == 0 then
        self._no_gzip = true
    end

    context_reply_fillheader(t, self, code, message)

    if body and self._gzip_body then
        body = zlib.compress(body, nil, nil, 31)
    end

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
    if content_length and type(content_length) == "string" and content_length:match("[0-9]+") then
        return tonumber(content_length)
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
        if not context or context.reply_status == "end" then
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
            local st, msg = pcall(onservice, context, context)

            if not st then
                print("Exception", msg)

                if not context.reply_status then
                    context:reply(500, "Exception", msg)
                elseif context.reply_status == "start" then
                    context:reply_chunk(msg)
                    context:reply_end()
                end

                apt:close()
                break
            end

            if tonumber(context.version) < 1.1 then
                apt:close()
            end
        end
    end
end

local function bind(arg)
    local onservice = arg and arg.onService
    local obj = {host = arg and arg.host, port = arg and arg.port}
    obj.serv = connector.bind(string.format("tcp://%s:%d", obj.host or "0.0.0.0", obj.port or 0))
    obj.serv.onaccept = function(apt)
        onaccept(apt, onservice)
    end

    return obj
end

return {bind = bind}
