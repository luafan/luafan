--[[
speed result:
bit > ffi > core
]]
local config = require "config"
local support_zero_string = string.len(string.format("%c", 0)) == 1

if not jit
  or not support_zero_string
  or (not config.stream_ffi and not config.stream_bit)
  then
  if config.debug then
    print("use stream_core")
  end
  return require "fan.stream.core"
else
  local config = require "config"
  if config.stream_ffi then
    if config.debug then
      print("use stream_ffi")
    end
    return require "fan.stream.ffi"
  else
    if config.debug then
      print("use stream_bit")
    end
    return require "fan.stream.bit"
  end
end
