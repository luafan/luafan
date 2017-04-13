config
======

### `local config = require "config"`

load config files from `./config` and `./config.d`

### config samples

* mariadb config

```lua
-- mariadb/mysql host
maria_host = os.getenv("MARIA_HOST") or os.getenv("MARIA_PORT_3306_TCP_ADDR") or "127.0.0.1"

-- mariadb/mysql port
maria_port = os.getenv("MARIA_PORT") or os.getenv("MARIA_PORT_3306_TCP_PORT")

-- mariadb/mysql database name
maria_database = os.getenv("MARIA_DATABASE_NAME") or "test"

-- mariadb/mysql user
maria_user = os.getenv("MARIA_USERNAME") or "root"

-- mariadb/mysql password
maria_passwd = os.getenv("MARIA_PASSWORD") or os.getenv("MARIA_ENV_MYSQL_ROOT_PASSWORD")

-- mariadb/mysql charset
maria_charset = os.getenv("MARIA_CHARSET") or "utf8"

-- mariadb/mysql connection pool size.
maria_pool_size = tonumber(os.getenv("MARIA_POOL_SIZE") or 10)
```

* worker config

```lua
-- set fan.pool's pool size.
pool_size = tonumber(os.getenv("POOL_SIZE") or 10)

-- set serialize method for different process.
worker_using_cjson = true -- default false, use fan.objectbuf
```

* develop config

```lua
-- enable debug log output.
debug = os.getenv("DEBUG") == "true"

-- udp package timeout to resend udp data.
udp_package_timeout = 3 -- default 2

-- config udp ack waiting pool size, if full, all new packet will keep waiting until pool size is not full.
udp_waiting_count = 20 -- default 10

-- config single udp packet mtu, config only if you know what you are doing.
udp_mtu = 8492 -- default 576

-- config time between two udp timeout checking task.
udp_check_timeout_duration = 1 -- default 0.5

-- config use luajit bit instead default fan.stream, to reduce luajit NYI report.
stream_bit = true -- default false

-- config use luajit ffi feature. read fan/stream/ffi.lua for more information (how to compile).
stream_ffi = false -- default false
```
