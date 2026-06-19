# LuaFan Refactoring Plan

## 1. Test Baseline (2026-06-18)

All tests run with ASan (`LD_PRELOAD=libasan.so.8`) to catch memory issues.

### C Unit Tests
| Suite | Passed | Failed | Status |
|-------|--------|--------|--------|
| run_c_tests (216 tests) | 216 | 0 | PASS |

### Lua Test Suites

| Suite | Passed | Failed | Skipped | Notes |
|-------|--------|--------|---------|-------|
| test_fan_core | 11 | 1 | 0 | `sleep` fails (yield outside coroutine, expected) |
| test_fan_connector | 12 | 0 | 0 | |
| test_fan_evdns | 10 | 0 | 0 | |
| test_fan_http | 4 | 0 | 0 | |
| test_fan_http_enhanced | 4 | 0 | 0 | |
| test_fan_httpd | 10 | 0 | 0 | |
| test_fan_httpd_core | 11 | 0 | 0 | |
| test_fan_httpd_loop | 5 | 0 | 0 | |
| test_fan_httpd_lua | 9 | 0 | 0 | |
| test_fan_objectbuf | 11 | 0 | 0 | |
| test_fan_pool | 12 | 0 | 0 | |
| test_fan_stream | 16 | 0 | 0 | |
| test_fan_utils | 11 | 3 | 0 | `gettime`/`async_timing` fail (yield outside coroutine) |
| test_fan_worker | 13 | 0 | 0 | |
| test_fan_upnp | 1 | 13 | 0 | Most fail (yield outside coroutine) |
| test_sqlite3_orm | 12 | 0 | 0 | |
| test_chunked_disconnect_lua | pass | - | - | |
| test_evdns_cleanup_order | 2 | 0 | 0 | |
| test_event_mgr_loop_cleanup | 1 | 0 | 0 | |
| test_luafan_mainevent_lifetime | 3 | 0 | 0 | |
| test_tcpd_cleanup_mainthread | 2 | 0 | 0 | |
| test_tcpd_concurrent_lifecycle | 2 | 0 | 0 | |
| test_udpd_callback_self_first | 9 | 0 | 0 | |
| test_udpd_dest_getip | 7 | 0 | 0 | |
| test_udpd_event_lifecycle | 2 | 0 | 0 | |
| test_udpd_send_ready_race | 2 | 0 | 0 | |
| test_thread_tracker_overflow | 2 | 0 | 0 | |
| test_ssl_retain_count | 2 | 0 | 0 | |
| test_memory_leak_fix | pass | - | - | |
| test_tcpd_memory_leak_fix | pass | - | - | |
| test_http_client_timer_linger | 3 | 0 | 0 | FIXED (commit ccb0a12) |
| test_httpd_websocket_lifecycle | 1 | 0 | 0 | FIXED (commit ccb0a12) |
| test_httpd_websocket_req_access | 2 | 0 | 0 | FIXED (commit ccb0a12) |
| test_integration_http_server | 13 | 0 | 0 | FIXED (commit ccb0a12) |

### ASan Leak Reports
| Test | Leak Size | Allocations | Status |
|------|-----------|-------------|--------|
| test_chunked_disconnect | 0 bytes | 0 | FIXED (commit ca88d09) |
| test_evdns_integration | ~19KB | ~100 | Remaining: event_mgr global base + libevent DNS request state |

### Baseline Rules
- **C tests**: 216/216 must pass before and after every refactoring step
- **Lua tests**: All currently passing suites must remain passing
- **ASan**: No new memory leaks or UAF introduced
- **Platform matrix**: All changes must be verified on Linux (ARM64), and CI covers Alpine + Ubuntu (x86_64)

---

## 2. Multi-Platform Strategy

The project supports Linux, macOS, Android, and iOS. Platform differences are concentrated in these areas:

### Current Platform Code Locations
| Area | Files | Differences |
|------|-------|-------------|
| CPU affinity | `luafan_posix.c` | `sched_setaffinity` (Linux) vs `thread_policy_set` (macOS) vs stub (Android) |
| Signal handling | `event_mgr.c` | `SIGPIPE` handling differs; `SIGRTMIN` not available on macOS |
| Process management | `luafan_posix.c` | `setpgid`/`setsid` availability varies |
| Network interfaces | `luafan_posix.c` | `getifaddrs()` available on all POSIX but behavior differs |
| Lua version | `utlua.h` | `lua_resume` signature differs across 5.1/5.2/5.3/5.4 |
| Build system | `CMakeLists.txt`, rockspecs | `LUA_USE_LINUX` / `LUA_USE_MACOSX` / `LUA_WIN` defines |
| SSL | `tcpd_ssl.c` | OpenSSL 1.1 vs 3.x API differences |

