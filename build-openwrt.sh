STAGING_DIR=/Volumes/OpenWRT_1407/openwrt/staging_dir

TOOLCHAIN=toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2
TARGET=target-mips_34kc_uClibc-0.9.33.2
SRC=./src

PATH=$PATH:$STAGING_DIR/$TOOLCHAIN/bin

CC=mips-openwrt-linux-uclibc-gcc
LD=mips-openwrt-linux-uclibc-ld
STRIP=mips-openwrt-linux-uclibc-strip

CFLAGS=-I$STAGING_DIR/$TARGET/usr/include
LDFLAGS=-L$STAGING_DIR/$TARGET/usr/lib

# $CC -O2 $CFLAGS $LDFLAGS -shared -o fan.so -fPIC \
#     $SRC/hostcheck.c \
#     $SRC/fifo.c \
#     $SRC/bytearray.c \
#     $SRC/event_mgr.c \
#     $SRC/http.c \
#     $SRC/httpd.c \
#     $SRC/luafan.c \
#     $SRC/openssl_hostname_validation.c \
#     $SRC/stream.c \
#     $SRC/tcpd.c \
#     $SRC/udpd.c \
#     $SRC/utlua.c \
#     \
#     -lcurl -lpolarssl -levent -lcrypto -lssl -levent_openssl -lresolv

# $CC -O2 $CFLAGS $LDFLAGS -o hello -fPIC \
#     $SRC/hostcheck.c \
#     $SRC/bytearray.c \
#     $SRC/event_mgr.c \
#     $SRC/luafan.c \
#     $SRC/tcpd.c \
#     $SRC/utlua.c \
#     $SRC/openssl_hostname_validation.c \
#     hello.c\
#     \
#     -levent -llua -lm -ldl -lssl -levent_openssl -lcrypto

$CC -O2 $CFLAGS $LDFLAGS -DFAN_HAS_OPENSSL=0 -DFAN_HAS_LUAJIT=0 -shared -o fan.so -fPIC \
    $SRC/hostcheck.c \
    $SRC/fifo.c \
    $SRC/bytearray.c \
    $SRC/event_mgr.c \
    $SRC/luafan.c \
    $SRC/stream.c \
    $SRC/tcpd.c \
    $SRC/udpd.c \
    $SRC/utlua.c \
    \
    -levent

$STRIP -s fan.so
