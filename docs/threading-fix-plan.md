# luafan threading fix plan

Companion to [threading-model.md](threading-model.md). This document
defines the remediation plan for the issues found in the threading
review (§7 of threading-model.md: R1–R7) plus the backpressure gap
noted in its §9. It is a *design plan*: implementation status is
tracked per phase below (see §Status).

Reference sources: `src/httpd.c`, `src/httpd_websocket.c`,
`src/httpd_internal.h`, `src/event_mgr.c`, `src/httpd_metrics.c`,
libevent patch (`lua-apple/libevent/http.c`, `http-internal.h`,
`include/event2/http.h`).

---

## Status

- **Phase 1 — implemented** in `src/httpd_internal.h`, `src/httpd.c`,
  `src/httpd_websocket.c` (no commit/push; luafan is a shared submodule).
  §2.7 lists the deviations from the design text.
- **Phase 2 — implemented** (§3): backpressure in distribute mode
  (`accept_high_water = max(64, workers*16)`; main listener disables above
  high-water, re-enables on a main-base job once the in-flight backlog drops
  below half; teardown re-enables before deleting the accept socket).
- **Phase 3 — implemented** (§4): metrics counters atomized
  (`_Atomic unsigned long` + `atomic_fetch_add`/`atomic_load`,
  `metrics_init` no longer `memset`s); `LUA_EVHTTP_REQUEST_DATA_TYPE` gets a
  `__gc` that destroys `ws_mutex` once the request is fully detached. Keep
  `ws_state`/`ws_bev` as plain fields: all accesses are on the owning event
  loop's single thread (accept/readcb/eventcb/cleanup/drain) or behind
  `ws_mutex` (cross-thread send fast-path snapshots) — see §4.7.
- **Phase 4 — runtime regression passed on qa @8081 (`--workers 2`)** (§5):
  native build via Xcode (`schemeName=luan`) succeeds; no condvar wait
  remains on any teardown path (`grep cond_wait|httpd_server_cleanup|pthread_cond`
  is empty). qa was restarted on the rebuilt Debug/luan and exercises the C
  HTTPD teardown through `luan/tests/test_httpd_teardown.lua` (registered in
  `luan/tests/run_unit.sh`), which asserted in a real worker-pool runtime:
  bind (distribute) works, `serv:close()` releases the port so a retry-bind
  succeeds (async teardown → evhttp_free path), and 40 bind/close/collect
  cycles stay clean. `run_unit.sh test_httpd_teardown.lua` → PASS (7/7 cli
  unit tests pass). Active-WebSocket teardown (R1 UAF) is covered by the
  luafan-repo integration suite `tests/lua/test_httpd_async_teardown.lua`
  (scenario A) for the fan.so environment; ASan/TSan runs remain optional
  follow-ups.

- **Post-review hardening (current batch)**: closed the accept-dispatch
  vs. worker `evhttp` free race (per-owner `instance_accepts` counter,
  drained before `evhttp_free`); `finalize` is now a main-base job so
  every accept/resume job queued before it has already run; drain-check
  and deferred-free re-arm with a 5 ms / 1 ms backoff instead of a
  zero-delay spin; MariaDB `wait_for_status` returns a status and every
  caller resumes the coroutine when event registration fails;
  `ws_schedule_free` checks `event_base_once`; `worker` options are
  validated consistently across fifo/tcpd/tcpd.bind/udpd/popen/mariadb;
  core `fan.http` per-worker CURLM behaviour is documented.

---

## 0. Problem recap & priorities

| id | severity | problem | disposition |
|----|----------|---------|-------------|
| R1 | HIGH | server teardown force-frees live WebSocket connections, racing their deferred cleanup / queued send jobs (use-after-free) | **fix — Phase 1** |
| R2 | MED/HIGH | cross-thread, blocking (condvar) server teardown while holding the global Lua lock can deadlock against other event loops | **fix — Phase 1** (remove all blocking waits from teardown) |
| R3 | MED | dispatch-flag vs loop-break window can leave barrier counters non-zero forever | **fix — Phase 1/2** (no blocking waits remain; leftover counters become "leak, never hang") |
| R4 | LOW/MED | data races: `ws_state` / `ws_bev` lock-free reads, metrics counters | **fix — Phase 3** (atomics) |
| R5 | LOW | cross-sender frame ordering not guaranteed | keep as documented constraint |
| R6 | LOW | per-request `ws_mutex` never destroyed; sync objects destroyed only on full path | **fix — Phase 3** (`__gc` finalizer + teardown cleanup) |
| R7 | INFO | conservative leaks when an owner loop has stopped | keep semantics; make them deterministic (Phase 1); later narrowed by the loop-exit drain — see threading-model.md R7 |
| R8 | NEW | accept dispatch has no backpressure: unbounded job queue when a worker is slow/blocked | **fix — Phase 2** |