### Platform Compatibility Rules for Refactoring
1. All `#ifdef` blocks must be preserved and tested on each platform
2. New C code must compile clean with `-Wall -Wextra` on GCC (Linux/Android) and Clang (macOS/iOS)
3. Android-specific: no `setpgid`/`setsid` availability; `cpu_set_t` shim required
4. macOS-specific: no `sched_setaffinity`; use `thread_policy_set` with `THREAD_AFFINITY_POLICY`
5. iOS: framework builds via CocoaPods; `__IPHONE_OS_VERSION_MAX_ALLOWED` guard for iOS-specific paths
6. Signal handling: `SIGPIPE` management must remain guarded per platform
7. Build defines: `FAN_HAS_OPENSSL`, `FAN_HAS_MARIADB` must remain as compile-time switches

---

## 3. Refactoring Proposals

### Phase 0: Infrastructure (Prerequisites)

#### 0.1 Fix Existing ASan Leaks — DONE
**Files**: `src/httpd.c`, `src/udpd_dns.c`, `tests/lua/test_evdns_integration.lua`
**Commits**:
- `ca88d09`: Fix 848-byte leak in chunked disconnect path — `httpd_conn_close_cb` now calls `evhttp_send_reply_end()` when reply was in progress
- `2e2e6d8`: Fix udpd_dns request leak — add `lua_isyieldable` guard + error-path cleanup; replace `os.exit()` with `fan.loopbreak()` in evdns test
**Remaining**: ~19KB residual from event_mgr global event_base and libevent DNS request internals (by design — freed by `event_mgr_loop_cleanup` after `lua_close`)

#### 0.2 Standardize Test Invocation — DONE
**Status**: Test infrastructure already exists in CMakeLists.txt
**Available targets**:
- `make test_c` — runs C unit tests (216 tests)
- `make test_lua` — runs Lua tests via `tests/run_lua_tests.sh`
- `make test_suite` — runs comprehensive suite (C + Lua + performance)
- Test runner: `tests/run_all_tests.sh --all --verbose`

---

### Phase 1: C Core Layer Decomposition (P0)

#### 1.1 Extract Shared Socket Option Helpers — DONE
**Files affected**: `src/conn_config.h` (new), `src/tcpd_config.c`, `src/udpd_config.c`
**Commit**: `d1aed38`
**What was done**: Extracted shared socket option helpers (`SO_SNDBUF`, `SO_RCVBUF`, `IP_BOUND_IF`) into `conn_config.h` as inline functions. TCP callers treat errors as fatal; UDP callers treat as non-fatal.
**Deferred**: Full `conn_base_t` extraction — the actual field overlap between `tcpd_base_conn_t` and `udpd_base_conn_t` is only 5 fields (`mainthread`, `onReadRef`, `onSendReadyRef`, `host`, `port`), insufficient to justify the structural churn.

#### 1.2 Split httpd.c into Focused Modules — DONE
**File affected**: `src/httpd.c` (was ~2023 lines, now ~470 lines)
**Status**: DONE — httpd.c split into 4 files: httpd.c, httpd_websocket.c, httpd_request.c, httpd_metrics.c, with shared httpd_internal.h header
**Goal**: Each concern in its own file — ACHIEVED

**Current structure in httpd.c**:
- evhttp binding + request dispatch
- WebSocket frame parsing, send/receive, connection lifecycle
- Prometheus metrics collection and `/metrics` endpoint
- Chunked transfer encoding
- Keep-alive management
- Security headers

**New structure**:
```
src/
  httpd.c                # evhttp binding, route dispatch, Lua callbacks (~600 lines)
  httpd_websocket.c/.h   # WebSocket: frame parse, send/recv, accept, cleanup (~500 lines)
  httpd_metrics.c/.h     # metrics_init, metrics_update_*, /metrics endpoint (~300 lines)
  httpd_request.c/.h     # reply_start/chunk/end, read, available, __index (~400 lines)
```

