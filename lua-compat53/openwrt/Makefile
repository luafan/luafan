lua-compat53:
	$(CC) -O2 -shared -o ../compat53/utf8.so -fPIC ../lutf8lib.c
	$(CC) -O2 -shared -o ../compat53/table.so -fPIC ../ltablib.c
	$(CC) -O2 -shared -o ../compat53/string.so -fPIC ../lstrlib.c

clean:
	rm *.o ../compat53/*.so
