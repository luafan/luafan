lua-lmd5:
	$(CC) -O2 -shared -DUSE_MD4_OPENSSL -o ../md4.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_MD5_OPENSSL -o ../md5.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_SHA1_OPENSSL -o ../sha1.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_SHA224_OPENSSL -o ../sha224.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_SHA256_OPENSSL -o ../sha256.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_SHA384_OPENSSL -o ../sha384.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_SHA512_OPENSSL -o ../sha512.so -fPIC ../lmd5.c -lcrypto
	$(CC) -O2 -shared -DUSE_RIPEMD160_OPENSSL -o ../ripemd160.so -fPIC ../lmd5.c -lcrypto

clean:
	rm *.o ../*.so