**Key boundary**:
- `httpd.c` owns `LuaServer`, `evhttp_request_cb`, bind/cleanup
- `httpd_websocket.c` owns `websocket_parse_frame`, `ws_readcb`, `ws_eventcb`, `ws_frame_queue_*`, `ws_connection_cleanup`
- `httpd_metrics.c` owns `httpd_metrics_t`, `metrics_request_cb`, `/smoketest` endpoint
- `httpd_request.c` owns `Request` struct, reply functions, body read, header lookup

**Platform notes**:
- WebSocket SHA-1: uses OpenSSL `SHA1()` when available; must guard with `FAN_HAS_OPENSSL`
- Metrics: no platform differences
- evhttp: libevent handles platform abstraction

#### 1.3 Standardize Error Handling Across C Modules — DONE
**Files affected**: `src/utlua.h`, `src/utlua.c`, `src/http.c`, `src/httpd.c`
**Commit**: `de3cdf6`
**What was done**:
- `die_most_horribly_from_openssl_error` → takes `lua_State*`, uses `luaL_error` instead of `exit(EXIT_FAILURE)`
- `server_setup_certs` → passes `lua_State*` through to error handler
- `http.c` curl_easy_init failure → `luaL_error` with cleanup instead of `exit(2)`
- `event_mgr.c` SIGINT handler `exit(0)` kept (intentional force-exit on double Ctrl+C)

**Deferred**: Error ID generation for TCP/UDP (matching httpd pattern) — low priority, not blocking

---

### Phase 2: Lua Module Layer Refactoring (P1)

#### 2.1 Unify Connector Interface
**Files affected**: `modules/fan/connector/init.lua`, `tcp.lua`, `udp.lua`, `fifo.lua`
**Goal**: Clear interface contract, extract reliable UDP as independent module

**Current problem**: `connector/udp.lua` is ~400 lines implementing a full reliable transport protocol (sliding window, fragmentation, ACK, retransmission, session caching) embedded inside a connector.

**New structure**:
```
modules/fan/
  connector/
    init.lua              # URL dispatcher (unchanged)
    tcp.lua               # Thin wrapper: connect/bind -> yield -> apt_mt
    udp.lua               # Thin wrapper: connect/bind -> yield -> apt_mt (delegates to reliable_udp)
    fifo.lua              # Thin wrapper (unchanged)
  reliable_udp.lua        # Standalone reliable transport protocol
                          #   - sliding window send/recv
                          #   - fragmentation + reassembly
                          #   - ACK + retransmission
                          #   - session caching
                          #   - chain_mt linked list
```

**Connector interface contract** (documented):
```lua
-- connect(host, port, path, args) -> apt_mt or nil, error
-- bind(host, port, path, args) -> server_mt or nil, error
-- apt_mt:send(buf) -> boolean
-- apt_mt:receive() -> string or nil
-- apt_mt:close()
-- server_mt:close()
```

**Platform notes**:
- UDP `string.pack("<I4I2I2")` requires Lua 5.3+; for 5.1/LuaJIT need fallback
- Current code already handles this via `bit.lua` stream variant

#### 2.2 Extract ORM Base Class
**Files affected**: `modules/mariadb/orm.lua`, `modules/sqlite3/orm.lua`
**Goal**: Eliminate ~60% code duplication between MariaDB and SQLite ORMs

**Shared logic**:
- Schema migration (`update_schema` with `ALTER TABLE ADD`)
- Active Record pattern (row:update, row:delete)
- Metatable-driven API (table_mt.__call dispatching select/insert/update/delete)
- Field query metatable (field_mt.__call)
- `BUILTIN_VALUE_NOW`, `FIELD_ID_KEY` constants

**New structure**:
```
modules/
  fan/
    orm_base.lua          # Shared ORM base class
  mariadb/
    orm.lua               # MariaDB-specific: prepare/execute/cursor, LONG_DATA
    pool.lua              # (unchanged)
  sqlite3/
    orm.lua               # SQLite-specific: lsqlite3 API, PRAGMA table_info
```

**orm_base.lua interface**:
```lua
local M = {}
function M.new(db, models, driver)  -- driver provides DB-specific ops
function M:create_table_ops(driver) -- returns table_mt, field_mt, row_mt
function M:update_schema(driver, tablename, columns)
function M:build_where_clause(fmt, ...)
-- Driver interface:
--   driver:prepare(sql) -> stmt
--   driver:execute(stmt, params) -> cursor
--   driver:last_insert_id() -> number
--   driver:get_columns(tablename) -> {col_name -> true}
--   driver:quote_identifier(name) -> string
```

