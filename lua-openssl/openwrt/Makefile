lua-openssl:
	$(CC) -O2 -shared -DPTHREADS -I../deps -o ../openssl.so -fPIC \
	../src/asn1.c \
	../src/auxiliar.c \
	../src/bio.c \
	../src/cipher.c \
	../src/cms.c \
	../src/compat.c \
	../src/crl.c \
	../src/csr.c \
	../src/dh.c \
	../src/digest.c \
	../src/dsa.c \
	../src/ec.c \
	../src/engine.c \
	../src/hmac.c \
	../src/lbn.c \
	../src/lhash.c \
	../src/misc.c \
	../src/ocsp.c \
	../src/openssl.c \
	../src/ots.c \
	../src/pkcs12.c \
	../src/pkcs7.c \
	../src/pkey.c \
	../src/rsa.c \
	../src/ssl.c \
	../src/th-lock.c \
	../src/util.c \
	../src/x509.c \
	../src/xattrs.c \
	../src/xexts.c \
	../src/xname.c \
	../src/xstore.c \
	../src/xalgor.c \
	../src/callback.c \
	-lssl -lcrypto

clean:
	rm *.o ../openssl.so
