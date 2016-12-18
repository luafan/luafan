# luafan
* [Quick Guide](guide.md)
* [Setup](setup.md)
* [Sample](sample.md)

## Non-blocking Modules
* [fan](api/fan.md) common module.
* [fan.tcpd](api/tcpd.md) tcp protocol module.
* [fan.udpd](api/udpd.md) udp protocol module.
* [fan.fifo](api/fifo.md) fifo pipe module.
* [fan.httpd](api/httpd.md) httpd webserver module.
* [fan.http](api/http.md) http request module.
* [fan.mariadb](api/mariadb.md) mariadb client module.
* [fan.stream](api/stream.md) stream helper.

Introduce
=========

There are 3 spec in luarocks, luafan/luafanlite/luafanmicro

* `luafan` contains `fifo` `tcpd` `udpd` `httpd` `stream` `http` `mariadb` depends `libevent curl openssl mariadb`

* `luafanlite` contains `fifo` `tcpd` `udpd` `httpd` `stream` `http` depends `libevent curl openssl`

* `luafanmicro` contains `fifo` `tcpd` `udpd` `httpd` `stream` depends `libevent`
