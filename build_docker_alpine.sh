TZ=Asia/Shanghai
ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

LUAFAN_VERSION=0.8-0
LUAROCKS_VERSION=3.13.0
MARIADB_VERSION=5.5.68
BORINGSSL_COMMIT=1b7fdbd9101dedc3e0aa3fcf4ff74eacddb34ecc
LIBEVENT_VERSION=2.1.12-stable
CURL_VERSION=8.7.1

apk add --update bsd-compat-headers tzdata linux-headers git lua5.3-dev lua5.3 libstdc++ wget ca-certificates gcc libc-dev unzip cmake g++ make ncurses-dev bison perl go sqlite-dev \
    && ln -s /usr/bin/lua5.3 /usr/bin/lua \
    && git clone https://boringssl.googlesource.com/boringssl && cd boringssl && git checkout $BORINGSSL_COMMIT \
    && mkdir build && cd build && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON .. && make -j$(nproc) && cd .. \
    && cp -r include/openssl /usr/local/include/ && cp build/ssl/libssl.a /usr/local/lib/ && cp build/crypto/libcrypto.a /usr/local/lib/ \
    && echo '#ifndef SHLIB_VERSION_NUMBER' >> /usr/local/include/openssl/opensslv.h \
    && echo '#define SHLIB_VERSION_NUMBER "BoringSSL"' >> /usr/local/include/openssl/opensslv.h \
    && echo '#endif' >> /usr/local/include/openssl/opensslv.h \
    && cd .. && rm -rf boringssl \
    && wget https://github.com/libevent/libevent/releases/download/release-$LIBEVENT_VERSION/libevent-$LIBEVENT_VERSION.tar.gz && tar xzf libevent-$LIBEVENT_VERSION.tar.gz \
    && cd libevent-$LIBEVENT_VERSION && mkdir build && cd build \
    && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DEVENT__LIBRARY_TYPE=BOTH -DOPENSSL_ROOT_DIR=/usr/local -DOPENSSL_INCLUDE_DIR=/usr/local/include -DOPENSSL_CRYPTO_LIBRARY=/usr/local/lib/libcrypto.a -DOPENSSL_SSL_LIBRARY=/usr/local/lib/libssl.a .. \
    && make -j$(nproc) && make install && cd ../.. && rm -rf libevent-$LIBEVENT_VERSION* \
    && wget https://curl.se/download/curl-$CURL_VERSION.tar.gz && tar xzf curl-$CURL_VERSION.tar.gz \
    && cd curl-$CURL_VERSION && ./configure --with-openssl=/usr/local --without-libpsl LDFLAGS="-L/usr/local/lib" CPPFLAGS="-I/usr/local/include" LIBS="-lpthread" \
    && make -j$(nproc) && make install && cd .. && rm -rf curl-$CURL_VERSION* \
    && wget https://github.com/MariaDB/server/archive/mariadb-$MARIADB_VERSION.tar.gz && tar xzf mariadb-$MARIADB_VERSION.tar.gz \
    && cd server-mariadb-$MARIADB_VERSION && sed -i '/HAVE_UCONTEXT_H/d' config.h.cmake && cmake -DWITHOUT_TOKUDB=1 . && cd libmysql && make -j$(nproc) install && cd ../include && make install && cd ../.. && rm -rf mariadb-$MARIADB_VERSION.tar.gz server-mariadb-$MARIADB_VERSION \
    && wget https://luarocks.github.io/luarocks/releases/luarocks-$LUAROCKS_VERSION.tar.gz && tar xzf luarocks-$LUAROCKS_VERSION.tar.gz && cd luarocks-$LUAROCKS_VERSION && ./configure && make build && make install && cd .. \
    && git clone https://github.com/luafan/luafan.git && cd luafan && luarocks make luafan-$LUAFAN_VERSION.rockspec MARIADB_DIR=/usr/local/mysql && cd .. && rm -rf luafan \
    && luarocks install compat53 && luarocks install lpeg && luarocks install lua-cjson 2.1.0-1 && luarocks install luafilesystem \
    && luarocks install lzlib ZLIB_LIBDIR=/lib && luarocks install lbase64 \
    && luarocks install lua-protobuf \
    && luarocks install lmd5 \
    && luarocks install lua-iconv \
    && luarocks install lsqlite3 \
    && cd /tmp && luarocks unpack openssl && cd openssl-*/openssl-* \
    && sed -i '/#include <openssl\/ts.h>/d' src/openssl.h \
    && sed -i 's/#define OPENSSL_HAVE_TS//' src/openssl.h \
    && luarocks make && cd / && rm -rf /tmp/openssl-* \
    && cd /root && cd luarocks-$LUAROCKS_VERSION && make uninstall && cd .. && rm -rf luarocks* \
    && apk del linux-headers git lua5.3-dev g++ bison ncurses-dev libc-dev wget cmake make gcc unzip bsd-compat-headers perl go \
    && rm -rf /usr/include /usr/local/include /usr/local/share/doc /usr/local/share/man /usr/local/share/lua/5.3/luarocks /usr/local/mysql/lib/*.a /usr/local/mysql/include /usr/local/bin /var/cache/apk/* .cache
