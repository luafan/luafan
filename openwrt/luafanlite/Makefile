luafan:
	$(CC) -O2 -DFAN_HAS_OPENSSL=1 -DFAN_HAS_LUAJIT=1 -DDISABLE_AFFINIY -shared -o ../../fan.so -fPIC \
    ../../src/hostcheck.c \
    ../../src/fifo.c \
    ../../src/bytearray.c \
    ../../src/event_mgr.c \
    ../../src/http.c \
    ../../src/httpd.c \
    ../../src/luafan.c \
    ../../src/stream.c \
    ../../src/tcpd.c \
    ../../src/udpd.c \
    ../../src/utlua.c \
    ../../src/luafan_posix.c \
    ../../src/objectbuf.c \
    \
    -levent -levent_openssl -lssl -lcrypto -lcurl

clean:
	rm *.o ../../fan.so