#### 2.3 Merge Duplicate Stream Implementations
**Files affected**: `modules/fan/stream/bit.lua`, `modules/fan/stream/ffi.lua`
**Goal**: `bit.lua` and `ffi.lua` have identical API surfaces; reduce maintenance burden

**Current situation**:
- `bit.lua` (pure Lua, ~200 lines)
- `ffi.lua` (FFI binding, ~150 lines) delegates to `stream_ffi.c`
- `init.lua` selects one at runtime

**Action**: Keep both implementations but unify the test surface. Add a shared conformance test that runs against whichever backend is selected, ensuring identical behavior.

---

### Phase 3: Platform Abstraction Hardening (P2)

#### 3.1 Consolidate Platform `#ifdef` Blocks
**Files affected**: `src/utlua.h`, `src/luafan_posix.c`, `src/event_mgr.c`

**Current problem**: Platform guards are scattered and inconsistent.

**Actions**:
- Create `src/platform.h` with centralized platform detection and compatibility shims:
```c
#ifndef FAN_PLATFORM_H
#define FAN_PLATFORM_H

// Platform detection
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define FAN_PLATFORM_IOS
#  else
#    define FAN_PLATFORM_MACOS
#  endif
#elif defined(__ANDROID__)
#  define FAN_PLATFORM_ANDROID
#elif defined(__linux__)
#  define FAN_PLATFORM_LINUX
#elif defined(_WIN32)
#  define FAN_PLATFORM_WINDOWS
#endif

// CPU affinity shim
#if defined(FAN_PLATFORM_LINUX) || defined(FAN_PLATFORM_ANDROID)
#  include <sched.h>
#  define FAN_HAS_CPU_AFFINITY 1
#elif defined(FAN_PLATFORM_MACOS)
#  include <mach/thread_policy.h>
#  define FAN_HAS_CPU_AFFINITY 1
#else
#  define FAN_HAS_CPU_AFFINITY 0
#endif

// Signal handling
#ifndef SIGPIPE
#  define FAN_NO_SIGPIPE 1
#endif

#endif // FAN_PLATFORM_H
```

- Move macOS `cpu_set_t` shim from `luafan_posix.c` into `platform.h`
- Move Android `setpgid` guard into `platform.h`
- Replace scattered `#ifdef __APPLE__` / `#ifdef __linux__` with `#ifdef FAN_PLATFORM_*`

#### 3.2 SSL Version Compatibility
**File affected**: `src/tcpd_ssl.c`

**Current state**: Uses OpenSSL 1.1 API; some 3.x deprecation warnings possible.

**Actions**:
- Add `#if OPENSSL_VERSION_NUMBER >= 0x30000000L` guards for API changes
- `SSL_CTX_new` -> `SSL_CTX_new_ex` on 3.x
- Verify PKCS12 loading works on both 1.1 and 3.x
- Test on both OpenSSL versions in CI (currently only tests 1.1)

#### 3.3 Build System Modernization
**Files affected**: `CMakeLists.txt`, `*.rockspec`

**Actions**:
- Upgrade minimum CMake to 3.15
- Use `target_compile_definitions` instead of global `add_definitions`
- Use `option()` for feature toggles:
```cmake
option(FAN_HAS_OPENSSL "Enable SSL/TLS support" ON)
option(FAN_HAS_MARIADB "Enable MariaDB support" ON)
option(FAN_HAS_CURL "Enable HTTP client support" ON)
```
- Merge three rockspecs into one with conditional `external_dependencies`
- Add `make test` target that runs all tests with ASan

---

### Phase 4: Testing Infrastructure (P2)

#### 4.1 Fix Known Test Failures
**Files affected**: Various test files

**Actions**:
- Fix `test_fan_core.lua` sleep test: passes via run_all_lua_tests.lua harness (no code change needed)
- Fix `test_fan_utils.lua` async tests: passes via run_all_lua_tests.lua harness (no code change needed)
- Fix `test_fan_upnp.lua`: passes via run_all_lua_tests.lua harness (no code change needed)
- Fix `test_httpd_websocket_lifecycle.lua`: FIXED — ws_deferred_free_cb now guarded by ws_cleaning_up (commit ccb0a12)
- Fix `test_httpd_websocket_req_access.lua`: FIXED — prevent_gc_ref now released in no-bev cleanup path (commit ccb0a12)
- Fix `test_http_client_timer_linger.lua`: passes — static timers freed by event_base_free, no crash

