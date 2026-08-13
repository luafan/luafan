#!/usr/bin/env sh
# Alpine image builder for luafan. Invoked as a single RUN in Dockerfile.alpine
# and also as a plain script in CI (ci.yml -> Build LuaFan (Alpine)); the two
# environments differ only in starting cwd, so we normalize to /opt below.
# Uses POSIX sh (busybox ash), not bash -- keep it portable.
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
# NOTE: no readline-dev -- we build Lua without readline (container has no REPL
# use case). Matches the ubuntu build; avoids libreadline runtime dep.
# NOTE: no libevent/libevent-dev from apk. To stay byte-for-byte consistent
# with the ubuntu image (and not depend on Alpine's system OpenSSL happening to
# match), we compile libevent from source against the self-built OpenSSL 1.1.1w
# below, so fan.so and libevent_openssl always share one OpenSSL. (On Ubuntu
# 22.04 the apt libevent_openssl links OpenSSL 3.x and mixing it with luafan's
# 1.1.1w crashes tcpd's SSL path; Alpine 3.16 happens to ship 1.1.1w so it did
# not crash, but we build our own here regardless for a single, predictable
# OpenSSL across both images.)
apk add --update \
    bsd-compat-headers tzdata linux-headers git libstdc++ wget ca-certificates \
    gcc libc-dev unzip cmake g++ make \
    curl-dev curl \
    ncurses-dev bison openssl-dev openssl perl sqlite-dev
update-ca-certificates

# Normalize cwd so every download/extract below lives under /opt.
cd /opt

# --- luafan sources (cloned early: fan_lua_lock.{h,c} needed to build lua) ---
git clone https://github.com/luafan/luafan.git /opt/luafan

# --- Lua interpreter from source WITH the global lua_lock hook ---
# apk lua5.3 bakes lua_lock as a no-op, unsafe once luafan runs worker threads
# sharing one lua_State. Build Lua ourselves, force-including
# luafan/src/fan_lua_lock.h and linking fan_lua_lock.c into CORE_O so the lock
# symbols are exported (-Wl,-E) and resolvable by fan.so at dlopen.
wget https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz
tar xzf lua-$LUA_VERSION.tar.gz
(
    cd lua-$LUA_VERSION
    cp /opt/luafan/src/fan_lua_lock.c src/fan_lua_lock.c
    cp /opt/luafan/src/fan_lua_lock.h src/fan_lua_lock.h
    sed -i 's/^CORE_O=/CORE_O= fan_lua_lock.o /' src/Makefile
    # Strip readline from the linux target and from luaconf.h auto-defines.
    sed -i 's/ -lreadline//' src/Makefile
    sed -i 's|^#define LUA_USE_READLINE|/* readline disabled */|' src/luaconf.h
    make linux MYCFLAGS="-fPIC -include fan_lua_lock.h -pthread" MYLIBS="-pthread"
    make install INSTALL_TOP=/usr/local
    cp src/luaconf.h /usr/local/include/
)
rm -rf lua-$LUA_VERSION*
ln -sf /usr/local/bin/lua /usr/bin/lua

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
# alpine's musl lacks ucontext.h; strip the probe so cmake doesn't bail.
wget https://github.com/MariaDB/server/archive/mariadb-$MARIADB_VERSION.tar.gz
tar xzf mariadb-$MARIADB_VERSION.tar.gz
(
    cd server-mariadb-$MARIADB_VERSION
    sed -i '/HAVE_UCONTEXT_H/d' config.h.cmake
    cmake -DWITHOUT_TOKUDB=1 .
    (cd libmysql && make -j$(nproc) install)
    (cd include  && make install)
)
rm -rf mariadb-$MARIADB_VERSION.tar.gz server-mariadb-$MARIADB_VERSION

# --- openssl 1.1.1w ---
wget https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz
tar xzf openssl-$OPENSSL_VERSION.tar.gz
(
    cd openssl-$OPENSSL_VERSION
    ./config
    make -j$(nproc)
    make install
)
rm -rf openssl*

# Point pkg-config at the freshly-installed OpenSSL 1.1.1w BEFORE building
# libevent, so libevent's OpenSSL bufferevent support links against it. musl
# has no ldconfig; its default library search path already includes
# /usr/local/lib ahead of /usr/lib, so the self-built libs win at runtime.
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- libevent (built against OpenSSL 1.1.1w to match fan.so) ---
# Mirrors the ubuntu build: we do NOT use the distro libevent, we compile it
# here (after OpenSSL 1.1.1w is installed) so libevent_openssl shares the same
# OpenSSL as fan.so. Installs into /usr/local/lib.
wget https://github.com/libevent/libevent/releases/download/release-$LIBEVENT_VERSION/libevent-$LIBEVENT_VERSION.tar.gz
tar xzf libevent-$LIBEVENT_VERSION.tar.gz
(
    cd libevent-$LIBEVENT_VERSION
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

# Sanity check: the newly built libevent_openssl must link the self-built
# OpenSSL 1.1.1w and NOT drag in a mismatched system OpenSSL 3.x.
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
    luarocks make rockspec/lzlib-0.4.1.53.luafan1-1.rockspec ZLIB_LIBDIR=/lib
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

apk del linux-headers git g++ bison ncurses-dev libc-dev \
    curl-dev wget cmake make gcc unzip openssl-dev \
    bsd-compat-headers perl

# Keep /usr/local/bin/lua (source-built with the lock hook); only drop
# luarocks' launcher scripts, not the lua/luac interpreters.
rm -f /usr/local/bin/luarocks /usr/local/bin/luarocks-admin
rm -rf /usr/include /usr/local/include /usr/local/share/doc /usr/local/share/man \
       /usr/local/share/lua/5.3/luarocks \
       /usr/local/mysql/lib/*.a /usr/local/mysql/include \
       /var/cache/apk/* .cache
