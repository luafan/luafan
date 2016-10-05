local fan = require "fan"

local scheme_map = {}
scheme_map["tcp"] = require "fan.connector.tcp"
scheme_map["udp"] = require "fan.connector.udp"
scheme_map["fifo"] = require "fan.connector.fifo"

local function extract_url(url)
  if not url then
    return
  end

  local _,_,scheme,twoslash,others = string.find(url, "^(%w+):([/]?[/]?)(.*)")
  local path,server,host,port
  if twoslash and #(twoslash) > 0 then
    _,_,server,path = string.find(others, "([^/]*)(.*)")
    if #(server) > 0 then
      _,_,host,port = string.find(server, "^([^:]+):(%d+)$")
    end
  else
    path = others
  end

  return scheme, host, port, path
end

local function connect(url)
  local scheme, host, port, path = extract_url(url)
  if not scheme then
    return
  end

  local connector = scheme_map[scheme:lower()]
  if not connector then
    return
  end

  return connector.connect(host, port, path)
end

local function bind(url)
  local scheme, host, port, path = extract_url(url)
  if not scheme then
    return
  end

  local connector = scheme_map[scheme:lower()]
  if not connector then
    return
  end

  return connector.bind(host, port, path)
end

return {
  connect = connect,
  bind = bind,
}
