fan
===

### `fan.loop(startup:function?)`
start the event loop, can pass startup function or nothing.

### `fan.loopbreak()`
break the event loop, make sure call this api after all the daemon service(fifo, tcpd.bind, udpd.bind, httpd.bind) has been garbage collected.

### `fan.sleep(sec:number)`
sleep for any seconds, e.g. 0.1 or 10

### `fan.data2hex(data:string)`
convert binary data to hex string.

### `fan.hex2data(data:string)`
convert hex string to binary data.

### `fan.gettime()`
return 2 integer values, sec, usec
