## Quick Setup

### Ubuntu/Debian (Current LTS) + LuaJIT + luafanlite

```sh
# Update system and install dependencies
sudo apt-get update
sudo apt-get install -y wget lua5.1-dev luajit make gcc libc6-dev \
    libcurl4-openssl-dev libevent-dev libssl-dev git build-essential

# Install LuaRocks (use system package if available)
sudo apt-get install -y luarocks || {
    # Manual installation if package not available
    wget https://luarocks.org/releases/luarocks-3.9.2.tar.gz
    tar xzf luarocks-3.9.2.tar.gz
    cd luarocks-3.9.2
    ./configure
    make build
    sudo make install
    cd ..
    rm -rf luarocks-3.9.2*
}

# Install luafanlite
sudo luarocks install luafanlite
```

### Ubuntu/Debian + LuaJIT + luafan (with MariaDB)

```sh
# Update system and install base dependencies
sudo apt-get update
sudo apt-get install -y wget lua5.1-dev luajit make gcc libc6-dev \
    libcurl4-openssl-dev libevent-dev libssl-dev git build-essential \
    cmake pkg-config

# Install MariaDB development libraries
sudo apt-get install -y libmariadb-dev libmariadb-dev-compat

# Install LuaRocks
sudo apt-get install -y luarocks || {
    wget https://luarocks.org/releases/luarocks-3.9.2.tar.gz
    tar xzf luarocks-3.9.2.tar.gz
    cd luarocks-3.9.2
    ./configure
    make build
    sudo make install
    cd ..
    rm -rf luarocks-3.9.2*
}

# Install luafan with MariaDB support
sudo luarocks install luafan MARIADB_DIR=/usr CURL_INCDIR=/usr/include/x86_64-linux-gnu
```

### Alternative: Using System MySQL/MariaDB

If you have MySQL or MariaDB installed system-wide:

```sh
# For systems with mysql-config available
sudo luarocks install luafan MARIADB_DIR=$(mysql_config --variable=prefix) \
    CURL_INCDIR=/usr/include/$(uname -m)-linux-gnu

# Or specify path directly
sudo luarocks install luafan MARIADB_DIR=/usr/local/mysql \
    CURL_INCDIR=/usr/include/$(uname -m)-linux-gnu
```

### macOS + Homebrew

```sh
# Install dependencies via Homebrew
brew install lua luajit luarocks libevent openssl curl

# For luafanlite
luarocks install luafanlite

# For luafan with MariaDB
brew install mariadb
luarocks install luafan MARIADB_DIR=/usr/local OPENSSL_DIR=/usr/local/opt/openssl
```

### CentOS/RHEL/Rocky Linux

```sh
# Install EPEL repository first
sudo yum install -y epel-release

# Install dependencies
sudo yum install -y lua-devel luajit-devel gcc gcc-c++ make wget git \
    libevent-devel openssl-devel libcurl-devel

# Install LuaRocks
sudo yum install -y luarocks || {
    wget https://luarocks.org/releases/luarocks-3.9.2.tar.gz
    tar xzf luarocks-3.9.2.tar.gz
    cd luarocks-3.9.2
    ./configure
    make build
    sudo make install
    cd ..
    rm -rf luarocks-3.9.2*
}

# For MariaDB support
sudo yum install -y mariadb-devel
sudo luarocks install luafan MARIADB_DIR=/usr CURL_INCDIR=/usr/include
```

### Docker

Pre-built Docker images are available:

```sh
# Alpine-based (minimal)
docker pull luafan/alpine

# Ubuntu-based (full features)
docker pull luafan/ubuntu
```

**Links:**
- [luafan-alpine](https://github.com/luafan/luafan-alpine)
- [luafan-ubuntu](https://github.com/luafan/luafan-ubuntu)

### Building from Source

For latest development version or custom builds:

```sh
git clone https://github.com/luafan/luafan.git
cd luafan

# Build luafanlite
sudo luarocks make luafanlite-0.7-3.rockspec

# Build luafan with MariaDB
sudo luarocks make luafan-0.7-3.rockspec MARIADB_DIR=/usr/local/mysql \
    CURL_INCDIR=/usr/include/$(uname -m)-linux-gnu

# Build luafanmicro (minimal)
sudo luarocks make luafanmicro-0.7-3.rockspec
```

## Package Variants

Choose the appropriate variant based on your needs:

| Package | Features | Dependencies |
|---------|----------|--------------|
| **luafan** | Full features with MariaDB | libevent, openssl, curl, mariadb |
| **luafanlite** | HTTP + SSL, no database | libevent, openssl, curl |
| **luafanmicro** | Basic networking only | libevent |

## Troubleshooting

### Common Issues

**Missing headers during compilation:**
```sh
# Install development packages
sudo apt-get install -y lua5.1-dev libevent-dev libssl-dev libcurl4-openssl-dev
```

**MariaDB/MySQL not found:**
```sh
# Find your MySQL installation
mysql_config --cflags
mysql_config --libs

# Or use pkg-config
pkg-config --cflags --libs mysqlclient
```

**LuaJIT FFI issues:**
```sh
# Ensure LuaJIT development headers are installed
sudo apt-get install -y libluajit-5.1-dev
```

### Platform-Specific Notes

- **Ubuntu 18.04+**: Use `libmariadb-dev` instead of `libmysqlclient-dev`
- **macOS**: May need to specify OpenSSL path explicitly
- **Alpine Linux**: Use `musl-dev` instead of `libc6-dev`
- **ARM64**: Architecture detection should work automatically with `uname -m`
