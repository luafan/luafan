local config = require "config"

if config.http_using_core then
    if config.debug then
        print("use http_core")
    end
    return require "fan.http.core"
else
    if config.debug then
        print("use http_lua")
    end
    return require "fan.http.http"
end
