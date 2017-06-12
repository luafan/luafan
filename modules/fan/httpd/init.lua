local fan = require "fan"
local connector = require "fan.connector"

local function readheader(ctx, input)
    while not ctx.header_complete do
        local line = input:readline()
        if not line then
            break
        else
            if #(line) == 0 then
                ctx.header_complete = true
            else
                if ctx.first_line then
                    local k, v = string.match(line, "([^:]+):[ ]*(.*)")
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
                    local a,b = string.find(ctx.path, "?", 1, true)
                    if a and b then
                        ctx.query = string.sub(ctx.path, b + 1)
                        ctx.path = string.sub(ctx.path, 1, a - 1)
                    end
                    ctx.first_line = true
                end
            end
        end
    end
end

local context_mt = {}

function context_mt:reply(code, message, body)
    self.replied = true

    local t = {}
    table.insert(t, string.format("HTTP/1.0 %d %s", code, message))
    table.insert(t, string.format("Content-Length: %d", body and #body or 0))
    table.insert(t, "Connection: keep-alive")
    table.insert(t, "")
    table.insert(t, body)
    self.apt:send(table.concat(t, "\r\n"))
end

local function context_index_body(ctx)
    if not ctx.content_length then
        return nil
    end

    local t = {}
    local count = 0
    while count < ctx.content_length do
        local input = ctx.apt:receive()
        if input then
            local buff = input:GetBytes()
            count = count + #buff
            table.insert(t, buff)
        else
            break
        end
    end

    if count < ctx.content_length then
        ctx.broken_body = table.concat(t)
        return nil
    end

    ctx.body = table.concat(t)
    return ctx.body
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

local context_mt_index_map = {
    ["body"] = context_index_body,
    ["remoteip"] = context_index_remoteip,
    ["remoteport"] = context_index_remoteport,
    ["remoteinfo"] = context_index_remoteinfo,
}

context_mt.__index = function(ctx, key)
    if context_mt[key] then
        return context_mt[key]
    elseif context_mt_index_map[key] then
        return context_mt_index_map[key](ctx)
    end
end

local function onaccept(apt, onservice)
    local context

    while true do
        if not context or context.replied then
            -- recreate context if previous has been replied.
            context = {headers = {}, apt = apt}
            setmetatable(context, context_mt)
        end

        local input = apt:receive()
        if not input then
            break
        end

        if not context.header_complete then
            readheader(context, input)
        end

        if context.header_complete then
            for k,v in pairs(context.headers) do
                if k == "content-length" then
                    context.content_length = tonumber(v)
                end
            end
            local st,msg = pcall(onservice, context, context)

            if not st then
                print(msg)
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

return {
    bind = bind
}