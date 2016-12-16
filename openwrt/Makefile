luafan:
	$(CC) -O2 -DFAN_HAS_OPENSSL=0 -DFAN_HAS_LUAJIT=0 -DDISABLE_AFFINIY -shared -o ../fan.so -fPIC \
    ../src/hostcheck.c \
    ../src/fifo.c \
    ../src/bytearray.c \
    ../src/event_mgr.c \
    ../src/httpd.c \
    ../src/luafan.c \
    ../src/stream.c \
    ../src/tcpd.c \
    ../src/udpd.c \
    ../src/utlua.c \
    \
    -levent

clean:
	rm *.o fan.so

