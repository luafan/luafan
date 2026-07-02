TZ=Asia/Shanghai
ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

LUAFAN_VERSION=0.8-0
LUAROCKS_VERSION=3.13.0
MARIADB_VERSION=5.5.68
BORINGSSL_COMMIT=1b7fdbd9101dedc3e0aa3fcf4ff74eacddb34ecc
LIBEVENT_VERSION=2.1.12-stable
CURL_IMPERSONATE_VERSION=0.6.1
BROTLI_VERSION=1.0.9
NGHTTP2_VERSION=nghttp2-1.56.0

apk add --update bsd-compat-headers tzdata linux-headers git lua5.3-dev lua5.3 libstdc++ wget ca-certificates gcc libc-dev unzip cmake g++ make ncurses-dev bison perl go sqlite-dev ninja autoconf automake libtool pkgconfig curl \
    && ln -s /usr/bin/lua5.3 /usr/bin/lua \
    && git clone https://github.com/lwthiker/curl-impersonate.git && cd curl-impersonate \
    && wget -q https://github.com/google/boringssl/archive/$BORINGSSL_COMMIT.zip -O boringssl.zip \
    && unzip -q boringssl.zip && mv boringssl-$BORINGSSL_COMMIT boringssl \
    && cd boringssl && for p in ../chrome/patches/boringssl-*.patch; do patch -p1 < $p; done \
    && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_C_FLAGS="-Wno-unknown-warning-option -Wno-stringop-overflow -Wno-array-bounds" -DBUILD_SHARED_LIBS=ON -GNinja .. \
    && ninja \
    && mkdir -p lib && cp crypto/libcrypto.so lib/ && cp ssl/libssl.so lib/ && cp -Rf ../include . \
    && cd ../.. \
    && cp -r boringssl/include/openssl /usr/local/include/ \
    && cp boringssl/build/crypto/libcrypto.so /usr/local/lib/ && cp boringssl/build/ssl/libssl.so /usr/local/lib/ \
    && ldconfig \
    && mkdir -p /usr/local/lib/pkgconfig \
    && printf 'prefix=/usr/local\nexec_prefix=${prefix}\nlibdir=${exec_prefix}/lib\nincludedir=${prefix}/include\n\nName: OpenSSL-libcrypto\nDescription: BoringSSL crypto library\nVersion: 1.1.1\nLibs: -L${libdir} -lcrypto\nLibs.private: -lpthread\nCflags: -I${includedir}\n' > /usr/local/lib/pkgconfig/libcrypto.pc \
    && printf 'prefix=/usr/local\nexec_prefix=${prefix}\nlibdir=${exec_prefix}/lib\nincludedir=${prefix}/include\n\nName: OpenSSL-libssl\nDescription: BoringSSL ssl library\nVersion: 1.1.1\nRequires.private: libcrypto\nLibs: -L${libdir} -lssl\nLibs.private: -lpthread\nCflags: -I${includedir}\n' > /usr/local/lib/pkgconfig/libssl.pc \
    && printf 'prefix=/usr/local\nexec_prefix=${prefix}\nlibdir=${exec_prefix}/lib\nincludedir=${prefix}/include\n\nName: OpenSSL\nDescription: BoringSSL (OpenSSL compatible)\nVersion: 1.1.1\nRequires: libssl libcrypto\n' > /usr/local/lib/pkgconfig/openssl.pc \
    && echo '#ifndef SHLIB_VERSION_NUMBER' >> /usr/local/include/openssl/opensslv.h \
    && echo '#define SHLIB_VERSION_NUMBER "BoringSSL"' >> /usr/local/include/openssl/opensslv.h \
    && echo '#endif' >> /usr/local/include/openssl/opensslv.h \
    && wget https://github.com/nghttp2/nghttp2/releases/download/v1.56.0/$NGHTTP2_VERSION.tar.bz2 \
    && tar xjf $NGHTTP2_VERSION.tar.bz2 && cd $NGHTTP2_VERSION \
    && ./configure --prefix=/usr/local --with-pic --enable-lib-only --disable-shared --disable-python-bindings \
    && make -j$(nproc) && make install && cd .. \
    && wget https://github.com/google/brotli/archive/refs/tags/v$BROTLI_VERSION.tar.gz -O brotli-$BROTLI_VERSION.tar.gz \
    && tar xzf brotli-$BROTLI_VERSION.tar.gz && cd brotli-$BROTLI_VERSION && mkdir out && cd out \
    && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib .. \
    && cmake --build . --config Release --target install && cd ../.. \
    && wget https://curl.se/download/curl-8.1.1.tar.xz && tar xf curl-8.1.1.tar.xz && cd curl-8.1.1 \
    && for p in ../chrome/patches/curl-*.patch; do patch -p1 < $p; done && autoreconf -fi \
    && LDFLAGS="-L/usr/local/lib" LIBS="-lpthread" ./configure --prefix=/usr/local --with-nghttp2=/usr/local --with-brotli=/usr/local --with-openssl=/root/curl-impersonate/boringssl/build --with-zlib --enable-websockets --enable-shared USE_CURL_SSLKEYLOGFILE=true \
    && make -j$(nproc) && make install \
    && ln -sf /usr/local/lib/libcurl-impersonate-chrome.so /usr/local/lib/libcurl.so \
    && ln -sf /usr/local/lib/libcurl-impersonate-chrome.so.4 /usr/local/lib/libcurl.so.4 \
    && ln -sf /usr/local/lib/libcurl-impersonate-chrome.a /usr/local/lib/libcurl.a \
    && ldconfig && cd ../.. && rm -rf curl-impersonate \
    && wget https://github.com/libevent/libevent/releases/download/release-$LIBEVENT_VERSION/libevent-$LIBEVENT_VERSION.tar.gz && tar xzf libevent-$LIBEVENT_VERSION.tar.gz \
    && cd libevent-$LIBEVENT_VERSION && mkdir build && cd build \
    && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DEVENT__LIBRARY_TYPE=BOTH -DOPENSSL_ROOT_DIR=/usr/local -DOPENSSL_INCLUDE_DIR=/usr/local/include -DOPENSSL_CRYPTO_LIBRARY=/usr/local/lib/libcrypto.so -DOPENSSL_SSL_LIBRARY=/usr/local/lib/libssl.so -DEVENT__DISABLE_REGRESS=ON .. \
    && make -j$(nproc) && make install && cd ../.. && rm -rf libevent-$LIBEVENT_VERSION* \
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
    && cd luarocks-$LUAROCKS_VERSION && make uninstall && cd .. && rm -rf luarocks* \
    && apk del linux-headers git lua5.3-dev g++ bison ncurses-dev libc-dev wget cmake make gcc unzip bsd-compat-headers perl go ninja autoconf automake libtool pkgconfig \
    && rm -rf /usr/include /usr/local/include /usr/local/share/doc /usr/local/share/man /usr/local/share/lua/5.3/luarocks /usr/local/mysql/lib/*.a /usr/local/mysql/include /usr/local/bin /var/cache/apk/* .cache
