[![CircleCI](https://circleci.com/gh/luafan/luafan.svg?style=svg)](https://circleci.com/gh/luafan/luafan)

# LuaFan - Asynchronous Network and I/O Library for Lua

A single-process, non-blocking network and I/O library for Lua that provides asynchronous APIs with full coroutine support. LuaFan simulates blocking code style over non-blocking implementation, making it easy to write sequential API calls without callbacks.

* [Official Documentation](http://luafan.com/)

## Build Instructions

```bash
sudo luarocks make luafan-0.7-3.rockspec MARIADB_DIR=/usr/local/mysql CURL_INCDIR=/usr/include/`uname -m`-linux-gnu
```

## Package Variants

1. **luafan** (full) - Complete package with all features including MariaDB support
2. **luafanlite** - No MariaDB, includes HTTP/SSL features
3. **luafanmicro** - Minimal version with basic networking only (no SSL/HTTP client)

## Core C Modules

- `fan.tcpd` - TCP protocol module for client/server connections
- `fan.udpd` - UDP protocol module with DNS support
- `fan.fifo` - FIFO pipe module for inter-process communication
- `fan.httpd` - HTTP server module with SSL support
- `fan.http` - HTTP client module
- `fan.stream` - Stream processing utilities
- `fan.objectbuf` - Object serialization helpers
- `fan.mariadb` - MariaDB client module (full version only)

## Lua Modules

- `fan.connector` - Unified connector for TCP/UDP/FIFO with URL-based connections
- `fan.worker` - Multi-process worker management
- `fan.pool` - Connection pooling utilities
- `mariadb.orm` - MariaDB ORM implementation
- `mariadb.pool` - MariaDB connection pool
- `sqlite3.orm` - SQLite3 ORM implementation
- `config` - Common configuration module

## Dependencies

### External Dependencies
- **libevent** (required for all versions) - Event-driven programming
- **OpenSSL** (luafan + luafanlite) - SSL/TLS support
- **libcurl** (luafan + luafanlite) - HTTP client functionality
- **MariaDB/MySQL client** (luafan only) - Database connectivity

### Lua Dependencies
- **Lua >= 5.1** (supports LuaJIT)

## Project Structure

```
luafan/
├── src/                    # C source code (25+ files)
│   ├── luafan.c           # Main library entry point
│   ├── tcpd*.c            # TCP implementation
│   ├── udpd*.c            # UDP implementation
│   ├── httpd.c            # HTTP server
│   ├── fifo.c             # FIFO pipes
│   └── event_mgr.c        # Event management
├── modules/               # Lua modules (21 files)
│   ├── fan/               # Core fan modules
│   ├── mariadb/           # Database ORM/pool
│   └── sqlite3/           # SQLite ORM
├── docs/                  # API documentation
├── *.rockspec            # Three LuaRocks specifications
├── CMakeLists.txt        # CMake build configuration
├── build*.sh             # Build scripts
├── ios/                  # iOS platform support
├── openwrt/              # OpenWrt platform support
└── Dockerfile.*          # Docker configurations
```

## Key Features

- **Event-driven architecture** using libevent
- **Coroutine-based async programming** - no callback hell
- **Cross-platform support** (Linux, macOS, Windows, iOS, OpenWrt)
- **SSL/TLS support** for secure communications
- **Database integration** with ORM patterns
- **HTTP server and client** capabilities
- **Process management** and worker pools
- **Memory-efficient streaming** and serialization

## Quick Start

1. Ensure all dependencies are installed
2. Build using the command above
3. Check `docs/api/` directory for detailed API documentation
4. Reference example code in the `modules/` directory

## License

MIT
