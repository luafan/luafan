APIs
====

## Goals
Connector is used to create tcp/udp/fifo connection quickly with same interface. It simulate block api call for tcp/fifo, so it's easy for you to write functions without callback hell.

* `cli = connector.connect(url)`

 create a logic connection to fifo/tcp/udp

* `serv = connector.bind(url)`

bind a listener of fifo/tcp/udp

* `filename = connector.tmpfifoname()`

create a temporary fifo file that will not conflict with others.

URL_Format
==========

* `fifo:<file path>`
* `tcp://host:port`
* `udp://host:port`

CLI_APIs
========

* `cli:send(buf)` (fifo/tcp)

yield until buf sent, if `#buf` is too big, it will be divided to parts (fifo `MAX_LINE_SIZE = 8192`).
 
* `stream = cli:receive(expect_length?)` (fifo/tcp)

yield to wait for expect data ready for read, return the read stream ([fan.stream](stream.md)) on read ready, the default expect_length is 1.

* `cli:close()`(fifo/tcp)

close this connection.

* `cli:send(buf)` (udp with embedded private protocol)

send buf on background, using embedded private protocol to control squence, buf size limit around 65536 * (MTU payload len), support 33.75MB (65536 * (576 - 8 - 20 - 8)) over internet at last.

* `cli.onread = function(buf) end` (udp with embedded private protocol)

input buffer callback, using embedded private protocol to control squence, when all parts of the buffer received, this callback will be invoked.

SERV
====

* `serv.onaccept = function(cli) end`