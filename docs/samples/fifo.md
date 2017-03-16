FIFO Sample
===========

* run lua script

```lua
local fan = require "fan"
local fifo = require "fan.fifo"

p1 = fifo.connect{
    name = "fan.fifo",
    rwmode = "r",
    onread = function(buf)
        print("onread", #(buf), buf)
    end
}
fan.loop()
```

* execute shell

`shell# echo test > fan.fifo`
