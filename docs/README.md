[![CircleCI](https://circleci.com/gh/luafan/luafan.svg?style=svg)](https://circleci.com/gh/luafan/luafan)
[GitHub](https://github.com/luafan/luafan)

# LUAFAN (Powered by: LUAFAN LAB LTD)

luafan is a non-blocking/single-process module collection to lua, full supported lua coroutine.
luafan simulated blocking code style over non-blocking implementation.
It's easy to write several api invocations one by one without any callback.

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
* [fan.objectbuf](api/objectbuf.md) serialize helper.
* [fan.connector](api/connector.md) fifo/tcp/udp connector helper.
* [fan.worker](api/worker.md) multi-process worker helper.

## Utility Modules
* [fan.utils](api/utils.md) utility functions for strings, time, and weak references.
* [fan.pool](api/pool.md) generic resource pooling system.
* [fan.upnp](api/upnp.md) UPnP client for automatic port mapping.

## ORM
* [mariadb.orm](api/mariadb_orm.md) mariadb orm impl.
* [sqlite3.orm](api/sqlite3_orm.md) sqlite3 orm impl.

## POOL
* [mariadb.pool](api/mariadb_pool.md) mariadb simple connection pool.

## Config
* [config](api/config.md) common config.

Package Variants
================

There are 3 package variants available in LuaRocks:

## luafan (Full Package)
**Modules:** `fifo` `tcpd` `udpd` `httpd` `stream` `objectbuf` `http` `mariadb` `connector` `worker` `utils` `pool` `upnp`
**Dependencies:** `libevent` `curl` `openssl` `mariadb`
**Use case:** Complete async networking and database development

## luafanlite (Lite Package)
**Modules:** `fifo` `tcpd` `udpd` `httpd` `stream` `objectbuf` `http` `connector` `worker` `utils` `pool` `upnp`
**Dependencies:** `libevent` `curl` `openssl`
**Use case:** Async networking without database requirements

## luafanmicro (Micro Package)
**Modules:** `fifo` `tcpd` `udpd` `httpd` `stream` `objectbuf` `connector` `worker` `utils` `pool`
**Dependencies:** `libevent`
**Use case:** Minimal async networking (no SSL/HTTP client/UPnP)
