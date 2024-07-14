local config = require "config"

if not config.loglevel then
    config.loglevel = 1
end

local ERROR = 1
local WARN = 2
local INFO = 3
local DEBUG = 4
local TRACE = 5

local m = {adapter = print}

m.isTrace = function(category)
    return config.loglevel >= TRACE and (not category or config[category])
end

m.isDebug = function(category)
    return config.loglevel >= DEBUG and (not category or config[category])
end

m.isInfo = function(category)
    return config.loglevel >= INFO and (not category or config[category])
end

m.isWarn = function(category)
    return config.loglevel >= WARN and (not category or config[category])
end

m.isError = function(category)
    return config.loglevel >= ERROR and (not category or config[category])
end

m.trace = function(...)
    if m.isTrace() then
        m.adapter("[TRACE]", ...)
    end
end

m.debug = function(...)
    if m.isDebug() then
        m.adapter("[DEBUG]", ...)
    end
end

m.info = function(...)
    if m.isInfo() then
        m.adapter("[INFO]", ...)
    end
end

m.warn = function(...)
    if m.isWarn() then
        m.adapter("[WARN]", ...)
    end
end

m.error = function(...)
    if m.isError() then
        m.adapter("[ERROR]", ...)
    end
end

return m
