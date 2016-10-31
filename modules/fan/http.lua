local tcpd = require "fan.tcpd"

local function request(method, args)
  if type(args) == "string" then
    args = {url = args}
  end
  local _,_,host,port,path = string.find(args.url, "[^:]+://([^:]+):(%d+)(.*)")
  if not host then
    _,_,host,path = string.find(args.url, "[^:]+://([^:/]+)(.*)")
    port = 80
  end

  local ret = {responseCode = 0}

  local cache = nil

  local version
  local code
  local message
  local headers = {}

  local first_line_completed = false
  local header_complete = false
  local accepted = false
  local disconnected = false

  local content_length

  local conn
  local conn_connected = false

  local readline = function()
    if cache then
      local st,ed = string.find(cache, "\r\n", 1, true)
      if not st or not ed then
        st,ed = string.find(cache, "\n", 1, true)
      end
      if st and ed then
        data = string.sub(cache, 1, st - 1)
        if #(cache) > ed then
          cache = string.sub(cache, ed + 1)
        else
          cache = nil
        end
        return data
      end
    end
  end

  local readheader = function()
    while not header_complete do
      local line = readline()
      if not line then
        break
      else
        if #(line) == 0 then
          header_complete = true
        else
          if first_line_completed then
            local k,v = string.match(line, "([^:]+):[ ]*(.*)")
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
            version,code,message = string.match(line, "HTTP/([0-9.]+) (%d+) (.+)")
            first_line_completed = true
            ret.responseCode = code
            ret.responseMessage = message
          end
        end
      end
    end
  end

  local running = coroutine.running()

  local conn
  conn = tcpd.connect{
    host = host,
    port = tonumber(port),
    onconnected = function()
      local t = {
        string.format("%s %s HTTP/1.0", method:upper(), path),
      }
      if args.headers then
        for k,v in pairs(args.headers) do
          table.insert(t, string.format("%s: %s", k, v))
        end
      end
      table.insert(t, "")
      table.insert(t, "")

      conn:send(table.concat(t, "\r\n"))
    end,
    onread = function(buf)
      cache = cache and (cache .. buf) or buf

      if not header_complete then
        readheader()
      end

      if header_complete then
        if cache and content_length and #(cache) >= content_length then
          conn:close()
          ret.body = string.sub(cache, 1, content_length)
          if running then
            coroutine.resume(running)
          end
        end
      end
    end,
    ondisconnected = function(msg)
      if not ret.body then
        ret.error = msg
      end

      if running then
        coroutine.resume(running)
      end
    end
  }

  coroutine.yield()
  running = nil
  return ret
end

return {
  get = function(args)
    return request("GET", args)
  end
}