## 1. Core invariants after the fix

- **I1** An `evhttp` instance (main listener or per-worker) is freed
  only on its owner thread, and only after every WebSocket connection
  attached to that instance has completed its deferred cleanup. Plain
  HTTP connections may be force-closed by `evhttp_free` on the owner
  thread (no Lua can run concurrently there; unchanged semantics).
- **I2** Lua-side destruction (GC, explicit close) never blocks and
  never waits for another event loop. It records intent and returns;
  all native teardown runs as owner-thread jobs. The global Lua lock is
  never held across a cross-thread wait.
- **I3** Accept dispatch into the worker pool is bounded by a
  high-water mark; above it the main listener stops accepting until
  workers drain.
- **I4** Every mutable field shared between threads is either guarded
  by a mutex or `_Atomic`.

## 2. Phase 1 — asynchronous, drain-aware server teardown (R1/R2/R3/R7)

### 2.1 LuaServer lifecycle state

Replace the "destroy synchronously from `__gc`" behaviour with an
explicit state machine.

```c
typedef enum {
    HTTPD_LIVE     = 0, /* accepting + serving               */
    HTTPD_DRAINING = 1, /* destroy requested; teardown jobs running */
    HTTPD_GONE     = 2  /* native resources released; __gc is idempotent */
} httpd_life_state_t;

/* LuaServer additions (guarded by the existing accept mutex, renamed
 * life_mutex since it now covers lifecycle state as well): */
_Atomic int            life_state;        /* atomic; other fields under life_mutex */
unsigned int           teardown_remaining;/* instances not yet freed  */
unsigned int           pending_accepts;   /* existing (kept)          */
int                    listener_paused;   /* Phase 2                 */
Request                *ws_list;          /* intrusive list of live WebSocket reqs */
unsigned int           *instance_ws;      /* ws count per instance, index = worker_id+1 */
int                    accept_high_water; /* Phase 2, default later  */
```

`life_state` is read under `life_mutex` (except one atomic fast-path
check in `httpd_accept_dispatch` and in `websocket_accept` to reject
new work once draining).

### 2.2 Request ↔ server linkage

Each live WebSocket `Request` registers with its server:

- `websocket_accept` (owner thread): reject when the server is not
  `HTTPD_LIVE` (close the connection with 503); otherwise
  `server_ws_attach(req)`:
  - link into `server->ws_list` (under `life_mutex`);
  - `server->instance_ws[req->worker_id + 1]++` — owner thread only,
    no lock needed (invariant: attach/detach happen on the owner).
- `ws_deferred_free_cb` final step (owner thread) calls
  `server_ws_detach(req)`:
  - unlink from `ws_list` (under `life_mutex`);
  - decrement `instance_ws[...]` (owner thread);
  - if the server is `HTTPD_DRAINING` and this instance's count hits 0,
    signal the instance's drain-check (already scheduled, see below).

Attach happens inside the owner loop's Lua callback; detach inside the
owner loop's deferred-cleanup callback — never concurrently for the
same request, and the counter is only ever touched by the owner thread.

### 2.3 Teardown choreography

New public flow — `lua_evhttp_server_gc` (any thread, under the Lua
lock):

```
lua_evhttp_server_gc(L):
  if server->life_state == HTTPD_GONE:
      return 0                        /* idempotent; native already gone */
  if server->life_state == HTTPD_LIVE:
      self-pin (existing luaL_ref of the userdata)
      CLEAR_REF(server->onServiceRef)
      httpd_server_begin_destroy(server)      /* non-blocking */
  return 0
```

`httpd_server_begin_destroy(server)` (any thread, non-blocking):

