TZ=Asia/Shanghai
ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

LUAFAN_VERSION=0.7-3
LUAROCKS_VERSION=3.9.2
MARIADB_VERSION=5.5.68
OPENSSL_VERSION=1.1.1w

apk add --update bsd-compat-headers tzdata linux-headers git lua5.3-dev lua5.3 libstdc++ wget ca-certificates gcc libc-dev unzip cmake g++ make libevent libevent-dev curl-dev curl ncurses-dev bison openssl-dev openssl perl sqlite-dev \
    && ln -s /usr/bin/lua5.3 /usr/bin/lua \
    && wget http://luarocks.org/releases/luarocks-$LUAROCKS_VERSION.tar.gz && tar xzf luarocks-$LUAROCKS_VERSION.tar.gz && cd luarocks-$LUAROCKS_VERSION && ./configure && make build && make install && cd .. \
    && wget https://github.com/MariaDB/server/archive/mariadb-$MARIADB_VERSION.tar.gz && tar xzf mariadb-$MARIADB_VERSION.tar.gz \
    && cd server-mariadb-$MARIADB_VERSION && sed -i '/HAVE_UCONTEXT_H/d' config.h.cmake && cmake -DWITHOUT_TOKUDB=1 . && cd libmysql && make install && cd ../include && make install && cd ../.. && rm -rf mariadb-$MARIADB_VERSION.tar.gz server-mariadb-$MARIADB_VERSION \
    && wget https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz && tar xzf openssl-$OPENSSL_VERSION.tar.gz && cd openssl-$OPENSSL_VERSION && ./config && make && make install && cd .. && rm -rf openssl* \
    && git clone https://github.com/luafan/luafan.git && cd luafan && luarocks make luafan-$LUAFAN_VERSION.rockspec MARIADB_DIR=/usr/local/mysql && cd .. && rm -rf luafan \
    && luarocks install compat53 && luarocks install lpeg && luarocks install lua-cjson 2.1.0-1 && luarocks install luafilesystem \
    && luarocks install lzlib ZLIB_LIBDIR=/lib && luarocks install openssl && luarocks install lbase64 \
    && luarocks install lua-protobuf \
    && luarocks install lmd5 \
    && luarocks install lua-iconv \
    && luarocks install lsqlite3 \
    && cd luarocks-$LUAROCKS_VERSION && make uninstall && cd .. && rm -rf luarocks* \
    && apk del linux-headers git lua5.3-dev g++ bison ncurses-dev libc-dev curl-dev wget libevent-dev cmake make gcc unzip openssl-dev bsd-compat-headers perl \
    && rm -rf /usr/include /usr/local/include /usr/local/share/doc /usr/local/share/man /usr/local/share/lua/5.3/luarocks /usr/local/mysql/lib/*.a /var/cache/apk/* .cache
