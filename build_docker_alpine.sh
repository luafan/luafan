TZ=Asia/Shanghai
ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

LUAFAN_VERSION=0.7-3
LUAROCKS_VERSION=3.13.0
MARIADB_VERSION=5.5.68
OPENSSL_VERSION=1.1.1w

LUA_VERSION=5.3.6

apk add --update bsd-compat-headers tzdata linux-headers git libstdc++ wget ca-certificates gcc libc-dev unzip cmake g++ make libevent libevent-dev curl-dev curl ncurses-dev readline-dev bison openssl-dev openssl perl sqlite-dev \
    && update-ca-certificates \
    `# --- luafan sources (cloned early: fan_lua_lock.{h,c} needed to build lua) ---` \
    && git clone https://github.com/luafan/luafan.git /opt/luafan \
    `# --- Lua interpreter from source WITH the global lua_lock hook ---` \
    `# apk lua5.3 bakes lua_lock as a no-op, unsafe once luafan runs worker` \
    `# threads sharing one lua_State. Build Lua ourselves, force-including` \
    `# luafan/src/fan_lua_lock.h and linking fan_lua_lock.c into CORE_O so the` \
    `# lock symbols are exported (-Wl,-E) and resolvable by fan.so at dlopen.` \
    && cd /opt && wget https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz && tar xzf lua-$LUA_VERSION.tar.gz && cd lua-$LUA_VERSION \
        && cp /opt/luafan/src/fan_lua_lock.c src/fan_lua_lock.c && cp /opt/luafan/src/fan_lua_lock.h src/fan_lua_lock.h \
        && sed -i 's/^CORE_O=/CORE_O= fan_lua_lock.o /' src/Makefile \
        && make linux MYCFLAGS="-fPIC -include fan_lua_lock.h -pthread" MYLIBS="-pthread" \
        && make install INSTALL_TOP=/usr/local && cp src/luaconf.h /usr/local/include/ \
        && cd /opt && rm -rf lua-$LUA_VERSION* \
    && ln -sf /usr/local/bin/lua /usr/bin/lua \
    && wget https://luarocks.github.io/luarocks/releases/luarocks-$LUAROCKS_VERSION.tar.gz && tar xzf luarocks-$LUAROCKS_VERSION.tar.gz && cd luarocks-$LUAROCKS_VERSION && ./configure --with-lua=/usr/local && make build && make install && cd .. \
    && wget https://github.com/MariaDB/server/archive/mariadb-$MARIADB_VERSION.tar.gz && tar xzf mariadb-$MARIADB_VERSION.tar.gz \
    && cd server-mariadb-$MARIADB_VERSION && sed -i '/HAVE_UCONTEXT_H/d' config.h.cmake && cmake -DWITHOUT_TOKUDB=1 . && cd libmysql && make -j$(nproc) install && cd ../include && make install && cd ../.. && rm -rf mariadb-$MARIADB_VERSION.tar.gz server-mariadb-$MARIADB_VERSION \
    && wget https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz && tar xzf openssl-$OPENSSL_VERSION.tar.gz && cd openssl-$OPENSSL_VERSION && ./config && make -j$(nproc) && make install && cd .. && rm -rf openssl* \
    `# fan.so: force-include the SAME hook header so its lua_lock and depth` \
    `# accessors match the interpreter; symbols resolve at dlopen. The header` \
    `# self-defines LUA_USER_H (so utlua.c uses the real depth funcs). -I so the` \
    `# #include LUA_USER_H inside luafan sources finds it.` \
    && cd /opt/luafan && luarocks make luafan-$LUAFAN_VERSION.rockspec MARIADB_DIR=/usr/local/mysql CFLAGS="-O2 -fPIC -include /opt/luafan/src/fan_lua_lock.h -I/opt/luafan/src -pthread" && cd / && rm -rf /opt/luafan \
    && luarocks install compat53 && luarocks install lpeg && luarocks install lua-cjson 2.1.0-1 && luarocks install luafilesystem \
    && git clone --depth 1 --branch 0.4.1.53-luafan1 https://github.com/luafan/lzlib.git /tmp/lzlib-luafan && cd /tmp/lzlib-luafan && luarocks make rockspec/lzlib-0.4.1.53.luafan1-1.rockspec ZLIB_LIBDIR=/lib && cd - && rm -rf /tmp/lzlib-luafan && luarocks install openssl && luarocks install lbase64 \
    && luarocks install lua-protobuf \
    && luarocks install lmd5 \
    && luarocks install lua-iconv \
    && luarocks install lsqlite3 \
    && cd luarocks-$LUAROCKS_VERSION && make uninstall && cd .. && rm -rf luarocks* \
    && apk del linux-headers git g++ bison ncurses-dev readline-dev libc-dev curl-dev wget libevent-dev cmake make gcc unzip openssl-dev bsd-compat-headers perl \
    `# Keep /usr/local/bin/lua (source-built with the lock hook); only drop` \
    `# luarocks' launcher scripts, not the lua/luac interpreters.` \
    && rm -f /usr/local/bin/luarocks /usr/local/bin/luarocks-admin \
    && rm -rf /usr/include /usr/local/include /usr/local/share/doc /usr/local/share/man /usr/local/share/lua/5.3/luarocks /usr/local/mysql/lib/*.a /usr/local/mysql/include /var/cache/apk/* .cache
