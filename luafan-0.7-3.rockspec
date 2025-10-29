package = "luafan"
version = "0.7-3"
source = {
   url = "git://github.com/luafan/luafan",
   tag = "v0.7.3"
}

description = {
   summary = "A single process/thread library, with async apis for Lua.",
   detailed = [[
      full coroutine apis: http/httpd tcp/tcpd udp/udpd mariadb fifo.
   ]],
   homepage = "https://github.com/luafan/luafan",
   license = "MIT/X11"
}

dependencies = {
   "lua >= 5.1"
}

external_dependencies = {
   MARIADB = {
      header = "mysql/mysql.h"
   },
   OPENSSL = {
      header = "openssl/opensslv.h"
   },
   LIBEVENT = {
      header = "event2/event.h"
   },
   CURL = {
      header = "curl/curl.h"
   }
}

build = {
   type = "builtin",
   modules = {
      fan = {
         sources = {
            "src/utlua.c",
            "src/bytearray.c",
            "src/event_mgr.c",
            "src/luafan.c",
            "src/luafan_posix.c",
            "src/tcpd.c",
            "src/tcpd_config.c",
            "src/tcpd_event.c",
            "src/tcpd_error.c",
            "src/tcpd_ssl.c",
            "src/tcpd_server.c",
            "src/udpd.c",
            "src/udpd_config.c",
            "src/udpd_event.c",
            "src/udpd_dns.c",
            "src/udpd_dest.c",
            "src/udpd_utils.c",
            "src/evdns.c",
            "src/stream.c",
            "src/stream_ffi.c",
            "src/objectbuf.c",
            "src/fifo.c",
            "src/http.c",
            "src/httpd.c",
            "src/luasql.c",
            "src/luamariadb.c",
            -- MariaDB module sources
            "src/mariadb/luamariadb_connect.c",
            "src/mariadb/luamariadb_close.c",
            "src/mariadb/luamariadb_query.c",
            "src/mariadb/luamariadb_prepare.c",
            "src/mariadb/luamariadb_ping.c",
            "src/mariadb/luamariadb_commit.c",
            "src/mariadb/luamariadb_rollback.c",
            "src/mariadb/luamariadb_autocommit.c",
            "src/mariadb/luamariadb_setcharset.c",
            "src/mariadb/luamariadb_stmt.c",
            "src/mariadb/luamariadb_cursor.c",
            "src/mariadb/luamariadb_stmt_close.c",
            "src/mariadb/luamariadb_stmt_fetch.c",
            "src/mariadb/luamariadb_stmt_storeresult.c",
            "src/mariadb/luamariadb_stmt_sendlongdata.c",
            "src/mariadb/luamariadb_stmt_execute.c",
         },
         defines = { "FAN_HAS_OPENSSL=1", "FAN_HAS_LUAJIT=1", "_GNU_SOURCE=1" },
         libraries = { "event", "event_openssl", "ssl", "crypto", "curl", "resolv", "mysqlclient" },
         incdirs = { "$(CURL_INCDIR)", "$(LIBEVENT_INCDIR)", "$(OPENSSL_INCDIR)", "$(MARIADB_INCDIR)" },
         libdirs = { "$(CURL_LIBDIR)", "$(LIBEVENT_LIBDIR)", "$(OPENSSL_LIBDIR)", "$(MARIADB_LIBDIR)" }
      },
      ["fan.connector.init"] = "modules/fan/connector/init.lua",
      ["fan.connector.tcp"] = "modules/fan/connector/tcp.lua",
      ["fan.connector.udp"] = "modules/fan/connector/udp.lua",
      ["fan.connector.fifo"] = "modules/fan/connector/fifo.lua",
      ["fan.worker.init"] = "modules/fan/worker/init.lua",
      ["fan.pool"] = "modules/fan/pool.lua",
      ["fan.stream.init"] = "modules/fan/stream/init.lua",
      ["fan.stream.ffi"] = "modules/fan/stream/ffi.lua",
      ["fan.stream.bit"] = "modules/fan/stream/bit.lua",
      ["fan.httpd.init"] = "modules/fan/httpd/init.lua",
      ["fan.httpd.httpd"] = "modules/fan/httpd/httpd.lua",
      ["fan.objectbuf.init"] = "modules/fan/objectbuf/init.lua",
      ["fan.upnp"] = "modules/fan/upnp.lua",
      ["fan.utils"] = "modules/fan/utils.lua",
      ["mariadb.orm"] = "modules/mariadb/orm.lua",
      ["mariadb.pool"] = "modules/mariadb/pool.lua",
      ["fan.http.init"] = "modules/fan/http/init.lua",
      ["fan.http.http"] = "modules/fan/http/http.lua",
      ["config"] = "modules/config.lua",
      ["sqlite3.orm"] = "modules/sqlite3/orm.lua"
   }
}
