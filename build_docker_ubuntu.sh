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
LIBEVENT_VERSION=2.1.12-stable

# --- packages ---
# NOTE: no libreadline* -- we build Lua without readline (container has no REPL
# use case) so we avoid the autoremove trap that dropped libreadline8 and broke
# /usr/local/bin/lua at runtime.
# NOTE: no libevent* from apt. Ubuntu 22.04's libevent_openssl links the system
# OpenSSL 3.x, while luafan builds against OpenSSL 1.1.1w (below). Mixing the
# two ABIs makes tcpd's SSL path (bufferevent_openssl_socket_new -> SSL_clear)
# crash in libcrypto.so.3 (EVP_CIPHER_CTX_reset) because a 1.1.1w-created SSL*
# is read with the 3.x struct layout. We compile libevent from source against
# 1.1.1w further down so both fan.so and libevent_openssl share one OpenSSL.
apt update
apt install -y \
    ca-certificates libsqlite3-0 libsqlite3-dev tzdata wget unzip \
    zlib1g-dev make gcc libc-dev \
    libcurl4-openssl-dev libcurl4 \
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

# Make the freshly-installed OpenSSL 1.1.1w visible to the dynamic linker and
# to pkg-config BEFORE building libevent, so libevent's OpenSSL bufferevent
# support links against 1.1.1w rather than the system 3.x.
ldconfig
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- libevent (built against OpenSSL 1.1.1w to match fan.so) ---
# We deliberately do NOT use apt's libevent: its libevent_openssl is linked to
# OpenSSL 3.x and crashes when handed a 1.1.1w SSL* by tcpd (see note at top).
# Building here (after OpenSSL 1.1.1w is installed) yields a libevent_openssl
# that shares the same OpenSSL as fan.so. Installs into /usr/local/lib, which
# ldconfig already ranks ahead of /usr/lib/x86_64-linux-gnu.
wget https://github.com/libevent/libevent/releases/download/release-$LIBEVENT_VERSION/libevent-$LIBEVENT_VERSION.tar.gz
tar xzf libevent-$LIBEVENT_VERSION.tar.gz
(
    cd libevent-$LIBEVENT_VERSION
    # --disable-shared? No: fan.so dlopen-resolves libevent_openssl at runtime,
    # so we need the shared libs. Force OpenSSL from /usr/local (1.1.1w).
    ./configure \
        --prefix=/usr/local \
        --disable-libevent-regress \
        --disable-samples \
        --disable-dependency-tracking \
        CPPFLAGS="-I/usr/local/include" \
        LDFLAGS="-L/usr/local/lib" \
        OPENSSL_LIBADD="-lssl -lcrypto"
    make -j$(nproc)
    make install
)
rm -rf libevent-$LIBEVENT_VERSION*
ldconfig

# Sanity check: the newly built libevent_openssl must link ONLY 1.1.1w.
if ldd /usr/local/lib/libevent_openssl-2.1.so.7 | grep -q 'libssl.so.3'; then
    echo "FATAL: libevent_openssl linked against system OpenSSL 3.x, aborting" >&2
    ldd /usr/local/lib/libevent_openssl-2.1.so.7 >&2
    exit 1
fi

# --- luafan itself ---
# Force-include the SAME hook header so fan.so's lua_lock and depth accessors
# match the interpreter; symbols resolve at dlopen. The header self-defines
# LUA_USER_H (so utlua.c uses the real depth funcs). -I so the #include
# LUA_USER_H inside luafan sources finds it.
(
    cd /opt/luafan
    luarocks make luafan-$LUAFAN_VERSION.rockspec \
        MARIADB_DIR=/usr/local/mysql \
        LIBEVENT_DIR=/usr/local \
        OPENSSL_DIR=/usr/local \
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
    zlib1g-dev libcurl4-openssl-dev unzip cmake make gcc \
    binutils libc-dev-bin git
apt-get -y autoremove

# Keep /usr/local/bin/lua (source-built with the lock hook). Only drop
# luarocks' own launcher scripts there, not the lua/luac interpreters.
rm -f /usr/local/bin/luarocks /usr/local/bin/luarocks-admin
rm -rf /usr/local/share/doc /usr/local/mysql/lib/*.a /usr/local/mysql/include \
       /var/lib/apt/lists/*
