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
  `fan.loop()`). `event_mgr_is_loop_running()` reports whether that
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

- A process-wide **recursive** mutex (`fan_lua_lock.c`,
  `LockMainState`/`UnLockMainState`, thread-local depth mirror) is
  enabled exactly once, inside `event_mgr_workers_init()` **before**
  the first worker thread is spawned.
- Every event callback that touches Lua takes this lock
  (`lua_lock`/`lua_unlock` are real, recursive operations; nested
  locking is safe).
- The main thread releases all lock levels before parking inside
  `event_mgr_loop()` (`LuaLockSuspendForLoop`) and re-acquires them
  afterwards, so worker threads can run Lua callbacks while the main
  loop is idle.
- Request/connection handler code runs as coroutines created by
  `fan_cb_setup()`; `FAN_RESUME` (provided by the embedder, see
  LuanMac `LuaBridge.m`) owns/releases the lock around
  `lua_resume`.

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
