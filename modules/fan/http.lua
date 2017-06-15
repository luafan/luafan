local tcpd = require "fan.tcpd"
local connector = require "fan.connector"

local string = string
local table = table
local type = type
local coroutine = coroutine
local tonumber = tonumber

local function request(method, args)
    if type(args) == "string" then
        args = {url = args}
    end
    local _, _, host, port, path = string.find(args.url, "[^:]+://([^:]+):(%d+)(.*)")
    if not host then
        _, _, host, path = string.find(args.url, "[^:]+://([^:/]+)(.*)")
        port = 80
    end
    
    local headers = {}
    local ret = {responseCode = 0, headers = headers}
    
    local version
    local code
    local message
    
    local first_line_completed = false
    local header_complete = false
    local accepted = false
    local disconnected = false
    
    local content_length = 0
    
    local conn
    local conn_connected = false

    conn = connector.connect(string.format("tcp://%s:%d", host, port))
    
    local t = {string.format("%s %s HTTP/1.0\r\n", method:upper(), path)}
    if args.headers then
        for k, v in pairs(args.headers) do
            table.insert(t, string.format("%s: %s\r\n", k, v))
        end
    end
    table.insert(t, "\r\n")
    
    conn:send(table.concat(t))
    
    local readheader = function(input)
        while not header_complete do
            local line = input:readline()
            if not line then
                break
            else
                if #(line) == 0 then
                    header_complete = true
                else
                    if first_line_completed then
                        local k, v = string.match(line, "([^:]+):[ ]*(.*)")
                        k = string.lower(k)
                        if k == "content-length" then
                            content_length = tonumber(v)
                        end
                        local old = headers[k]
                        if old then
                            if type(old) == "table" then
                                table.insert(old, v)
                            else
                                headers[k] = {old, v}
                            end
                        else
                            headers[k] = v
                        end
                    else
                        version, code, message = string.match(line, "HTTP/([0-9.]+) (%d+) (.+)")
                        first_line_completed = true
                        ret.responseCode = code
                        ret.responseMessage = message
                    end
                end
            end
        end
    end
    
    local input
    while not header_complete do
        input = conn:receive()
        readheader(input)
    end
    
    if header_complete then
        local t = {}
        local count = 0

        repeat
            local buff = input:GetBytes()
            if buff then
              table.insert(t, buff)
              count = count + #buff
            end

            input = conn:receive()
        until count >= content_length or not input
        
        if count == content_length then
            ret.body = table.concat(t)
        else
            ret.error = conn.disconnected_message
        end
    else
        ret.error = conn.disconnected_message
    end
    
    return ret
end

return {get = function(args)
        return request("GET", args)
    end}
