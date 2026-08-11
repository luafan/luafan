#!/usr/bin/env bash
# Ubuntu image builder for luafan. Invoked as a single RUN in Dockerfile.ubuntu
# and also as a plain script in CI (ci.yml -> Build LuaFan (Ubuntu)); the two
# environments differ only in starting cwd, so we normalize to /opt below.
set -eux

TZ=Asia/Shanghai
ln -snf /usr/share/zoneinfo/$TZ /etc/localtime
echo $TZ > /etc/timezone

LUAFAN_VERSION=0.7-3
LUAROCKS_VERSION=3.13.0
MARIADB_VERSION=5.5.68
OPENSSL_VERSION=1.1.1w
LUA_VERSION=5.3.6

# --- packages ---
# NOTE: no libreadline* -- we build Lua without readline (container has no REPL
# use case) so we avoid the autoremove trap that dropped libreadline8 and broke
# /usr/local/bin/lua at runtime.
apt update
apt install -y \
    ca-certificates libsqlite3-0 libsqlite3-dev tzdata wget unzip \
    zlib1g-dev make gcc libc-dev \
    libcurl4-openssl-dev libcurl4 \
    libevent-dev libevent-2.1-7 libevent-core-2.1-7 libevent-extra-2.1-7 libevent-openssl-2.1-7 \
    git cmake g++ bison libncurses5-dev
update-ca-certificates

# Normalize cwd so every download/extract below lives under /opt regardless of
# who called us (Dockerfile WORKDIR / CI $GITHUB_WORKSPACE).
cd /opt

# --- luafan sources (cloned early: we need fan_lua_lock.{h,c} to build lua) ---
git clone https://github.com/luafan/luafan.git /opt/luafan

# --- Lua interpreter built from source WITH the global lua_lock hook ---
# Stock apt lua5.3 bakes lua_lock as a no-op, which is unsafe once luafan runs
# worker threads (they share one lua_State). We compile Lua ourselves,
# force-including luafan/src/fan_lua_lock.h and linking fan_lua_lock.c into the
# core objects so LockMainState/... are exported (-Wl,-E) for fan.so.
# We also DROP readline: the container has no interactive REPL use case, and
# libreadline-dev pulls libreadline8 which gets swept by autoremove during
# cleanup below -- leaving /usr/local/bin/lua broken at runtime.
wget https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz
tar xzf lua-$LUA_VERSION.tar.gz
(
    cd lua-$LUA_VERSION
    cp /opt/luafan/src/fan_lua_lock.c /opt/luafan/src/fan_lua_lock.h src/
    sed -i 's/^CORE_O=/CORE_O= fan_lua_lock.o /' src/Makefile
    # Strip readline from the linux target and from luaconf.h auto-defines.
    sed -i 's/ -lreadline//' src/Makefile
    sed -i 's|^#define LUA_USE_READLINE|/* readline disabled */|' src/luaconf.h
    make linux MYCFLAGS="-fPIC -include fan_lua_lock.h -pthread" MYLIBS="-pthread"
    make install INSTALL_TOP=/usr/local
    cp src/luaconf.h /usr/local/include/
)
rm -rf lua-$LUA_VERSION*

# --- luarocks (kept until end so we can 'make uninstall' during cleanup) ---
wget https://luarocks.github.io/luarocks/releases/luarocks-$LUAROCKS_VERSION.tar.gz
tar xzf luarocks-$LUAROCKS_VERSION.tar.gz
(
    cd luarocks-$LUAROCKS_VERSION
    ./configure --with-lua=/usr/local
    make build
    make install
)

# --- mariadb (client libs / headers only) ---
wget https://github.com/MariaDB/server/archive/mariadb-$MARIADB_VERSION.tar.gz
tar xzf mariadb-$MARIADB_VERSION.tar.gz
(
    cd server-mariadb-$MARIADB_VERSION
    cmake .
    (cd libmysql && make -j$(nproc) install)
    (cd include  && make install)
)
rm -rf mariadb-$MARIADB_VERSION.tar.gz server-mariadb-$MARIADB_VERSION

# --- openssl 1.1.1w (system openssl on ubuntu 22.04 is 3.x; luafan needs 1.1) ---
wget https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz
tar xzf openssl-$OPENSSL_VERSION.tar.gz
(
    cd openssl-$OPENSSL_VERSION
    ./config
    make -j$(nproc)
    make install
)
rm -rf openssl*

# --- luafan itself ---
# Force-include the SAME hook header so fan.so's lua_lock and depth accessors
# match the interpreter; symbols resolve at dlopen. The header self-defines
# LUA_USER_H (so utlua.c uses the real depth funcs). -I so the #include
# LUA_USER_H inside luafan sources finds it.
(
    cd /opt/luafan
    luarocks make luafan-$LUAFAN_VERSION.rockspec \
        MARIADB_DIR=/usr/local/mysql \
        CURL_INCDIR=/usr/include/`uname -m`-linux-gnu \
        CFLAGS="-O2 -fPIC -include /opt/luafan/src/fan_lua_lock.h -I/opt/luafan/src -pthread"
)
rm -rf /opt/luafan

# --- extra rocks ---
luarocks install compat53
luarocks install lpeg
luarocks install lua-cjson 2.1.0-1
luarocks install luafilesystem

# luafan's lzlib fork (>8192 decompress fix)
git clone --depth 1 --branch 0.4.1.53-luafan1 https://github.com/luafan/lzlib.git /tmp/lzlib-luafan
(
    cd /tmp/lzlib-luafan
    luarocks make rockspec/lzlib-0.4.1.53.luafan1-1.rockspec
)
rm -rf /tmp/lzlib-luafan

luarocks install openssl
luarocks install lbase64
luarocks install lua-protobuf
luarocks install lmd5
luarocks install lua-iconv
luarocks install lsqlite3

# --- cleanup: uninstall luarocks + drop build toolchain ---
(
    cd luarocks-$LUAROCKS_VERSION
    make uninstall
)
rm -rf luarocks*

apt-get -y remove g++ bison libncurses5-dev libc-dev \
    zlib1g-dev libcurl4-openssl-dev libevent-dev unzip cmake make gcc \
    binutils libc-dev-bin git
apt-get -y autoremove

# Keep /usr/local/bin/lua (source-built with the lock hook). Only drop
# luarocks' own launcher scripts there, not the lua/luac interpreters.
rm -f /usr/local/bin/luarocks /usr/local/bin/luarocks-admin
rm -rf /usr/local/share/doc /usr/local/mysql/lib/*.a /usr/local/mysql/include \
       /var/lib/apt/lists/*
