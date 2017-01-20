if not jit then
  return require "fan.stream.core"
else
  local config = require "config"
  if config.stream_ffi then
    print("use stream_ffi")
    return require "fan.stream.ffi"
  else
    print("use stream_bit")
    return require "fan.stream.bit"
  end
end
