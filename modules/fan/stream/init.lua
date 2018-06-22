--[[
speed result:
bit > ffi > core
]]
local config = require "config"
local support_zero_string = string.len(string.format("%c", 0)) == 1

local stream

if not jit or not support_zero_string or (not config.stream_ffi and not config.stream_bit) then
    if config.debug then
        print("use stream_core")
    end
    stream = require "fan.stream.core"
else
    local config = require "config"
    if config.stream_ffi then
        if config.debug then
            print("use stream_ffi")
        end
        stream = require "fan.stream.ffi"
    else
        if config.debug then
            print("use stream_bit")
        end
        stream = require "fan.stream.bit"
    end
end

local test = stream.new()
local mt = getmetatable(test)

function mt:readline()
    if self:available() > 0 then
        local breakflag
        local t = {}
        while true do
            local b = self:GetBytes(1)
            if not b then
                break
            elseif b == "\r" then
                self:mark()
                if self:GetBytes(1) ~= "\n" then
                    self:reset()
                    breakflag = "\r"
                else
                    breakflag = "\r\n"
                end
                break
            elseif b == "\n" then
                breakflag = "\n"
                break
            else
                table.insert(t, b)
            end
        end

        return table.concat(t), breakflag
    end
end

return stream
