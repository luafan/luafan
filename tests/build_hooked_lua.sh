#!/bin/sh
# build_hooked_lua.sh - build the Lua interpreter WITH the global lua_lock hook.
#
# WHY
# ---
# luafan can run its libevent loop on several worker threads that share ONE
# lua_State (fan.workers_init(n), n > 0). A stock Lua core compiles
# lua_lock/lua_unlock to nothing (lua53/llimits.h), so nothing serialises those
# threads. Building the interpreter with luafan's hook makes the CORE own the
# lock -- exactly what LuanMac does on Apple (-DLUA_USER_H="<luauser.h>") and
# what the release images do.
#
# A "hooked" layout has three parts, all required:
#   1. fan_lua_lock.c added to CORE_O, so the lock implementation lives in the
#      interpreter and NOT in fan.so (two copies would mean two recursive mutexes
#      and two thread-local depth counters -- see luafan/src/fan_lua_lock.h),
#   2. -include fan_lua_lock.h so every core translation unit maps lua_lock to
#      it, plus -DFAN_LUA_LOCK_CORE=1 so LuaCoreLockHooked() reports "hooked",
#   3. -Wl,-E so a dlopen'd fan.so can resolve LockMainState/LuaGlobalLock.
#
# A hooked interpreter also means luafan does NOT install its resume wrapper:
# every lua_resume() already holds the mutex, and an extra level taken outside
# it would suppress the core's cooperative yield points. See
# luafan/src/event_mgr.c install_locking_resume() and docs/threading-model.md.
#
# USAGE
#   tests/build_hooked_lua.sh [lua_version] [luafan_src_dir] [prefix]
#     lua_version     default $LUA_VERSION or 5.3.3
#     luafan_src_dir  default <repo>/src (must contain fan_lua_lock.{c,h})
#     prefix          default $PREFIX or /usr/local
#
# The script fails loudly if the resulting interpreter does not export the lock
# symbols, because such a build only breaks later, at dlopen time.

set -e

here=$(cd "$(dirname "$0")" && pwd)
LUA_VERSION="${1:-${LUA_VERSION:-5.3.3}}"
LUAFAN_SRC="${2:-${LUAFAN_SRC:-$(dirname "$here")/src}}"
PREFIX="${3:-${PREFIX:-/usr/local}}"

for f in fan_lua_lock.c fan_lua_lock.h; do
    if [ ! -f "$LUAFAN_SRC/$f" ]; then
        echo "build_hooked_lua: $LUAFAN_SRC/$f not found" >&2
        exit 1
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

cd "$work"
echo "build_hooked_lua: fetching lua-$LUA_VERSION"
wget -q "https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz"
tar xzf "lua-$LUA_VERSION.tar.gz"
cd "lua-$LUA_VERSION"

cp "$LUAFAN_SRC/fan_lua_lock.c" "$LUAFAN_SRC/fan_lua_lock.h" src/
# The lock implementation must be part of the core objects.
sed -i 's/^CORE_O=/CORE_O= fan_lua_lock.o /' src/Makefile
# No interactive REPL in images or CI: drop readline from the linux target.
sed -i 's/ -lreadline//' src/Makefile
sed -i 's|^#define LUA_USE_READLINE|/* readline disabled */|' src/luaconf.h

make linux \
    MYCFLAGS="-fPIC -pthread -DFAN_LUA_LOCK_CORE=1 -include fan_lua_lock.h" \
    MYLDFLAGS="-Wl,-E" \
    MYLIBS="-pthread"
make install INSTALL_TOP="$PREFIX"
cp src/luaconf.h "$PREFIX/include/"

for sym in LockMainState LuaCoreLockHooked; do
    if ! ( (nm -D "$PREFIX/bin/lua" 2>/dev/null || nm "$PREFIX/bin/lua") | grep -q "T $sym" ); then
        echo "build_hooked_lua: $PREFIX/bin/lua does not export $sym" >&2
        echo "build_hooked_lua: check -Wl,-E (MYLDFLAGS) and that fan_lua_lock.o is in CORE_O" >&2
        exit 1
    fi
done

echo "build_hooked_lua: installed $("$PREFIX/bin/lua" -v 2>&1 | head -1) at $PREFIX/bin/lua"
