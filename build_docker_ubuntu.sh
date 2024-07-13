TZ=Asia/Shanghai
ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

LUAFAN_VERSION=0.7-3
LUAROCKS_VERSION=3.9.2
MARIADB_VERSION=5.5.68
OPENSSL_VERSION=1.1.1w

apt update \
    && apt install -y libsqlite3-0 libsqlite3-dev tzdata wget liblua5.3-dev lua5.3 unzip zlib1g-dev make gcc libc-dev libcurl4-openssl-dev libcurl4 libevent-dev git libevent-2.1-7 libevent-core-2.1-7 libevent-extra-2.1-7 libevent-openssl-2.1-7 cmake g++ bison libncurses5-dev \
    && wget http://luarocks.org/releases/luarocks-$LUAROCKS_VERSION.tar.gz && tar xzf luarocks-$LUAROCKS_VERSION.tar.gz && cd luarocks-$LUAROCKS_VERSION && ./configure && make build && make install && cd .. \
    && wget https://github.com/MariaDB/server/archive/mariadb-$MARIADB_VERSION.tar.gz && tar xzf mariadb-$MARIADB_VERSION.tar.gz && cd server-mariadb-$MARIADB_VERSION && cmake . && cd libmysql && make install && cd ../include && make install && cd ../.. && rm -rf mariadb-$MARIADB_VERSION.tar.gz server-mariadb-$MARIADB_VERSION \
    && wget https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz && tar xzf openssl-$OPENSSL_VERSION.tar.gz && cd openssl-$OPENSSL_VERSION && ./config && make && make install && cd .. \
    && luarocks make luafan-$LUAFAN_VERSION.rockspec MARIADB_DIR=/usr/local/mysql CURL_INCDIR=/usr/include/`uname -m`-linux-gnu \
    && luarocks install compat53 && luarocks install lpeg && luarocks install lua-cjson 2.1.0-1 && luarocks install luafilesystem \
    && luarocks install lzlib && luarocks install openssl && luarocks install lbase64 \
    && luarocks install lua-protobuf \
    && luarocks install lmd5 \
    && luarocks install lua-iconv \
    && luarocks install lsqlite3 \
    && cd luarocks-$LUAROCKS_VERSION && make uninstall && cd .. && rm -rf luarocks* \
    && apt-get -y remove g++ bison libncurses5-dev liblua5.3-dev libc-dev zlib1g-dev libcurl4-openssl-dev libevent-dev unzip cmake make gcc binutils libc-dev-bin git \
    && apt-get -y autoremove \
    && rm -rf openssl* /usr/local/share/doc /usr/local/mysql/lib/*.a /var/lib/apt/lists/* || exit -1