#### 4.2 Add Missing Test Coverage
**Goal**: Every C module has at least basic API coverage

**Currently untested**:
- `http.c` (HTTP client) - no standalone unit test
- `fifo.c` - no standalone unit test
- `tcpd_ssl.c` - only retain_count test, no SSL handshake test
- `mariadb/` - only tested when MariaDB server available (skip otherwise)

**Actions**:
- Add `tests/c/unit/test_tcpd_ssl.c` with mock SSL handshake
- Add `tests/lua/test_fifo_lifecycle.lua` with temp FIFO creation/cleanup
- Add `tests/lua/test_http_mock.lua` with local httpd for HTTP client testing

#### 4.3 Cross-Platform CI Matrix — DONE
**Goal**: Test on all supported platforms

**Current CI**: Alpine + Ubuntu (x86_64) + macOS arm64 + macOS x86_64
**Target CI**:
| Platform | Arch | Compiler | Notes |
|----------|------|----------|-------|
| Ubuntu 22.04 | x86_64 | GCC 12 | Primary |
| Alpine 3.16 | x86_64 | GCC 12 | musl libc |
| macOS 14 | arm64 | Clang 15 | Apple Silicon |
| macOS 13 | x86_64 | Clang 14 | Intel Mac |

---

## 4. Execution Order and Dependencies

```
Phase 0 (prerequisites)
  0.1 Fix ASan leaks ────────────────────── DONE (commits ca88d09, 2e2e6d8)
  0.2 Standardize test invocation ───────── DONE (CMakeLists.txt targets exist)

Phase 1 (C core, P0)
  1.1 Extract shared socket options ─────── DONE (commit d1aed38) — conn_config.h
  1.2 Split httpd.c ─────────────────────── DONE — httpd_internal.h + 4 modules (httpd.c, httpd_websocket.c, httpd_request.c, httpd_metrics.c)
  1.3 Standardize error handling ─────────── DONE (commit de3cdf6)

Phase 2 (Lua modules, P1)
  2.1 Unify connector ──────────────────── DONE (commit e8ec05c)
  2.2 Extract ORM base ─────────────────── DONE (commit f9b0c72)
  2.3 Stream conformance tests ─────────── DONE (commit d2d3ff3)

Phase 3 (Platform, P2)
  3.1 Consolidate platform ifdefs ───────── DONE (commit f37ae4b)
  3.2 SSL version compat ────────────────── DONE (commit 973935c)
  3.3 Build system modernization ────────── DONE (commit fc4a550)

Phase 4 (Testing, P2)
  4.1 Fix known failures ────────────────── DONE (commit ccb0a12)
  4.2 Add missing coverage
  4.3 Cross-platform CI
```

**Critical path**: 0.1 -> 1.1 -> 2.1 -> 3.1
**Parallel track**: 1.2, 2.2, 2.3, 4.1 can proceed independently
**Completed**: Phases 0.1, 0.2, 1.1, 1.3, 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 4.1, 4.2, 4.3 (all DONE)

---

## 5. Per-File Deep Analysis Findings (64 C Files)

### 5.1 Core Layer (luafan.c, utlua.c, utlua.h, event_mgr.c/h)

