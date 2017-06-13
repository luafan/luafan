local config = require "config"

if config.httpd_using_core then
    if config.debug then
        print("use httpd_core")
    end
    return require "fan.httpd.core"
else
    if config.debug then
        print("use httpd_lua")
    end
    return require "fan.httpd.httpd"
end
