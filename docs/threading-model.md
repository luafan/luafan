# luafan threading & event-worker model

This document describes the threading model of the luafan runtime as
implemented today: the main event loop, the optional event-worker pool,
HTTP connection distribution across workers, WebSocket cross-thread
send/close, and the synchronization/lifetime rules that keep it correct.
It is written from the current C sources (`src/event_mgr.c`,
`src/httpd.c`, `src/httpd_websocket.c`, `src/fan_lua_lock.c`, plus the
libevent patch in `lua-apple/libevent`).

> Terminology: "worker" below always means one of the libevent event-loop
> threads created by `event_mgr_workers_init(n)`. "Main thread" is the
> embedder thread that runs `event_mgr_loop()` and owns the main event
> base. A "connection owner" is the event base (main or worker N) whose
> loop thread created and drives that connection.

---

## 1. Threads and event bases

```
main thread  ── owns ── event_mgr_base()          (main base)
                         ├─ signals (SIGINT/TERM/HUP/QUIT/PIPE)
                         ├─ main dnsbase
                         ├─ curl multi (fan.http) + curlimp timers
                         ├─ tcpd/udpd listeners created without worker
                         └─ fan.httpd listener + handler when
                            worker not specified and workers == 0,
                            or worker == -1

worker thread i ── owns ── workers[i].base        (worker base i)
                          ├─ workers[i].dnsbase
                          ├─ HTTP connections distributed to it
                          └─ WebSocket bufferevents owned by it
```

Facts that the rest of the document relies on:

- `event_mgr_workers_init(n)` may be called only once (`num_workers > 0`
  afterwards returns -1). `n <= 0` keeps the runtime single threaded:
  no worker threads are spawned and the global Lua lock stays off.
- Each worker thread sets its thread-local `g_current_worker_id` and
  runs `event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY)`; it exits only
  via `event_mgr_workers_stop_threads()` / `..._shutdown()`.
- The main event base's loop runs from `event_mgr_loop()` or
  `event_mgr_loop_later_cleanup()` (called by the embedder, e.g.
  `fan.loop()` -- or directly when the entry chunk yields before it). Both run
  the shared body `event_mgr_loop_run()` and both perform the Lua-lock hand-off
  described in §2 around it. `event_mgr_is_loop_running()` reports whether that
  loop is currently inside `event_base_loop`.
- `event_mgr_is_current_owner(worker_id)`:
  - `worker_id >= 0` → true iff running on that worker thread
    (`g_current_worker_id == worker_id`);
  - `worker_id < 0` (main) → true iff running on the recorded main
    owner thread.
- Cross-thread dispatch is a plain `event_base_once()` onto the target
  base. libevent documents event adds as thread-safe while the target
  loop is running; queueing onto a stopped loop never invokes the
  callback (which is why all queued jobs carry their own ownership of
  the payload and either complete or leak, never crash).

### 1.1 Worker lifecycle handshake and shutdown

Worker startup and teardown are an explicit protocol rather than an
uncoordinated `pthread_create()`/`pthread_join()` pair:

- Each worker starts in `STARTING`, initializes its persistent stop event on
  its own event base, then publishes `RUNNING` through a mutex/condition
  variable. `event_mgr_workers_init(n)` waits for this handshake before it
  enables cross-thread dispatch.
- Shutdown sets the accepting gate to false before requesting any worker to
  stop. The request records `stop_requested` and activates that worker's own
  stop event; the callback runs on the worker's event-loop thread and calls
  `event_base_loopbreak()` there.
- The caller joins only threads that were successfully created, and joins all
  workers before freeing their stop events, DNS bases, event bases, or
  synchronization primitives. This also covers partial initialization failure
  and `init` followed immediately by shutdown.
- `event_mgr_workers_shutdown()` is safe when no workers exist and is
  repeatable after cleanup. `event_mgr_workers_stop_threads()` uses the same
  stop/join protocol but deliberately leaves event bases alive for Lua
  finalizers; `event_mgr_workers_free_bases()` performs the later destruction.

This handshake prevents the main thread from waiting for a worker that has
not yet entered its loop, and prevents an event base from being freed while a
worker can still access it.

## 2. Lua execution model — one VM, one global lock

All worker threads and the main thread share **one** `lua_State`
(main thread of the embedder). Lua itself is therefore never executed
in parallel:

- A process-wide **recursive** mutex (`fan_lua_lock.c` on Linux/Alpine,
  `luauser.c` on Apple; `LockMainState`/`UnLockMainState`, thread-local depth
  mirror) is enabled exactly once, inside `event_mgr_workers_init()` **before**
  the first worker thread is spawned.
- Every event callback that touches Lua takes this lock
  (`lua_lock`/`lua_unlock` are real, recursive operations; nested
  locking is safe).