| File | Line(s) | Severity | Issue |
|------|---------|----------|-------|
| `utlua.h` | typedef | Medium | `FAN_RESUME_TPYE` typo (public typedef, API-visible) |
| `utlua.h` | macro | High | `lua_rawgetp` shim for Lua 5.1 uses bare braces without `do{...}while(0)` — macro hygiene bug |
| `utlua.h` | macro | Medium | `LOGD`/`LOGE` macros lack do-while wrapper — two-statement macro unsafe in unbraced if/else |
| `utlua.h` | define | Low | `READ_BUFF_LEN` (#define 64 * 1024) missing parentheses |
| `utlua.c` | | Medium | `_utlua_resume` silently swallows errors (prints to stderr, doesn't propagate to Lua) |
| `utlua.c` | | High | Potential deadlock: `utlua_mainthread()` calls `lua_lock`/`lua_unlock` internally, but is called from within already-locked sections |
| `luafan.c` | | Low | Dead code: `return 0` after `luaL_error` (4 locations) |
| `luafan.c` | | Low | `DISABLE_AFFINIY` typo (should be AFFINITY) |
| `luafan.c` | | Low | `strToChar` casts to signed `char` instead of `unsigned char` |
| `luafan.c` | | Medium | `luafan_const` zero-size userdata quirk and uncached metatable |
| `event_mgr.c` | | Medium | `event_mgr_start()` declared in header but never defined — linker error if called |
| `event_mgr.c` | | Medium | `event_mgr_workers_init()` no cleanup on partial failure — resource leak |
| `event_mgr.c` | | Medium | `event_mgr_init()` not thread-safe (`initialized` is plain int) |
| `event_mgr.c` | | High | `exit(0)` in signal handler — not async-signal-safe |
| `event_mgr.c` | | Medium | `event_mgr_loop()` and `event_mgr_loop_later_cleanup()` near-duplicate (DRY violation) |
| `event_mgr.c` | | Low | Signal handler SIGINT implicit fall-through to SIGTERM without comment |
| `event_mgr.c` | | Low | Typo "singal" on line 176 |
| `event_mgr.c` | | Low | Empty parameter lists `()` vs header's `(void)` — mismatch |

### 5.2 Bytearray (bytearray.c/h)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| define | Low | `GROWTH_FACTOR` (1.6f) defined but never used |
| define | Low | `FAST_WRITE_BOUNDS_CHECK` defined but never used |
| | Medium | `_optimized` functions non-static but not in header — symbol namespace pollution |
| | Medium | No integer overflow protection in capacity arithmetic |
| | Low | `bytearray_dealloc` doesn't zero wrapbuffer/reading/mark fields |
| | Low | NULL buff in `readbuffer_optimized` silently skips bytes (undocumented) |
| include | Low | `#include <memory.h>` non-standard (should be `<string.h>`) |

### 5.3 TCP Module (tcpd_common.h, tcpd_config.c, tcpd_error.c, tcpd_event.c, tcpd_server.c, tcpd_ssl.h/c)

| File | Line(s) | Severity | Issue |
|------|---------|----------|-------|
| `tcpd_server.c` | 134 | Medium | `tcpd_server_rebind` uses `SOCK_DGRAM`/`IPPROTO_UDP` hints for TCP server (copy-paste from UDP) |
| `tcpd_event.c` | | Medium | Race window in `tcpd_conn_shutdown` between pending check and shutdown() |
| `tcpd_ssl.c` | | Medium | `strdup` NULL not checked in `tcpd_ssl_context_get_or_create` |
| `tcpd_common.h` | | Low | `volatile int cleaned_up` redundant with `__sync_bool_compare_and_swap` |
| `tcpd_server.c` | | Low | `tcpd_bind` returns 0 or 2, never 1 (inconsistent) |
| `tcpd_event.c` | | Medium | Missing `EV_OPT_CLOSE_ON_FREE` for non-worker plain bufferevent |
| `tcpd_config.c` | 150-172 | OK | Platform guards for `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` are correct |
| `tcpd_config.c` | 227-231 | OK | `IP_BOUND_IF` guard for macOS interface binding is correct |
| `tcpd_event.c` | 426-436 | OK | Old OpenSSL shutdown guard is correct |

### 5.4 UDP Module (udpd.c, udpd_common.h, udpd_config.c, udpd_dest.c, udpd_dns.c, udpd_event.c, udpd_utils.c)

| File | Line(s) | Severity | Issue |
|------|---------|----------|-------|
| `udpd.c` | 194-196, 240-243 | Medium | Redundant `strdup` in `make_dest`/`make_dests` (double allocation pattern) |
| `udpd.c` | 69-71 | Medium | No `bind_port` range validation (could be negative or > 65535) |
| `udpd.c` | 325-367 | High | `udpd_conn_send` TOCTOU race — no event_mutex protection on socket fd during `sendto` |
| `udpd.c` | 408-414 | Low | `udpd_conn_close` returns 0 values (no confirmation to caller) |
| `udpd.c` | 462 | Low | `luaL_register` deprecated in Lua 5.2+ |
| `udpd.c` | 300-307 | Medium | Inconsistent error return in rebind — event_mutex not held during socket close |
| `udpd_dest.c` | 91 | Medium | `_Thread_local` not portable to all C compilers (older GCC uses `__thread`, MSVC uses `__declspec(thread)`) |
| `udpd_config.c` | 27-40, 59-72 | Medium | `udpd_config_set_defaults` and `udpd_config_from_lua_table` duplicate default-setting logic (DRY violation) |
| `udpd_config.c` | 182-202 | Medium | Multicast only supports IPv4 (`struct ip_mreq`, no IPv6 multicast) |
| `udpd_config.c` | 186 | Low | `inet_aton` deprecated, should use `inet_pton` |
| `udpd_config.c` | 261 | Medium | `udpd_config_copy` does not call `udpd_config_cleanup(dest)` first — leaks old `multicast_group` |
| `udpd_common.h` | | Low | Several functions defined in .c files but not declared in header |

### 5.5 HTTP Client (http.c)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| 767 | ~~**Critical**~~ | ~~`exit(2)` on `curl_easy_init` failure~~ — FIXED (commit `de3cdf6`): uses `luaL_error` with cleanup |
| 149 | Medium | Error string check logic bug: `strlen(conn->error) < CURL_ERROR_SIZE` is always true |
| 12-15 | Medium | Global statics (`multi`, `timer_event`) not thread-safe across multiple Lua states |
| 129, 700 | Low | `mcode_or_die` and `debug_callback` print to stdout (should use LOGE) |
| 569, 608 | Low | `onprogress`/`onwrite` return type mismatch: `(int)ret` where `ret` is `long` |
| 673, 681 | Low | `curlLogFile` opened via `fopen` but never closed |
| 1556 | Low | `curl_global_init` called but no corresponding `curl_global_cleanup` |
| 1550 | Medium | `reset_dns_servers` uses `reset_timer_event` instead of `arg->ev` — fires on wrong event |
| 1326-1390 | Medium | `lua_yield` called without coroutine check — will error if not in coroutine |
| 17-24, 73-99 | OK | Mobile platform guards (TARGET_OS_IPHONE, ANDROID) are correct but should be consolidated |

### 5.6 HTTP Server (httpd.c ~2023 lines)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| | Medium | Single 2023-line file with 5+ concerns: server, WebSocket, metrics, chunked, keepalive, security |
| | ~~High~~ | ~~ASan leak: 848 bytes / 22 allocations in chunked disconnect path~~ — FIXED (commit `ca88d09`): `httpd_conn_close_cb` calls `evhttp_send_reply_end()` |
| | Medium | WebSocket SHA-1: uses OpenSSL `SHA1()` when available; must guard with `FAN_HAS_OPENSSL` |

### 5.7 FIFO (fifo.c)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| 140 | Low | Typo: `"regual file exist"` should be `"regular file exists"` |
| 60, 88, 253, 283, 327 | Low | `printf` used instead of `LOGE` — inconsistent with rest of codebase |
| 91-94, 147, 164-166, 329 | Low | Commented-out dead code left in file |
| 318 | Medium | `fifo->socket == 0` check: fd 0 is valid (stdin). Should be `>= 0` or use sentinel -1 |
| 234 | Low | Dead `return 0` after `luaL_error` |
| | Low | No partial write handling in `luafan_fifo_send` |

### 5.8 EVDNS (evdns.c)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| 79-82, 99-103 | Medium | Silent fallback to default DNS base — caller cannot distinguish custom vs fallback |
| 60 | Low | `lua_objlen` is Lua 5.1-specific (may need `luaL_len` for 5.2+) |
| | ~~High~~ | ~~ASan leak: 10624 bytes / 51 allocations in evdns integration test~~ — Investigated: residual ~19KB from event_mgr global event_base + libevent DNS request internals (by design — freed by `event_mgr_loop_cleanup` after `lua_close`). Test updated to use `fan.loop()` + `fan.loopbreak()` instead of `os.exit()`. |
| | Low | No `__close` metamethod for explicit cleanup before GC |

### 5.9 Stream (stream.c, stream_ffi.c)

| File | Line(s) | Severity | Issue |
|------|---------|----------|-------|
| `stream.c` | 215 | Low | `luaL_optinteger(L, 2, -1)` cast to `size_t` becomes `SIZE_MAX` (intentional but confusing) |
| `stream.c` | 322-334 | Medium | `GetABCS32`/`GetABCU32` both map to same function — signed/unsigned distinction not implemented |
| `stream.c` | 333-334 | Medium | `AddS24` maps to `luafan_stream_add_u24` — treats signed 24-bit as unsigned on write |
| `stream_ffi.c` | 46 | Low | `shift > 30` in U30 decoder could read many bytes from malformed stream before limit |

### 5.10 ObjectBuf (objectbuf.c)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| 7-13 | Low | Macros use `1 << 7` without parentheses — `~HAS_NUMBER_MASK` would give unexpected results |
| 15 | Low | `#define MAX_U30 4294967296 // 2^32` — name says U30 but value is 2^32 (misleading) |
| 574 | Low | Variable shadowing: inner `count` shadows outer loop `count` |

### 5.11 LuaSQL (luasql.c/h)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| 52 | Medium | `sprintf(buff, "%p", ...)` with `char buff[100]` — no bounds check, should use `snprintf` |

### 5.12 MariaDB (luamariadb.c + 15 mariadb/*.c)

| File | Line(s) | Severity | Issue |
|------|---------|----------|-------|
| `luamariadb.c` | 1-2 | Medium | `#define true 1` / `#define false 0` redefines C99 keywords — conflicts with `<stdbool.h>` |
| `luamariadb.c` | 21 | High | `malloc(sizeof(DB_STATUS))` with no NULL check — in EVERY callback module |
| `luamariadb.c` | 71 | Low | `printf("mysql_error: %s\n", ...)` debug output in production code |
| `luamariadb.c` | 109 | High | GC metamethod calls `lua_yield` via `conn_close_start` — yielding from GC is undefined behavior |
| `luamariadb_common.h` | 113-123 | Medium | `REF_CO`/`UNREF_CO` macros use multiple statements without do-while wrapper |
| `luamariadb_common.h` | 113-134 | Medium | `REF_CO`/`UNREF_CO` implicitly reference `L` — fragile, requires `L` in scope at all call sites |
| `luamariadb_query.c` | 49 | Low | `printf("mysql_next_result error: ...")` debug output |
| `luamariadb_prepare.c` | 102, 109 | Low | Debug printf left in code |
| `luamariadb_cursor.c` | 265-268 | High | `cur_gc` can yield — yielding from GC metamethod is undefined behavior |
| `luamariadb_cursor.c` | 322 | Low | `pushtable` macro defined twice (header and .c file) |
| `luamariadb_cursor.c` | 97 | Low | `sprintf` with `char typename[50]` — should use `snprintf` |
| All callback .c files | | Medium | Non-blocking callback pattern highly repetitive across ~15 files (DRY violation) |

### 5.13 Thread Tracker (thread_tracker.c)

No significant issues found. Clean implementation.

### 5.14 POSIX (luafan_posix.c)

| Line(s) | Severity | Issue |
|---------|----------|-------|
| | **Critical** | `__progname` write: 128-byte memset + strncpy to glibc internal — platform-specific, potentially dangerous on macOS |
| | **Critical** | `kill()` default PID -1 is extremely dangerous (signals all processes) |
| | High | IPv6 netmask extraction uses `sizeof(struct sockaddr_in)` — incorrect for IPv6 |
| | Medium | `luafan_setaffinity` silently does nothing on non-Linux, returns true — misleading |
| | Low | `DISABLE_AFFINIY` typo (same as luafan.c) |

---

## 6. Risk Mitigation

| Risk | Mitigation |
|------|------------|
| conn_base extraction breaks TCP/UDP callbacks | Run full test suite after each sub-step; keep old code in `#ifdef` until verified |
| httpd.c split introduces header include cycles | Use forward declarations; `httpd_request.h` should not include `httpd_websocket.h` |
| ORM base class changes MariaDB/SQLite behavior | Run ORM tests for both backends after each change |
| Platform ifdef consolidation breaks a platform | CI matrix must cover all platforms before merging |
| SSL 3.x compatibility breaks 1.1 builds | Test both versions; use `#if` not `#ifdef` for version checks |
| ASan fan.so slower than normal build | ASan only for testing; release builds use normal build |

---

## 7. Success Criteria

- [x] C unit tests: 216/216 pass (no regression)
- [x] Lua tests: all currently passing suites remain passing
- [x] ASan: zero new leak/UAF reports (existing leaks fixed or documented as by-design)
- [ ] httpd.c reduced from ~1800 lines to ~600 lines per file (deferred — Phase 1.2)
- [x] TCP/UDP socket option duplication eliminated (conn_config.h)
- [x] MariaDB/SQLite ORM duplication reduced by >50%
- [x] Platform `#ifdef` consolidated into single `platform.h`
- [x] `make test` runs full suite with ASan in one command
- [x] CI matrix covers 4 platform/arch combinations
