## Quick Setup
* ubuntu 14.04 + luajit + luafanlite

```sh
cd
sudo apt-get update
sudo apt-get install -y wget lua5.1-dev lua5.1 luajit make gcc libc-dev libcurl4-openssl-dev libevent-dev git

# install luarocks if have not installed.
rm -rf luarocks-2.4.1.tar.gz
wget http://luarocks.org/releases/luarocks-2.4.1.tar.gz
tar xzf luarocks-2.4.1.tar.gz
cd luarocks-2.4.1
./configure
make build
sudo make install
cd
rm -rf luarocks-2.4.1*

sudo luarocks install luafanlite
```

* ubuntu 14.04 + luajit + luafan with mariadb

```sh
cd
apt-get update
apt-get install -y wget lua5.1-dev lua5.1 luajit make gcc libc-dev libcurl4-openssl-dev libevent-dev git libevent-2.0-5 libevent-core-2.0-5 libevent-extra-2.0-5 libevent-openssl-2.0-5 libcurl3 cmake g++ bison libncurses5-dev libssl-dev

# install luarocks if have not installed.
rm -rf luarocks-2.4.1.tar.gz
wget http://luarocks.org/releases/luarocks-2.4.1.tar.gz
tar xzf luarocks-2.4.1.tar.gz
cd luarocks-2.4.1
./configure
make build
make install
cd ..
rm -rf luarocks-2.4.1*

wget https://github.com/MariaDB/server/archive/mariadb-5.5.48.tar.gz
tar xzf mariadb-5.5.48.tar.gz
cd server-mariadb-5.5.48
cmake .
cd libmysql
make install
cd ../include
make install
cd
rm -rf mariadb-5.5.48.tar.gz server-mariadb-5.5.48
luarocks install luafan MARIADB_DIR=/usr/local/mysql
```

* Dockerfile
	* [luafan-alpine](https://github.com/luafan/luafan-alpine) `docker pull luafan/alpine`
	* [luafan-ubuntu](https://github.com/luafan/luafan-ubuntu) `docker pull luafan/ubuntu`
