local fan = require "fan"
require "compat53"

local scheme_map = {}
scheme_map["tcp"] = require "fan.connector.tcp"
scheme_map["udp"] = require "fan.connector.udp"
scheme_map["fifo"] = require "fan.connector.fifo"

local function extract_url(url)
  if not url then
    return
  end

  local _, _, scheme, twoslash, others = string.find(url, "^(%w+):([/]?[/]?)(.*)")
  local path, server, host, port
  if twoslash and #(twoslash) == 2 then
    _, _, server, path = string.find(others, "([^/]*)(.*)")
    if #(server) > 0 then
      _, _, host, port = string.find(server, "^([^:]+):(%d+)$")
    end
  else
    path = twoslash .. others
  end

  return scheme, host, port, path
end

local function connect(url, args)
  local scheme, host, port, path = extract_url(url)
  if not scheme then
    return
  end

  local connector = scheme_map[scheme:lower()]
  if not connector then
    return
  end

  return connector.connect(host, port, path, args)
end

local function bind(url, args)
  local scheme, host, port, path = extract_url(url)
  if not scheme then
    return
  end

  local connector = scheme_map[scheme:lower()]
  if not connector then
    return
  end

  return connector.bind(host, port, path, args)
end

local function tmpfifoname()
  local fifoname = os.tmpname()
  print(fifoname)
  os.remove(fifoname)

  return fifoname
end

return {
  connect = connect,
  bind = bind,
  tmpfifoname = tmpfifoname
}
