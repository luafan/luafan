Quick Guide
===========

1. complie latest libevent or install libevent-dev in your machine, e.g. for ubuntu, `sudo apt-get install libevent-dev`

2. `luarocks install luafanmicro`

3. save sample lua as "hello.lua", then run with `luajit hello.lua` or  `lua5.3 hello.lua` (module from luarocks only support luajit or lua5.2+)

```
local fan = require "fan"

local function main(time)
    while true do
        fan.sleep(time)
        print(time, os.time())
    end
end

main_co1 = coroutine.create(main)
print(coroutine.resume(main_co1, 2))
main_co2 = coroutine.create(main)
print(coroutine.resume(main_co2, 3))

fan.loop()
```