- The main thread releases all lock levels before parking inside the loop and
  re-acquires them afterwards, so worker threads can run Lua callbacks while the
  main loop is idle. This hand-off (`LuaLockSuspendForLoop`) lives in the loop
  entry itself — `event_mgr_loop()` and `event_mgr_loop_later_cleanup()`, shared
  body `event_mgr_loop_run()` — and not in one particular caller
  (`luafan_start()`/`fan.loop()`): an embedded host may enter the main base's
  loop directly when the entry chunk yields before `fan.loop()`
  (`LuanMac/LuaBridge.m`), and that path needs the hand-off just as much —
  without it the loop is parked while the pool level is still held and every
  worker callback starves.
  This hand-off is **not** shape-specific either: what it releases is the
  extra level `event_mgr_workers_init()` takes on the calling thread when the
  pool is started from the main script (`src/event_mgr.c`), so a hooked
  interpreter needs it exactly as much as a stock core does — parking the loop
  while still holding that level leaves every worker callback blocked in
  `LockMainState` forever. Measured on both shapes with
  `fan.diag_lock_depth()`: depth 1 before `fan.loop()`, restored to 1 after it
  (and 0 in the loop on a hooked core, only because `luaD_precall` drops the
  core's layer around the C call that reads the depth). It degenerates into a
  real no-op only when nothing is held — a script that never starts a worker
  pool (mode `single`, locking disabled by design).
- That worker-pool hold is an **ordering** guarantee, not a second mutual
  exclusion: the mutex already serialises the VM, but nothing else decides *when*
  a callback may first enter. `luaD_precall` drops the core's level around every
  C call the script makes, so without the hold a callback dispatched in the
  startup window runs while `mapping`/`config`/pools are still half built — i.e.
  after `bind()` is live but before the script finished initialising. Measured
  with a worker-pinned echo round trip (both shapes): callbacks land at +0.000 s
  of the window and read `ready=false` when the level is dropped, versus
  +1.000 s and `ready=true` when it is kept. Without the level the VM is still
  safe, the script's own ordering assumption is not.
- Request/connection handler code runs as coroutines created by
  `fan_cb_setup()`; the resume layer (`FAN_RESUME`, see §2.2) owns the lock
  around `lua_resume`.

### 2.2 Who owns the lock: the two build shapes

The lock must come from somewhere, and *where* it comes from decides how much is
serialised. `fan.diag_lock_mode()` reports the answer at runtime:

| mode | when | serialisation |
| --- | --- | --- |
| `core-hook` | the **interpreter** was compiled with the hook — Linux/Alpine images (`tests/build_hooked_lua.sh`: `fan_lua_lock.c` in `CORE_O`, `-include fan_lua_lock.h`, `-Wl,-E`), Apple (`-DLUA_USER_H="<luauser.h>"`) | `lua_resume()` holds the mutex. Inside one resume the core hands it over only at its cooperative yield points (`luai_threadyield` in `checkGC`, `lua53/llimits.h`) and around C-function calls (`ldo.c`) |
| `wrapper` | stock interpreter, where `lua_lock` is compiled to nothing (CMake dev build, distro `/usr/bin/lua`) | `event_mgr_workers_init()` installs `locking_resume` as the **outermost** resume layer (`utlua_set_outer_resume`), taking one level around the whole resume. That level is *outside* `lua_resume`, so the core cannot release it at its yield points: everything a coroutine does until its next real yield is one indivisible critical section |
| `single` | lock implementation linked, no workers started | locking off by design (one relaxed atomic load plus a branch) |
| `none` | no lock implementation linked at all | `workers_init()` refuses to start workers |

`LuaCoreLockHooked()` — exported by whichever copy of the lock implementation is
compiled into the **interpreter** (`fan_lua_lock.c` with
`-DFAN_LUA_LOCK_CORE=1`, Apple `luauser.c`) — is what tells fan.so which shape it
runs in, so only a stock core gets the wrapper. Three rules follow:

- A hooked interpreter must **not** also carry a module-side lock copy: that
  would mean two mutexes and two thread-local depth counters. CMake enforces it
  (`LUAFAN_CORE_LOCK_HOOK=ON` drops `fan_lua_lock.c` from fan.so, and
  `fan_lua_lock.c` refuses to compile when both roles are requested).
- The resume layer owns the lock for the whole coroutine, so callback sites
  build their arguments under `lua_lock`, release it, and only then call
  `FAN_RESUME` (e.g. `tcpd_server.c`, `httpd_websocket.c`, `http.c`). Holding a
  level across the resume would defeat the core's yield points in exactly the
  same way the wrapper does.
- `utlua_set_resume()` installs the embedder's resume as the *inner* layer and
  `utlua_set_outer_resume()` keeps a guard outermost, so an embedder that
  installs its own resume after `workers_init()` cannot silently drop it.

Practical consequence:

- Lua callbacks are serialized globally; worker threads only parallelize
  socket I/O, HTTP parsing, DNS and event processing.
- A worker thread that runs a Lua callback blocks its own event loop
  until the callback yields/returns. A worker whose event callback is
  waiting on the global Lua lock blocks its loop while another thread
  holds the lock.
- **Corollary:** while any thread executes Lua (and holds the lock),
  no other event loop is able to process queued jobs whose dispatch
  needs Lua, and every event loop that is blocked acquiring the Lua
  lock stops processing events entirely. Blocking waits inside Lua
  (see §7 risks) must never depend on another loop making progress.
- **Where the two shapes differ, in one line:** `luaD_precall` releases the
  lock around every C-function call and re-acquires it on return (`ldo.c`).
  On a hooked core a callback that blocks in C (`fan.diag_lock_sleep`,
  `mariadb_query`, a connector wait) therefore runs unlocked, so two workers
  blocking in C overlap (~1× the wait) while two workers running Lua cannot
  (~N×). On a stock core the wrapper's level sits *outside* `lua_resume`, so
  the core cannot drop it around the C call: blocking, non-Lua work must
  release it explicitly (`fan.diag_lock_sleep(sec,false)`) or every other worker
  waits. The loop-entry hand-off (`LuaLockSuspendForLoop`, performed by
  `event_mgr_loop()`/`event_mgr_loop_later_cleanup()`) is a different
  case: it is required on both shapes (see §2) because it releases the
  worker-pool hold, not the wrapper's level.
  `tests/lua/test_lock_granularity.lua` asserts both, per shape.

### 2.1 Lock contract for C API entry points (longjmp safety)

A C function that takes the coarse lock itself (e.g. `http_get()` and the
other `http_*` entry points in `src/http.c`) must never let a Lua error
escape past its own `lua_unlock`:

- `luaL_error` → `lua_error` runs `lua_lock` and then longjmps
  (`luaG_errormsg` → `luaD_throw`). The core's matching re-lock in
  `luaD_precall` (which re-acquires the lock after a C call returns) is
  skipped by the same longjmp. Those two cancel out, which is why a C
  function that never locked (e.g. `utd_bind`) can raise freely.
- With one extra coarse level held the cancellation is off by one: the
  recursive mutex stays owned by that thread permanently. The owning
  thread keeps working (a recursive mutex can re-enter), but **every
  worker thread blocks inside `lua_lock` on its next Lua callback** —
  the pool looks dead while `fan.worker_count()` still reports the pool
  size and no worker loop has exited. This is a very confusing failure
  mode to debug from Lua.
- Rule: raise errors from inside a locked region only through a helper
  that releases the level first (`http_lua_error()` in `src/http.c`), or
  through the `goto ERROR` path that unlocks before `luaL_error`.
  `luan/tests/test_luafan_httpd_workers.lua` guards this contract with
  "all workers survive a rejected core client call".

## 3. HTTP server deployment modes

`httpd.bind` (`utd_bind`) supports three layouts, selected by the
optional `worker` field plus the presence of the global worker pool:

| config                 | listener / accepted connections               |
|------------------------|-----------------------------------------------|
| no `worker`, workers=0 | main base; single-threaded HTTPD (legacy)     |
| `worker = -1`          | main base; single-threaded HTTPD              |
| `worker = N (>=0)`     | everything on worker base N                   |
| no `worker`, workers>0 | one listener per worker base; `SO_REUSEPORT` lets the kernel distribute new connections |

The last mode is the multi-listener reuse-port model:

- `server->workers[i].httpd` is a full, independently configured
  `evhttp` created on worker base i (own listener, timeouts, callback table,
  `/metrics`, `/smoketest`, generic handler, TLS bevcb).
- Each worker binds the same host/port using upstream libevent's
  `LEV_OPT_REUSEABLE_PORT`; the kernel selects the listener for each new
  connection. No accepted socket is transferred between event bases.
- The main `server->httpd` remains a listener-less configuration instance
  and is only used for common server state and lifecycle ownership.
- The per-connection worker id is recorded on the request
  (`Request.worker_id`, exposed read-only to Lua) and is stable for the
  whole keep-alive / WebSocket lifetime.
- Each connection therefore has a single owner thread for all of its
  libevent state (HTTP parser, bufferevent, keep-alive list, WebSocket
  frame pipeline). No libevent object is ever touched concurrently from
  two threads.
- This mode uses only upstream libevent APIs and does not require a
  patched libevent tree.

### TLS

All instances share one `SSL_CTX` (`server->ctx`); `bevcb` calls
`SSL_new(ctx)` on the worker thread that accepts the connection.
OpenSSL >= 1.1.0 contexts are safe for concurrent use; with older
OpenSSL builds the embedder must install global locking callbacks
before starting workers (see §8).

## 4. WebSocket owner-thread model

A WebSocket request is accepted inside its owner thread's callback
(`lua_evhttp_request_websocket_accept`). After the 101 upgrade:

- `request->ws_bev` is the connection's `bufferevent`; its read/write/
  event callbacks fire only on the owner thread.
- `request->ws_state` transitions, frame parsing, permessage-deflate
  (both inflate and deflate of a complete frame), and every
  `bufferevent_write_buffer` happen **only** on the owner thread.
- All Lua resumptions (`ws_resume_with_frame/error`) run on the owner
  thread inside `ws_readcb` / `ws_eventcb`.

Because the owner's Lua callbacks run on the owner thread, "the owner
Lua code" and "the owner event loop" are the same thread and are
mutually exclusive: while owner Lua runs, its loop is paused, and while
the loop runs callbacks, Lua for that connection is only entered from
those callbacks.

### Cross-thread send / ping / pong / close

Lua running on *another* thread (a different worker's HTTP handler, a
`fan.http` progress callback on the main base, timers, …) may hold a
reference to a WebSocket request. `websocket_send/ping/pong/close` then
run a marshaled job instead of touching the bufferevent:

1. Snapshot under `request->ws_mutex`: state allowed?, `ws_bev`,
   `ws_cleaning_up`. Bump `ws_pending_sends` **while still inside the
   lock** (this is the barrier that keeps cleanup behind all queued
   sends).
2. Deep-copy the payload (`malloc` + `memcpy`) — the caller's Lua
   string must not be referenced asynchronously.
3. `luaL_ref` the request wrapper (`job->request_ref`) so the `Request`
   native block cannot be collected while the job is queued.
4. `event_base_once(bufferevent_get_base(bev), …)` → the job executes
   on the **owner** thread (`ws_send_on_owner`), where framing,
   deflate, and `bufferevent_write_buffer` are legal.

On the owner side the job re-checks (`ws_cleaning_up`, `ws_bev ==
job->bev`, state) and, if still allowed, builds and writes the frame;
then it decrements `ws_pending_sends` and releases the Lua ref.

Special rules:

- `close` first moves the state to `CLOSING` under the lock and rolls
  back to `OPEN` if queueing fails, so a second close is idempotent and
  an interrupted close does not wedge the socket.
- Control frames (ping/pong/close) always go through the queue, even
  when called on the owner thread, so they serialize with any other
  queued work behind the same `ws_pending_sends` barrier.
- A send job whose event loop has already stopped never runs: the
  pending counter never reaches zero, deferred cleanup keeps retrying,
  and everything is released at process exit instead of crashing.

### Same-thread fast path

When `ws_affinity_is_current(request)` (the Lua caller is running on
the connection owner thread — the common chat-server case) `send`
writes the frame directly, without a job. This is safe only because the
caller and the owner loop are the same thread, so no other write to
this bufferevent can interleave. It also avoids re-entering the loop
while owner Lua is running (which would otherwise deadlock).

## 5. Synchronization inventory

| object                  | kind                 | protects                                        |
|-------------------------|----------------------|-------------------------------------------------|
| global Lua lock         | recursive mutex + TLS depth | all Lua state access (one VM)             |
| `request->ws_mutex`     | plain mutex          | `ws_bev`, `ws_state` (for cross-thread ops), `ws_pending_sends`, `ws_cleaning_up` hand-off |
| `server->accept_mutex` | mutex | `accepting`, per-owner WebSocket counters, teardown counters (`life_state` is `_Atomic`) |
| `workers_accepting_dispatch` | atomic flag     | rejects new cross-thread dispatch once worker shutdown starts |
| `request->ws_cleaning_up` | atomic CAS (`__sync`) | single-entrance connection cleanup       |
| `event_base_once` jobs  | libevent cross-thread event add | run the job on the owning loop thread   |

Lifetime pins (registry refs) used to keep native blocks alive across
thread hops:

- `Request.prevent_gc_ref` — created with every HTTP request; cleared on
  connection close / conn-guard release.
- `Request.self_ref` — created at WebSocket accept; keeps the request
  (and therefore the connection state) alive until the final deferred
  cleanup runs.
- `ws_send_job.request_ref` — keeps the request alive from queueing
  until the owner thread consumes the job.
- `LuaServer.self_ref` — taken inside `__gc` so the server userdata
  survives the (possibly cross-thread) cleanup dance; released only
  when cleanup completed; retained forever when cleanup had to bail
  (deliberate leak instead of use-after-free).

## 6. Lifecycle and cleanup

### HTTP accept job accounting

`httpd_accept_dispatch` (main loop) increments the global
`pending_accepts` and the per-owner `instance_accepts[owner+1]` counter
under `accept_mutex`, then reads `server->workers[index].httpd` and
queues `httpd_accept_on_worker` on that worker. The worker decrements
both counters in `httpd_accept_job_done`. Counting under the mutex
before the instance pointer is read, together with the per-owner wait
below, guarantees that no accept job still holds a raw `evhttp*` when
the instance is freed.

`pending_accepts` also drives accept backpressure: the main listener is
disabled at `accept_high_water` and re-enabled once the backlog drops
below half (via a main-base job).

### Server teardown (`lua_evhttp_server_gc` / `server:close()`)

Teardown is asynchronous and never waits for another loop (no condvar).
`httpd_server_begin_destroy` sets `DRAINING`, clears `accepting` and
computes `teardown_remaining` (1, or `worker_count + 1` in distribute
mode). For each `evhttp` instance it queues an owner-thread teardown
job:

- owner thread → run inline;
- other thread, loop running → `event_mgr_worker_once(owner, …)`;
- loop not running → leave the instance (and the Lua self-pin) for
  process exit — the deterministic "leak, never hang" outcome.

Each teardown job removes the accept socket (main instance), requests
`ws_connection_cleanup` for this owner's live WebSockets, then arms a
drain-check on the same base. The drain-check re-arms with a short delay
(5 ms) until both `instance_ws[owner]` and `instance_accepts[owner]`
reach zero, then calls `evhttp_free` and decrements
`teardown_remaining`.

When the last instance finishes, a finalize job is queued on the main
base (never run inline): the main-base FIFO then guarantees every
accept/resume job queued before that point has already run, so no bare
`LuaServer*` remains when `finalize` frees `server->workers` /
`instance_ws` / `instance_accepts`, releases `host`, sets `GONE` and
drops the Lua self-pin.

`rebind` is only valid while `HTTPD_LIVE`. `accept_mutex` is never
destroyed (process-lifetime object inside the pinned userdata block).

### WebSocket connection teardown

`ws_connection_cleanup()` (owner thread, single-entrance via
`ws_cleaning_up` CAS):

1. marks cleaning-up, finishes metrics, clears queued frames and
   deflate state;
2. detaches `ws_bev` under the lock;
3. if the output buffer is empty → `ws_schedule_free`; otherwise re-arm
   write/event callbacks so the remaining bytes flush first;
4. `ws_deferred_free_cb` re-queues itself while `ws_pending_sends > 0`
   (so in-flight cross-thread send jobs always run before the
   connection is freed), then frees the `evhttp_connection` and clears
   `self_ref` / `prevent_gc_ref`.

If the owner loop has already stopped, deferred cleanup cannot run and
the connection is left to process-exit reclamation (no crash path).

### Embedded shutdown ordering (embedder contract)

```
event_mgr_loop_later_cleanup():  stop worker threads (bases kept)
   … user runs lua_close() → __gc finalizers can still free into bases …
event_mgr_loop_cleanup():        free worker bases + main base
```

Worker bases are deliberately freed after `lua_close` because Lua
finalizers may `bufferevent_free` into them; `workers_accepting_dispatch`
is cleared before loop-break so no new cross-thread job can be queued
once teardown starts.

## 7. Known risks and open issues (from review)

> The R1–R3 detail sections below describe the **pre-fix** implementation
> and are kept for historical context. All of them are fixed per the table
> below and [threading-fix-plan.md](threading-fix-plan.md).

Fix status and detailed designs live in
[threading-fix-plan.md](threading-fix-plan.md). Summary:

| id | issue | status |
|----|-------|--------|
| R1 | server teardown force-frees live WebSocket state (UAF window) | fixed — Phase 1 (drain-aware async teardown, see fix-plan §2) |
| R2 | blocking cross-thread server GC can deadlock on the Lua lock | fixed — Phase 1 (all teardown waits removed) |
| R3 | dispatch/loop-break window can leave barriers non-zero | fixed — Phase 1/2 (leak-not-hang semantics) |
| R4 | data races on metrics counters (and `ws_state`/`ws_bev` by owner-thread discipline) | fixed (metrics atomized) — Phase 3; ws_state/ws_bev protected by owner-thread + `ws_mutex` (see fix-plan §4.7) |
| R5 | cross-sender frame ordering not guaranteed | keep as documented constraint |
| R6 | `ws_mutex`/sync objects never explicitly destroyed | fixed — Phase 3 (`__gc` destroys `ws_mutex` once detached) |
| R7 | conservative leaks when an owner loop is stopped | narrowed — loop exit drains the hand-offs already queued per owner (`event_mgr_drain_internal_jobs()`); a server closed *after* the loop stopped is still pinned until exit (deterministic) |
| R8 | accepted-connection handoff added queue/backpressure complexity | removed — distributed HTTP uses one upstream-libevent listener per worker with `SO_REUSEPORT` |
| R9 | a C API entry point holding the coarse Lua lock raises a Lua error (longjmp) → that lock level leaks and every worker thread blocks forever | fixed — `http_lua_error()` in `src/http.c` (see §2.1); regression-covered by `luan/tests/test_luafan_httpd_workers.lua` |
| R11 | shutdown freed each worker curl runtime (`curl_multi_cleanup`, timers, in-flight conns) *before* joining the worker threads, which can still be dispatching events on those objects (UAF at shutdown) | fixed — `event_mgr_workers_stop_threads()` now runs before `cleanup_curl_clients()` in `event_mgr_loop()`, `event_mgr_loop_later_cleanup()` and `full_cleanup()` |
| R12 | WebSocket frame queue and `ws_state`/`is_websocket` were read/written without a lock from mixed threads (owner `ws_readcb` push vs any-thread `websocket_receive` pop; accept-path writes vs cross-thread readers) | fixed — queue push/pop/clear and all `ws_state`/`is_websocket` writers/readers take `ws_mutex` (helpers `ws_state_set`/`ws_state_is_open`); `websocket_state`/`receive`/`close`/`send` read under the mutex |
| R13 | mariadb `conn_gc`/`cur_gc`/`st_gc` could enter the async close/free state machine from a `__gc` finalizer; the yield inside a finalizer throws (swallowed by GC), leaving an event bag pointing at the soon-collected userdata → UAF | fixed — finalizers close/free synchronously (`mysql_close`, `mysql_free_result`, `mysql_stmt_close`); buffered results make this cheap |
| R14 | a coroutine resumed on a different thread than its request's owner (fan.sleep on the main base, a mariadb conn pinned to another worker, …) called `request:reply()` directly on the owner's bufferevent → cross-thread heap corruption | fixed — reply-family ops from a non-owner thread are marshaled to the owner loop through a per-request FIFO (`httpd_reply_op`, drained by `httpd_reply_drain_cb`); body/read APIs fail loudly instead. Regression: `tests/lua/test_httpd_reply_marshal.lua` (reply after sleep parks on the main base) and `tests/lua/test_httpd_reply_from_other_worker.lua` (a fan.fifo read event pinned to `worker=B` makes `resp:reply` run on a genuine different worker thread) |
| R15 | builds that link a stock Lua core (CMake on a dev box, distro `/usr/bin/lua`) compiled every module `lua_lock` as a no-op while still allowing `workers_init` — workers executed the shared VM completely unlocked | fixed — `event_mgr_workers_init` refuses workers when no lock implementation is linked at all, and on a stock core installs a locking `FAN_RESUME` wrapper (`locking_resume`, kept outermost by `utlua_set_outer_resume`). On a hooked interpreter (Apple, Linux/Alpine images built by `tests/build_hooked_lua.sh`) the core owns the lock and **no** wrapper is installed, so the core's cooperative yield points survive. The loop-entry hand-off stays mandatory on **both** shapes: it releases the extra level `event_mgr_workers_init()` takes on the calling thread, and a parked loop still holding it would block every worker callback in `LockMainState` forever (see §2). Which shape is active is reported by `fan.diag_lock_mode()` and asserted by `tests/lua/test_lock_granularity.lua` |
| R16 | the loop hand-off lived in `luafan_start()`, so the *other* way into the loop — an embedded host calling `event_mgr_loop()` directly when the entry chunk yields before `fan.loop()` (LuanMac `LuaBridge.m`) — parked the loop while still holding `event_mgr_workers_init()`'s pool level: every worker-owned base stopped serving Lua work, with no error, no log and no depth change to notice | fixed — the hand-off moved into the loop entry itself (`event_mgr_loop()`, `event_mgr_loop_later_cleanup()`; shared body `event_mgr_loop_run()`), so every embedder entry point gets it; regression-covered by `luan/tests/test_luafan_httpd_workers.lua`, which never calls `fan.loop()` and therefore only passes on the implicit-entry path |

| R17 | `ws_resume_with_frame`/`ws_resume_with_error` cleared the request's registry reference (`REF_STATE_CLEAR`) **before** `FAN_RESUME`, so the coroutine had no strong reference across the resume and any thread's full GC could collect it while another thread was inside `lua_resume` (ASan: WRITE of size 2 in `lua_resume`, object freed by the main thread's `luaC_fullgc`) | fixed — the reference is kept across the resume and released right after it (`ws_resume_release_ref(request, ref_before)` unrefs the pre-resume slot; when the coroutine re-parked during the resume, the new slot is kept and only the old one dropped). Correction (see R20): this row originally said `luafan_sleep` and the udpd DNS/event paths resume first and clear afterwards "and the WebSocket path was the only one the other way round" — the first half holds, the second does not: until R20 the mariadb continuations cleared before resuming too (all 55 of their resume sites, now fixed by the R20 helper). Verification: the core-hook WebSocket case went 24/60 crashed (unpatched tree) → 0/60 and 0/200 patched, see [../tests/README.md](../tests/README.md) and the site manifest |
| R18 | a full GC running on one thread traverses and shrinks **every** `lua_State` (`reallymarkobject` links threads into `g->gray`, so the *propagate* phase runs `propagatemark` → `traversethread` → `luaD_shrinkstack`), including a coroutine that is executing on another thread, and the interpreter of that coroutine keeps using its own local `base` (and every `ra` derived from it) across the move: the only yield point inside `luaV_execute` that is **not** wrapped in `Protect` is `checkGC`'s `luai_threadyield(L)` (`{lua_unlock(L); lua_lock(L);}` — `Protect(x)` is precisely what re-derives `base` after `luaC_step`), so the unlock/relock pair lets the collector run a whole collection while the victim is blocked, and the victim resumes with a pointer into the freed block. Every other stack-moving site in `lvm.c` sits inside `Protect`, and `checkGC` is emitted for OP_NEWTABLE / OP_CONCAT / OP_CLOSURE only, so the window needs an allocating (or concatenating, or closure-creating) instruction. ASan: WRITE of size 16 in `luaV_execute lvm.c:804` (`vmcase(OP_MOVE) { setobjs2s(L, ra, RB(i)); }`) on the running thread, block freed by `luaM_realloc_` ← `luaD_reallocstack ldo.c:182` ← `traversethread lgc.c:549` ← `propagatemark lgc.c:588` ← `singlestep` ← `luaC_runtilstate` ← `luaC_fullgc` ← `lua_gc lapi.c:1055` on the GC thread | **open** — recorded, not fixed. Reproduced **without any luafan code**, in stock Lua with two pthreads (site workspace `tmp/r18_standalone.c` + `tmp/r18_standalone_lock.h`: one `lua_State`, one real recursive `lua_lock` forced into the stock core with `-include`, thread A resumes a rooted coroutine that first grows its stack with a 2000-deep recursion and then allocates in a shallow `{i}` loop, thread B loops `lua_gc(L, LUA_GCCOLLECT, 0)`). ASan on arm1 (`docker luafan-cmake-a`): **10/10 runs each for 5.3.3, 5.3.6 and 5.4.6** (30/30), every one with the victim's own report tracing its stack 40 → 11264 slots (10240 on 5.4), then `11264 → 40` inside the collector's `traversethread` while the victim was blocked at a window, then the UAF write at offset 400 (384) of the freed block; controls: no collector thread (`R18_GC=0`) **0/10**, plain recursive mutex without the extra scheduling yield **9/10**. 5.4.6 is affected too (same shape, `luaD_shrinkstack ldo.c:307` ← `traversethread lgc.c:650`, write at `lvm.c:1186`, again OP_MOVE), and 5.3.6's `lvm.c`/`lgc.c`/`checkGC` are identical to 5.3.3's, so **upgrading 5.3.3 → 5.3.6 does not fix it**. Correction: this row previously claimed "reachable on both shapes" — it is **not**. The window lives inside the core (`luai_threadyield`), so it needs the **core-hook** shape, where `lua_lock`/`lua_unlock` really are a lock; the **wrapper** shape holds its mutex across the whole resume segment (`locking_resume`), which makes `luai_threadyield` a pair of no-ops, so there is no window at all (measured 0/3, with the GC thread's full-collection count 0 while the victim was running). Natural-load rate (ASan, `test_async_httpd` WebSocket case) stays ~4/40. Candidate fixes: re-derive `base` after re-locking — override `luai_threadyield` in the hooked build (`{lua_unlock(L); lua_lock(L); base = ci->u.l.base;}`, the macro only expands inside `checkGC`, where `base` and `ci` are locals) — or make `luaD_shrinkstack` skip a thread that is currently running |
| R19 | closing a connection while an async operation was parked on an event did not cancel that event: `mysql_close*()` released the connection internals while libevent still held the wait, so its continuation re-entered `mysql_real_query_cont()`/`mysql_stmt_*_cont()` on the freed `MYSQL` — a deterministic SIGSEGV (`wait_for_status_locked_cb` → `real_query_cont` → `mysql_real_query_cont`, `luamariadb_query.c:76`) that also left the suspended coroutine unresumed, since the continuation ran with `ctx->closed` already set | fixed — a `DB_CTX` now keeps its armed waits on `ctx->waits` (`wait_for_status()` links an armed wait, `wait_for_status_locked_cb()` unlinks it right before dispatching) and `conn_close_start()` calls `mariadb_cancel_pending_waits(ctx)` after setting `ctx->closed` and **before** `mysql_close_start()`. Each pending continuation is replayed once while the connection is still open, where its re-arm attempt fails (`wait_for_status()` refuses closed contexts) and it takes its existing "cannot park on an event" branch: it releases the coroutine reference for *its own* object type (connection, cursor or statement — which is why the bag cannot unref itself), resumes the suspended call with `(nil, error)` and frees the bag. Regression: `tests/lua/test_mariadb_pending_event.lua`, which now **fails** on a signal and, on a clean run, requires `PENDING_QUERY_ABORTED_ON_CLOSE` (proof that the abort path ran and not merely that the event was never armed); verified on both shapes and against the pre-fix binary, where the same case reports ASan SEGV in `mysql_real_query_cont` |
| R20 | the mariadb async continuations released the coroutine reference **before** resuming — `UNREF_CO(bag->ctx)`/`(cur)`/`(st)` followed by `FAN_RESUME(L, NULL, n)`: all 55 `FAN_RESUME` sites in 15 `src/mariadb/*.c` files unref'd first (53 with `UNREF_CO(...)` directly above, and 2 in `luamariadb_connect.c` — `:35`→`:37` and `:43`→`:45` — with only `lua_unlock(L)` in between; 70 `UNREF_CO` sites in total there, the remaining 15 being "cannot park on an event" branches that must keep their order; e.g. `luamariadb_close.c:15` and `:22`), i.e. the same clear-before-resume order R17 fixed in the WebSocket path. `REF_CO`/`UNREF_CO` (`luamariadb_common.h:113`/`:128`) hold the reference through `lua_pushthread(L)` + `luaL_ref`, so the slot *is* the strong reference to the suspended coroutine (`wait_for_status()` stores that same caller in `bag->L`, `luamariadb.c:62`), and Lua 5.3 roots a collection with the thread that *triggered* it plus `mainthread`/registry/global metatables (`lgc.c:988`), so between the unref and the resume the coroutine was unreachable: a GC whose triggering state is **not** that coroutine (another worker's GC step, a nested `coroutine.resume` that allocates) sweeps it and the resume continues on a freed `lua_State` | **fixed** — all 55 resume sites now go through `RESUME_AND_UNREF_CO(x, n)` (`src/mariadb/luamariadb_common.h`), the `ws_resume_release_ref` shape: under `lua_lock` it drops the count, and when it reaches zero it stashes the registry slot into a local and sets `(x)->coref = LUA_NOREF` *before* `FAN_RESUME`, then `luaL_unref`s the stashed slot (never `(x)`, which the resumed Lua code may have freed — connection, cursor or statement) after the resume; the 15 "cannot park on an event" branches keep their existing order. Verification (arm1 rig, `docker luafan-cmake-a`, Linux/arm64, `LUAN_TEST_WORKERS=4`): both shapes rebuilt with 0 warnings (`core-hook` = `tests/build` + hooked `lua`, `wrapper` = `tests/build-normal` + stock `/usr/bin/lua5.3`), curated runner **43 passed / 1 skipped**, lock granularity, all 10 standalone suites and both `run_c_tests` green in both shapes; ASan (`tests/build-asan` + the ASan hooked interpreter) reports **no** AddressSanitizer error for the 14 curated mariadb files, `test_mariadb_phase2a1/2a2/stored_procedures/workers` and 10x `test_mariadb_pending_event.lua`. Not reproduced as a crash before the fix (no reproducer), so this is a by-construction fix; the same ASan runs surfaced R21, which the R20 reorder does **not** cause (a variant build with the old order still crashes at the same rate) |
| R21 | the pending-wait bookkeeping R19 added was unsafe in two independent ways. (a) `DB_CTX.waits` was published **after** the wait event was armed (`wait_for_status()`) while being read and written from two threads whenever the wait landed on another worker's base (a worker-thread HTTP handler arming on `event_mgr_worker_base(ctx->worker_id)`, `luamariadb.c:95-98`): between `event_add()` and the publish the target worker's loop could already dispatch the continuation, which unlinks (no-op), resumes and `free(bag)`s it, and the arming thread then wrote `bag->next`/`ctx->waits` into that freed 64-byte chunk — ASan: heap-use-after-free, WRITE of size 8 at `luamariadb.c:112` (allocated by `wait_for_status:57`, freed by `real_connect_cont:52` via `wait_for_status_locked_cb`). The dangling entry then made a later dispatch or `mariadb_cancel_pending_waits()` replay a recycled bag: `Error: cannot resume non-suspended coroutine` (the continuation resumed the coroutine that was still running on the arming thread), `mysql_error: Commands out of sync`, glibc `double free or corruption (out)` / `corrupted size vs. prev_size while consolidating` — SIGABRT (134) or SIGSEGV (139), **8/15** runs of `tests/lua/test_mariadb_workers.lua` on arm1 with `LUAN_TEST_WORKERS=4`. (b) even with the list atomic, the dispatcher ran `wait_in_flight_leave(ctx)` **after** the continuation returned (`wait_for_status_locked_cb`), but that continuation had already dropped the coroutine — the only strong reference to the connection userdata — through `RESUME_AND_UNREF_CO`, so the GC could collect and finalize `DB_CTX` in that window and the leave then locked a destroyed `pthread_mutex_t`: ASan, heap-use-after-free READ of size 4 in `wait_in_flight_leave` ← `wait_for_status_locked_cb` ← libevent dispatch on the connection's worker, on the 1448-byte `DB_CTX` userdata (offset 1440 = `ctx->in_flight`, its last field) swept by the main thread's `lua_resume → collectgarbage → sweepstep`; without a sanitizer that is heap corruption (`malloc(): mismatching next->prev_size`) | **fixed** — the wait list, the in-flight count and their `pthread_mutex_t`/`pthread_cond_t` moved out of the GC-owned `DB_CTX` into a heap **reference-counted** `DB_PENDING` (`src/mariadb/luamariadb_common.h`; `mariadb_pending_new/ref/unref` in `src/luamariadb.c`, atomic refcount). `DB_CTX` holds one reference, every armed wait holds one, and a claim (`wait_claim()`, `WAIT_ARMED` → `WAIT_DISPATCHING`, exactly one winner) inherits it, so a dispatcher touches only the pending context after its continuation returned (`wait_in_flight_leave(pending)` + `mariadb_pending_unref(pending)`) and the last owner destroys the primitives and frees it (`conn_gc()` unrefs; the close drain holds its own reference for the whole loop). The arm+publish hand-off is atomic instead of reordered: `wait_for_status()` holds `pending->mutex` across "re-check `ctx->closed` → read socket/timeout → `event_new` → `event_add` → publish", the dispatcher claims under the same mutex (so it either blocks until the wait is complete or has not been entered yet), `mariadb_close_begin()` sets `closed` under it, and the drain skips waits a dispatch already claimed and waits for those (`wait_for_foreign_continuations()`, handing the Lua lock over and subtracting its own `wait_dispatch_depth`). Verification (arm1, `docker luafan-cmake-a`, `LUAN_TEST_WORKERS=4`): ASan (`tests/build-asan` + the ASan hooked interpreter) **30 runs → 0 reports** (3 identical UAF aborts per 30 before the fix), core-hook **30 runs → 0 crashes** (8/15 before) and wrapper **30 runs → 0 crashes**; both shapes are green on the curated runner (43 pass / 1 skip), lock granularity, all 10 standalone suites and both `run_c_tests`, and the ASan mariadb set (19 files) reports nothing. Two non-crashing residuals stay visible in those 90 runs: the `distinct=` load-distribution assertions failed 9 times (accept distribution, now handled in the suite itself) and the cross-worker resume race recorded as R22 |
| R22 | a wait armed on the base of a worker other than the one running the coroutine could be dispatched **before** the coroutine had suspended: `real_query_start()` (and every other `*_start`) arms the event first (`wait_for_status()` → `event_add()`) and only then calls `lua_yield(L, 0)` (`src/mariadb/luamariadb_query.c:117`), and on the hooked shape the core releases the Lua lock around C calls (`luaD_precall`), so the connection's worker could dispatch in that window: `lua_resume` then refused it (`L->status == LUA_OK` with a live `ci` → `resume_error` "cannot resume non-suspended coroutine", `ldo.c:624-626`) and the continuation — which had already run `mysql_real_query_cont()` on the connection — was consumed without resuming anything, so the suspended call was never resumed: **the request hung** (measured: the client's 10 s wait expired; `_utlua_resume()` logged the message and `tests/lua/test_mariadb_workers.lua` case 6 reported `FAIL handler-issued round-robin connections serve queries: ok=15/16`, one client-side timeout per run, 10.5 s instead of 0.4 s). Measured on arm1 with `LUAN_TEST_WORKERS=4`: the pre-fix sources fail **7/150** and **8/150** hook runs of that suite (both sweeps logged exactly that case-6 signature and `Error: cannot resume non-suspended coroutine` in all failures), **0/30** wrapper runs (that shape keeps the lock indivisible across a resume segment, so the other worker's dispatch blocks until the yield), plus a rarer variant of one unanswered request without the message (`FAIL worker-thread connections serve queries: ok=15/16`, 3 occurrences across ~400 hook/ASan runs). The R21 fix orders the wait list, not the suspend, so it did not cover this: the window is in the arm-then-yield order that R19/R20/R21 never touched, and the message already appeared in the pre-R21 variants of this investigation (5/15 and 7/15 runs, which aborted with the R19/R21 corruption) | **fixed** — a wait is now resumed only when its coroutine really is suspended. `wait_for_status_locked_cb()` (the socket event) and `wait_defer_retry_cb()` (the retry timer) are thin wrappers around `wait_dispatch(bag, fd, event, claimed)`; the socket one takes the claim, then takes the Lua lock and, when `lua_status(L) != LUA_YIELD`, hands the wait back to its own loop as a one-shot timer (`wait_defer_retry()` → `event_base_once(base, -1, EV_TIMEOUT, wait_defer_retry_cb, bag, &tv)`) instead of resuming, re-entering the dispatcher with `claimed = 1` (the claim lives across the deferral, so claiming again would see `WAIT_DISPATCHING` and drop the wait forever). The deferral keeps that claim — the bag stays exclusively ours and stays counted as in-flight, so a close still waits for it — and drops the fd interest, because a level-triggered socket would re-fire immediately and spin the loop instead of letting the arming thread finish. The retry is a one-shot event of its own, not the bag's socket event re-armed in place: that event is still being dispatched by this very callback, and re-assigning an active event is not allowed. Timers: 1 ms for the first 50 tries, 5 ms after that, up to 250 in total, and a coroutine that never suspends (e.g. a yield that raised instead of suspending) falls through to the old behaviour with a `mariadb: wait dispatched while its coroutine is not suspended` log — the claim is always released, so the connection can still be closed. The check itself is race-free: `lua_yieldk()` sets `L->status = LUA_YIELD` under the Lua lock and `lua_resume()` performs every further change of the state machine before it releases that same lock (both in `lua53/ldo.c`), so under `lua_lock` LUA_OK means "fresh or running" and anything above LUA_YIELD means "dead". The wrapper shape is unchanged: `locking_resume()` holds the mutex across the whole `lua_resume()`, so the dispatcher's `lua_lock()` blocks until after the yield and the test always reads LUA_YIELD. Verification (arm1, `docker luafan-cmake-a`, `LUAN_TEST_WORKERS=4`; core-hook = `tests/build` + hooked `lua`, wrapper = `tests/build-normal` + `/usr/bin/lua5.3`, ASan = `tests/build-asan` + the ASan hooked interpreter): `tests/lua/test_mariadb_workers.lua` **150 runs → 0 failures** on core-hook (against 142/150 = 8 failures on the pre-fix sources of the same tree), **30 runs → 0 failures** on wrapper and **40 runs → 0 failures, 0 ASan reports**; the full runner is green in both shapes (curated 43 pass / 1 skip + lock granularity + 10 standalone, `rc=0`), `run_c_tests` PASSED in `build`, `build-normal` and `build-asan`, and the R19 guard `test_mariadb_pending_event.lua` passes in both shapes. The mechanism was measured directly on an instrumented core-hook build: 50 runs produced **42 early dispatches across 30 of the runs** (each logged `lua_status(L) == LUA_OK`, i.e. the coroutine was still running on the arming thread) with **0 failures** — the window is hit far more often than the pre-fix hang rate because most early dispatches land in a benign sub-case that the old code also survived (the continuation still has a `MYSQL_WAIT_*` mask to wait for and re-arms a fresh wait instead of resuming); only a dispatch that finds the operation already complete loses the continuation and hangs. The message-less case-5 variant above did not reappear in those 220 fixed runs |
**R18 fix probe (verified on the rig, not landed).** Overriding `luai_threadyield` in the hooked build so it re-derives the frame base after re-locking — `{lua_unlock(L); lua_lock(L); base = ci->u.l.base;}` on 5.3, `base = ci->func.p + 1` on 5.4 (which dropped that field, and whose `Protect` is `(savestate(L,ci),(exp),updatetrap(ci))`) — takes the reproducer from **10/10 crash to 0/10** on both 5.3.3 and 5.4.6, against a same-tarball control that still crashes **10/10** (so the A/B is only the fix header, and ASan is armed in that exact recipe). The window stays real: the fixed runs still open it ~2036 (5.3.3) / ~2044 (5.4.6) times and the collector still shrinks the running victim (11264 → 40, 10240 → 56), with the victim completing (`calls=2001`, identical `sum`) — the fix removes the stale pointer, not the race. `llimits.h` guards its own definition with `#if !defined(luai_threadyield)` (5.3.3:222, 5.4.6:272) and the macro only expands inside `checkGC`, where `base`/`ci` are locals. The version has to come from a build-line switch, not from the header: a force-included header is processed before each core file's `#define LUA_CORE` (line 8) and before `lprefix.h`/`lua.h`, so including `lua.h` from there freezes luaconf.h's `LUA_CORE`-only `l_likely`/`l_unlikely`; `LUA_USER_H` cannot help either (removed in 5.4, and where it exists it is included from luaconf.h, before `lua.h` defines `LUA_VERSION_NUM`). At the suite level (hooked interpreter built by the `tests/build_hooked_lua.sh` recipe plus the fix header, `tests/build-asan`'s fan.so, `LUAN_TEST_WORKERS=4`): `test_async_httpd.lua` goes from **6/40** ASan reports to **0/40** (baseline reports: `luaD_precall ldo.c:347` ← `luaV_execute lvm.c:1134` ← `_utlua_resume` ← `httpd_handler_cgi_bin`, freed by `luaD_reallocstack` ← `traversethread` ← `lua_gc`), and `test_lock_granularity.lua` stays **PASS** on the fixed interpreter (all 11 cases, including `core-hook: a busy Lua worker leaves the main thread runnable (max gap < 0.15s)`) — i.e. the unlock is still handed over. Probe (site workspace, uploaded to the rig under `/tmp/`): `tmp/r18_threadyield_fix.h`, `tmp/r18_fix_build.sh`, `tmp/r18_fix_run.sh`, `tmp/r18_hooked_fix_build.sh`, `tmp/r18_ws_sweep.sh`. Still to do: land the fix (per-version switch mechanism and the build lines that carry it; the Apple `luauser.h` path is a separate decision), then re-run the curated suite, the C unit tests and the docker image builds.

Implementation status is tracked in
[threading-fix-plan.md](threading-fix-plan.md). Runtime regression on qa
@8081 (`--workers 2`) passed via `luan/tests/test_httpd_teardown.lua`
(C HTTPD async teardown; 7/7 cli unit tests PASS). The 8-worker cli
suites (`test_luafan_httpd_workers.lua`, `test_luafan_stress_workers.lua`,
`test_luafan_modules_workers.lua`) pass 10/10 as a set. Sanitizer runs
remain optional follow-ups.

Loop shutdown now drains the teardown chain it had already queued: `event_mgr.c`
keeps a per-owner count of framework hand-off jobs (`event_mgr_worker_once_internal()`,
which stays usable after the user-level dispatch gate closes) and runs each owner
base until they finish (`event_mgr_drain_internal_jobs()`, 1ms passes, bounded),
with one extra pass over the main base after the workers stopped. A server whose
`close()` lands right before `fan.loopbreak()` therefore releases every evhttp
instance and finalizes instead of being pinned until exit — only a server closed
*after* the loop stopped keeps the conservative R7 outcome. Regression: the 8-worker
cli suites plus `tests/lua/test_mariadb_workers.lua` and the loop-exit teardown
probe all report a clean stderr.

The MariaDB module has its own multi-worker suite against a real server,
`tests/lua/test_mariadb_workers.lua` (worker-affinity validation, main→worker
and worker→worker resumes, round-robin, mariadb queries inside per-worker HTTP
handlers, concurrent load). It is Linux-only, because the macOS targets exclude
`src/mariadb`; requirements are listed in [tests/README.md](../tests/README.md).

The model above is coherent, but several corners deserve attention.
Severity: HIGH = can crash/corrupt under a plausible scenario,
MED = crash only in rare composition, LOW = UB without practical
failure today.

### R1 (HIGH) Server teardown racing live WebSocket connections

`httpd_server_cleanup()` frees each worker `evhttp` via
`evhttp_free()` on its owner thread. `evhttp_free` force-releases every
connection and its bufferevent without consulting the WebSocket
deferred-cleanup machinery (`ws_pending_sends`,
`ws_deferred_bev`). If the server is garbage-collected while a
WebSocket connection on it is still open (or is mid cross-thread send,
or has a pending `ws_deferred_free_cb` / send job):

- a queued-but-unexecuted `ws_send_on_owner` then dereferences a freed
  bufferevent (`job->bev`) → use-after-free;
- an already-queued `ws_deferred_free_cb` calls
  `bufferevent_get_base()` on a freed bev → use-after-free.

Recommended fix directions (not yet implemented):

- track active WebSocket connections per server and, during server
  teardown on each owner thread, run `ws_connection_cleanup()` +
  barriers *before* `evhttp_free`; or
- make server teardown fully asynchronous/refcounted so `evhttp_free`
  only happens when the connection table is empty; or
- document the hard requirement that callers close all WebSocket
  connections (or keep the server reachable) before dropping the last
  server reference.

### R2 (MED/HIGH) Blocking cross-thread server GC can deadlock

`httpd_server_cleanup` can be entered from a non-owner thread (any
worker Lua callback that lets its server userdata be collected). It
then blocks on condvars while queuing cleanup jobs on the main base and
other worker bases. A deadlock occurs when the *other* event loop is
currently executing a Lua-bound callback that is waiting on the global
Lua lock held by the GC thread:

```
worker A: holds Lua lock, inside __gc → waits for worker B loop job
worker B: loop runs a callback → waits for Lua lock (held by A)
```

Because all event loops can be stalled behind the single Lua lock at
any time, and there is no timeout on the condvar waits, this can hang
the process. The same concern applies to a cross-thread
`evhttp_del_accept_socket`/rebind.

Mitigations: perform server destruction/rebind from the main thread
while no worker Lua callbacks are pending, or (preferred) replace the
synchronous wait with an asynchronous refcounted teardown that does not
hold the Lua lock.

### R3 (MED) Dispatch flag vs loop-break window

`event_mgr_worker_once` checks `workers_accepting_dispatch` before
queueing; `event_mgr_workers_stop_threads()` clears the flag and then
loop-breaks. Between a successful `event_base_once()` and the loop
actually executing the job, loop-break can drop the queued event, so a
barrier (`pending_accepts`, `pending_worker_cleanups`, …) may never
reach zero → the waiting cleanup blocks forever. In practice this needs
a GC/cleanup racing the global embedder shutdown; still, adding a
timeout or an "abort job" mechanism to the barriers would make the
teardown robust.

### R4 (LOW/MED) Unsynchronized reads of mutable state

`request->ws_state` and `request->ws_bev` are read lock-free on the
owner thread (`ws_readcb`, direct-send path) while cross-thread code
writes them under `ws_mutex`; `lua_evhttp_request_websocket_state`
reads `ws_state` with no lock at all. Same for the global metrics
counters in `httpd_metrics.c`, which are incremented from every worker
thread. These are C11 data races (UB) though in practice single-word
loads on x86/ARM do not tear. Consider `_Atomic` for `ws_state` and
metrics, or route the readers through the mutex.

### R5 (LOW) Ordering of concurrent senders

A direct write on the owner thread and a marshaled job are ordered only
by the owner loop. Two different Lua threads calling `websocket_send`
concurrently can therefore observe their frames written in an order
different from call order (the direct write happens immediately while a
job waits for the owner loop). Frame *contents* stay intact; only
ordering across senders is not guaranteed. Single-sender ordering is
preserved.

### R6 (LOW) No explicit destroy for small sync objects

`Request.ws_mutex` is initialized per request and never destroyed (the
Request userdata has no `__gc`), and the server accept sync objects are
destroyed only on the full-cleanup path. Harmless on process exit but
not clean under repeated create/destroy churn; an owning finalizer or
explicit destroy at deferred-cleanup end would be tidy.

### R7 (INFO) Conservative leaks are intentional

When the owner loop is not running, `event_base_once` can still queue a
job that will never run (barriers never empty) or cleanup bails early.
The code deliberately keeps resources + pins alive until process exit in
those cases rather than freeing across threads. Expect "leaks at
shutdown" and treat them as the safe outcome, not a bug.

## 8. Usage constraints & best practices

- Start workers before binding servers
  (`workers_init(n)` once; HTTP distribution only activates when a
  pool exists and `worker` is omitted).
- Treat each connection's Lua request/response/WebSocket API as usable
  from any thread (sends are marshaled), but keep *native* HTTP server
  lifecycle operations (destroy/rebind, and anything that implicitly
  force-closes connections) on the main thread and outside Lua
  callbacks where possible.
- Keep servers reachable for as long as their WebSocket connections are
  open (see R1). When shutting down cleanly, first close every
  WebSocket connection (or drop clients), then drop the server
  reference.
- Do not rely on cross-sender ordering of WebSocket frames (R5).
- On OpenSSL < 1.1.0 the embedder must enable global CRYPTO locking
  callbacks before `workers_init`; on OpenSSL ≥ 1.1.0 this is not
  required.
- Do not run multi-worker HTTP distribution with code that was only
  validated single-threaded and that touches request internals directly
  from other threads without going through the public API.

## 9. Alignment with libevent guidance (httpd multi-threading)

Reviewed against the bundled libevent sources and its public headers.

### Where the model follows upstream guidance

1. **One dispatching thread per event_base.** `event.h` states that
   "only one thread can be dispatching a given event_base at a time"
   and offers exactly two sanctioned shapes for parallelism: a single
   base whose events add work to a work queue, or **multiple
   event_base objects**. luafan uses both halves of the advice:
   one event_base per worker (never shared across loop threads) plus
   an explicit cross-thread job queue (`event_base_once`) that moves
   work *to* the owning loop.
2. **Multiple evhttp instances instead of a shared one.** `http.h`
   documents `evhttp_accept_socket` as useful "to create a socket and
   then fork multiple instances of an http server, or when a socket has
   been communicated via file descriptor passing". The libevent HTTP
   layer itself is single-threaded and upstream exposes **no**
   thread-safe shared-evhttp API. The luafan patch follows the
   documented multi-instance shape: each worker owns a private
   `evhttp`; the main base only owns the listener; accepted sockets are
   "fd-passed" (in-process) into a worker instance on that worker's
   thread. The only upstream gap is that `evhttp_get_request` expects
   to run on its `evhttp`'s own base thread — which the patch preserves
   by executing the attach inside an `event_base_once` job on the
   worker, i.e. on the target base's owner thread.
3. **Thread support installed before any base is created.**
   `thread.h` requires lock callbacks to be set before an event_base is
   allocated. `event_mgr` runs `evthread_use_pthreads()` (once) before
   the main base is lazily created and again before every worker base
   exists (`workers_init`), so all bases are created lock-capable.
4. **Bufferevents are not shared across threads.** Upstream marks
   bufferevent thread safety as opt-in (`BEV_OPT_THREADSAFE`,
   `bufferevent_enable_locking`) and even then only protects individual
   calls, not frame-level state. luafan's stricter rule — every
   bufferevent/evhttp/connection object is touched only by its owner
   thread; other threads marshal data through jobs — is the model
   libevent's own documentation steers users toward for non-trivial
   per-connection state (WebSocket framing, deflate, close handshake).
5. **Cross-thread wakeup via `event_active`/`event_base_once`.**
   `event.h` calls out waking an `event_base_loop()` from another
   thread as a standard pattern; `event_base_once` is that mechanism in
   one shot. The once-event memory of a never-triggered event is freed
   by `event_base_free()` in 2.1, but the `arg` is not — luafan jobs
   therefore always own their payload and free it themselves, accepting
   a deliberate leak when the loop has already stopped (§R7).
6. **Teardown on the owner thread.** `evhttp_free` /
   `evconnlistener` ops have no cross-thread guarantee; luafan queues
   server destruction onto each base's owner thread instead of freeing
   from arbitrary Lua threads (safe direction, see R1/R2 for the gaps).

### Where the model deviates / should improve (httpd-specific)

| aspect | upstream-typical practice | current luafan | gap |
|---|---|---|---|
| accept fan-out | one listener, worker loop pulls accepted fds (nginx-style), or kernel `SO_REUSEPORT` with one listener per worker | one upstream-libevent listener per worker with `SO_REUSEPORT` | kernel owns distribution; accepted sockets stay on their listener's event base |
| callback duration | keep loop callbacks short, never block the loop | worker callbacks may block on the single global Lua lock (Lua is serialized VM-wide) | a slow/busy Lua handler stalls *its* worker's loop. Mitigate: enough workers for the expected Lua-blocking concurrency, or per-worker `lua_State` (large change, out of scope) |
| state sharing | one thread per object; share only via queues | enforced per-connection owner + marshaled jobs | correct, but R4 shows a few fields (`ws_state`, metrics) are still read without synchronization |
| server teardown | never free another thread's event state synchronously from a Lua callback | owner-thread cleanup jobs + barriers | R1 (force-free races live WS state) and R2 (condvar wait can deadlock against the Lua lock) |
| keep-alive affinity | connection pinned to the worker that accepted it | same (per-worker evhttp) | consistent; a worker stop drops only its own connections |

### Bottom line

The architecture is a faithful application of libevent's documented
multi-threading rules (per-base single dispatcher, multi-instance
httpd, fd passing, owner-thread teardown, cross-thread wakeup through
the event API), not a shared-base/parallel-dispatch misuse of libevent.
The remaining gaps are engineering ones — backpressure on accept
dispatch, non-blocking/asynchronous server teardown (R1/R2), and
synchronization hygiene on a few hot fields (R4) — rather than a wrong
threading model.
