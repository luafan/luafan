local fan = require "fan"
local http = require "fan.http"
local udpd = require "fan.udpd"

local upnp_mt = {}
upnp_mt.__index = upnp_mt

local WANIPConnectionType = "urn:schemas-upnp-org:service:WANIPConnection:1"

local template = [[<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <s:Body>
    <u:AddPortMapping xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">
      <NewRemoteHost></NewRemoteHost>
      <NewExternalPort>%s</NewExternalPort>
      <NewProtocol>%s</NewProtocol>
      <NewInternalPort>%s</NewInternalPort>
      <NewInternalClient>%s</NewInternalClient>
      <NewEnabled>1</NewEnabled>
      <NewPortMappingDescription>%s</NewPortMappingDescription>
      <NewLeaseDuration>0</NewLeaseDuration>
    </u:AddPortMapping>
  </s:Body>
</s:Envelope>]]

function upnp_mt:AddPortMapping(ip, port, extport, protocol, description)
  assert(ip and port and extport and protocol)
  if not description then
    description = "fan.upnp"
  end
  for i, v in ipairs(self.devices) do
    if v.st == WANIPConnectionType then
      local ret =
        http.post {
        url = v.url,
        -- verbose = 1,
        headers = {
          ["Content-Type"] = "text/xml",
          ["SOAPAction"] = [["urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping"]]
        },
        body = string.format(template, extport, protocol:upper(), port, ip, description)
      }

      return ret.responseCode == 200, ret.body
    end
  end
end

local function timeout(task, timeout_sec)
  fan.sleep(timeout_sec)
  if task.running then
    coroutine.resume(task.running)
  end
end

local function new(timeout_sec)
  local task = {running = coroutine.running(), responses = {}}

  local cli =
    udpd.new {
    host = "239.255.255.250",
    port = 1900,
    onread = function(buf)
      if task.running then
        table.insert(task.responses, buf)
      end
    end
  }

  local t = {
    "M-SEARCH * HTTP/1.1",
    "Host:239.255.255.250:1900",
    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    -- "ST: ssdp:all",
    'Man: "ssdp:discover"',
    string.format("MX:%d", timeout_sec),
    "",
    ""
  }
  cli:send(table.concat(t, "\r\n"))
  -- cli:send(table.concat(t, "\n"))

  coroutine.wrap(timeout)(task, timeout_sec)

  coroutine.yield()
  cli:close()
  cli = nil
  task.running = nil

  local obj = {devices = {}}
  local file_cache = {}
  for i, v in ipairs(task.responses) do
    local _, _, location = string.find(v, "LOCATION: ([^\r\n]+)")
    local _, _, st = string.find(v, "ST: ([^\r\n]+)")
    local _, _, server = string.find(v, "SERVER: ([^\r\n]+)")
    if location and st then
      local ret = file_cache[location]
      if not ret then
        ret = http.get(location)
        file_cache[location] = ret
      end

      if ret.responseCode == 200 and not ret.error and ret.body then
        local _, ed = string.find(ret.body, "<serviceType>" .. WANIPConnectionType .. "</serviceType>", 1, true)
        if ed then
          local _, _, path = string.find(ret.body, "<controlURL>([^><]+)</controlURL>", ed)
          local _, _, url = string.find(location, "([^:]+:[^:]+:%d+)")
          if url and path then
            table.insert(obj.devices, {st = WANIPConnectionType, server = server, url = url .. path})
          end
        end
      end
    end
  end

  setmetatable(obj, upnp_mt)
  return obj
end

return {
  new = new
}