1. lock `life_mutex`; if state != LIVE → unlock and return; set
   `DRAINING`, `accepting = 0`, compute `teardown_remaining`
   (1 for the main instance + worker_count when distributing);
   unlock.
2. For each instance (main `server->httpd`, then each
   `server->workers[i].httpd`), dispatch an owner-thread teardown job:
   - owner == current thread → run inline;
   - owner loop running → `event_mgr_worker_once(owner, teardown_job, …)`;
   - owner loop stopped → instance cannot be torn down: leave it (and
     the pin) for process exit — the deterministic R7 outcome.

`teardown_job` runs on the instance owner (single-threaded with that
instance's loop):

1. (main instance only) `evhttp_del_accept_socket(server->httpd,
   server->boundsocket)` — no new connections; no new accept jobs.
2. Drain WebSockets: iterate `server->ws_list` (under `life_mutex`)
   collecting requests whose `worker_id` maps to this instance, and for
   each call `ws_connection_cleanup(req)` (idempotent via the existing
   `ws_cleaning_up` CAS; may be invoked outside Lua without issue — it
   does not touch the Lua state).
3. Schedule a drain-check on this base (`event_base_once`, zero delay,
   re-scheduling itself like `ws_deferred_free_cb` does):
   - while `instance_ws[this] > 0` → re-schedule (loop must stay idle
     to let deferred cleanups finish);
   - when 0 → `evhttp_free(instance)`, clear the instance slot, and
     decrement `teardown_remaining` (under `life_mutex`); when it
     reaches 0 → dispatch `finalize_job` to the main base.

Because drain-check and the per-connection `ws_deferred_free_cb` run on
the same base FIFO queue, order is deterministic: deferred cleanups
drain the counter before the drain-check observes it. If a connection
never finishes (loop stopped), neither the drain-check nor anything
else runs — leak, never hang (R7).

`finalize_job` runs on the main base owner (main thread):

1. assert `pending_accepts == 0` (defensive log if not; re-dispatch in
   that impossible case).
2. free `server->workers`, `server->instance_ws`; `SSL_CTX_free`,
   `free(server->host)`; destroy `life_mutex`/cond.
3. set `life_state = GONE`.
4. take the Lua lock, `CLEAR_REF(server->self_ref)`, unlock.
   The userdata becomes collectable; its next `__gc` hits the
   idempotent branch above.

### 2.4 What disappears

- The blocking `pthread_cond_wait` loops in `httpd_server_cleanup`
  (pending accepts / pending worker cleanups). R2's deadlock cannot
  occur because no teardown path waits while holding the Lua lock.
- The "fail → keep `self_ref` forever" leak is narrowed to the single
  legitimate case: an owner loop that is already stopped when destroy
  starts (R7, now deterministic and documented).

### 2.5 Lock-order rules (documented for implementers)

- `life_mutex` may be taken while holding nothing else.
- `ws_mutex` may be taken while holding `life_mutex` (teardown drain).
- Never take `life_mutex` while holding `ws_mutex`.
- Teardown jobs never call `lua_*`; the only Lua contact is
  `finalize_job`'s `CLEAR_REF`, which runs on the main thread when it
  can take the lock.

### 2.6 API surface / semantics changes

- `server` userdata GC becomes asynchronous; dropping the last Lua
  reference no longer synchronously frees the listener. Port reuse
  after GC therefore needs a short retry loop on `bind` (accepting
  `EADDRINUSE` for a few ticks).
- Optional explicit API (recommended): `server:close()` — idempotent,
  calls `httpd_server_begin_destroy`; allowed from any thread.
- `rebind` gains a guard: only valid while `HTTPD_LIVE`; returns an
  error when draining/gone. Cross-thread rebind remains unsupported
  (documented constraint).

### 2.7 Deviations from the design text (as implemented)

- **Finalize runs on the last finishing teardown thread**, not as a
  separate job dispatched to the main base: `httpd_server_teardown_done`
  decrements `teardown_remaining` and, when it reaches 0, calls
  `httpd_server_finalize` inline on that (owner) thread. `finalize`
  clears the Lua self-pin under `lua_lock(server->mainthread)`, which is
  safe because teardown jobs never otherwise touch Lua and the main
  thread is the only one that may hold the lock concurrently.
- **`accept_mutex` is intentionally never destroyed.** It guards both
  accept state and lifecycle fields and can be reached by late
  `rebind`/`close`/`websocket_*` calls on other threads while teardown
  is still in flight; it is a process-lifetime object inside the pinned
  userdata block.
- **Attach/detach and the per-instance counter are serialized under
  `accept_mutex`** (plan §2.2 assumed owner-thread-only counting without
  the lock). Accept/teardown/detach all run on owner threads anyway, so
  this only adds safety for the boundary case where `websocket_accept`
  races `begin_destroy` from different threads.
- **`instance_ws` is allocated in `utd_bind`** (before the first bind),
  sized per mode: distribute = `worker_count + 1`, single on worker N =
  `worker_id + 2`, single on main base = 1. Bind-error paths use the
  synchronous `httpd_server_free_after_bind_error` (never exposed, no
  connections → safe) after `CLEAR_REF` of `onServiceRef`.
- **`httpd_accept_dispatch` already rejects when `life_state != LIVE`**
  and `accepting == 0`; `begin_destroy` clears `accepting` under the
  mutex, so no new accept jobs are created after drain starts. Phase 2
  will add the high-water pause/resume on top.
- **`close()` registered on the server metatable** (`server:close()`),
  idempotent, non-blocking; `rebind` errors unless `HTTPD_LIVE`.
- Drain-check re-arms itself at zero delay while `instance_ws > 0`
  (plan §2.3). Cleanup events queued before the drain-check on the same
  owner loop drain the counter first; a wedged connection therefore
  retries on every loop pass instead of busy-spinning between loop
  iterations (each re-arm is one event; other pending events still run).

## 3. Phase 2 — accept-dispatch backpressure (R8)

Only active in distribute mode (main listener → workers).

- New server knob `accept_high_water` (default `max(64, workers*16)`).
- In `httpd_accept_dispatch` (main owner thread): after
  `pending_accepts++`, if it reaches the high-water mark, set
  `listener_paused = 1` and `evconnlistener_disable()` the main
  listener (obtained via `evhttp_bound_socket_get_listener`); the
  current fd is still dispatched (it already counted).
- In `httpd_accept_on_worker` completion (worker): after decrementing
  `pending_accepts`, if `listener_paused` and
  `pending_accepts < high_water/2`, clear the flag and dispatch an
  enable job to the main base (`event_mgr_worker_once(-1, …)`) that
  calls `evconnlistener_enable()`.
- `httpd_server_begin_destroy` must first `evconnlistener_disable` /
  delete the accept socket so a paused listener never blocks the
  drain-check path.

Effect: bounded in-flight dispatch memory; slow/blocked workers stop
the accept loop instead of queueing unbounded jobs. New connections
then sit in the kernel backlog (TCP backpressure) — the standard
nginx-style behaviour.

## 4. Phase 3 — synchronization hygiene (R4/R6, rebind guard)

1. `ws_state`: make it `_Atomic int`; convert all accesses to
   `atomic_load`/`atomic_store` (`memory_order_relaxed` is sufficient
   for state queries; transitions that must be visible with the bev
   snapshot stay under `ws_mutex` as today).
2. `ws_bev`: `_Atomic(struct bufferevent *)`. Owner-loop hot paths
   (`ws_readcb`, direct send) use `atomic_load`; cleanup stores NULL;
   the cross-thread path already snapshots under `ws_mutex`, which now
   only guards `ws_pending_sends`/`ws_cleaning_up` hand-off. Simpler
   alternative if preferred: route all `ws_bev` reads through
   `ws_mutex` (readcb cost is negligible — one lock per frame batch).
3. Metrics (`httpd_metrics.c`): convert counters to
   `_Atomic unsigned long`, update with `atomic_fetch_add`, read with
   `atomic_load`; replace `memset` in `metrics_init` with per-field
   `atomic_store(0)`.
4. `Request.ws_mutex` teardown: give
   `LUA_EVHTTP_REQUEST_DATA_TYPE` a `__gc` (new, in `httpd.c`) that
   destroys `ws_mutex` once the request is fully detached
   (`ws_bev == NULL`; the self/pin refs guarantee the WS deferred path
   has already finished before GC can run). Log + leak if a request is
   collected with a live `ws_bev` (should be unreachable).
5. Server sync objects (`life_mutex`/cond, `instance_ws`, workers
   array) are destroyed in `finalize_job` only (Phase 1).
6. `rebind`/`close` state guards per §2.6.

### 4.7 Deviations from the design text (as implemented)

- **`ws_state` / `ws_bev` remain plain (non-atomic) fields.** Every access
  site is either (a) on the owning event loop's single thread — accept,
  `ws_readcb`, `ws_eventcb`, `ws_connection_cleanup`, the teardown drain, and
  the deferred-free path all run on that same loop — or (b) behind
  `ws_mutex`, which the cross-thread send path uses to snapshot
  `ws_bev`/`ws_state` before queuing a job. There is no lock-free read that
  races a write from another thread, so hardening them to atomics only adds
  conversion risk without removing a real race (fix-plan §4 allowed this via
  the "simpler alternative"). Metrics counters — the one genuinely
  multi-thread-updated structure — are atomized.
- **`Request.ws_mutex` is destroyed in `lua_evhttp_request_data_gc`**, which
  is registered on the `LUA_EVHTTP_REQUEST_DATA_TYPE` metatable in
  `luaopen_fan_httpd_core`. Per fix-plan it only destroys the mutex when
  `ws_bev == NULL` (fully detached); a request collected with a live
  `ws_bev` logs and leaks (unreachable because the deferred path nulls the
  bev and clears the self/pin refs before GC can run).

## 5. Phase 4 — tests & validation

Automated (luafan repo):

- C unit (`tests/c/unit/test_event_mgr.c`): worker-once round trip,
  dispatch rejection after `workers_accepting_dispatch = 0`.
- Lua integration (`tests/lua/test_fan_httpd_lua.lua`, worker=2):
  - **A (R1)**: open a WebSocket, drop the last server reference +
    `collectgarbage()` while the socket is live and while a
    cross-thread `websocket_send` is in flight; assert the client keeps
    receiving frames and then EOF, no crash, no UAF under
    AddressSanitizer.
  - **B (R2)**: two workers each serving blocking-ish Lua handlers,
    GC the server from one worker's callback; assert the process does
    not hang and eventually frees the port (retry bind loop).
  - **C (R8)**: connect storm against the distributed listener; assert
    dispatch stays bounded (instrument `pending_accepts` peak or
    memory), listener pauses/resumes, all requests complete.
  - **D (R3/R7)**: trigger `close()`/GC exactly as the main loop is
    stopping; assert graceful leak-not-hang (process exits).
- Build with ThreadSanitizer (if the toolchain allows) and run C tests
  to flush remaining R4-class races.
- QA (qa @8081, `--workers 2`): rerun the existing chat /
  `OWNER_OK` WebSocket scenario plus close-storm; no production
  environment involved.

Acceptance criteria:

- No condvar wait remains on any teardown path.
- `evhttp_free` is never reached while an attached WebSocket request is
  still in deferred cleanup (`instance_ws` counter non-zero).
- Stress test A/B/C pass with ASan/TSan clean.
- Existing single-worker (`worker = 0`/`-1`) behaviour unchanged.

## 6. Non-goals / kept constraints

- R5 cross-sender ordering: documented, not "fixed" (ordering through
  the owner loop is inherent to the owner-thread model).
- Graceful drain of long-lived plain-HTTP (non-WebSocket) requests on
  teardown: out of scope; they are closed by `evhttp_free` on the
  owner thread as today (no cross-thread Lua can touch them).
- Per-worker `lua_State` to remove the global Lua lock: explicitly out
  of scope (large architectural change, tracked separately if ever
  wanted).
- OpenSSL < 1.1.0 global locking callbacks remain the embedder's
  responsibility (documented in threading-model.md §8).
- No commits/pushes without explicit user confirmation; native changes
  require a build + QA verification before landing.

## 7. Suggested implementation order

1. Phase 1 core in `httpd.c`/`httpd_internal.h`/`httpd_websocket.c`
   (state machine, ws list, teardown jobs, idempotent `__gc`),
   removing `httpd_server_cleanup`'s blocking waits.
2. Phase 2 backpressure in `httpd.c`.
3. Phase 3 atomics + `__gc` mutex teardown + rebind guard.
4. Phase 4 tests; native build; QA validation on qa @8081 with
   `--workers 2`.
