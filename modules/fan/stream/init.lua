--[[
speed result:
bit > ffi > core
]]
if not jit then
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
