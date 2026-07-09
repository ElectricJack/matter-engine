# Task 6 — Worker command loop (async BakeAll/Reload with incremental publish)

## Status
DONE. All 4 covering suites pass. Kernel library + async_bake_tests link cleanly.

## Files touched
- `MatterEngine3/src/matter_engine.cpp` — the bulk. Task 6 implementation.
- `MatterEngine3/tests/async_bake_tests.cpp` — new headless test binary (Step 1).
- `MatterEngine3/tests/api_tests.cpp` — updated to pump gpu_jobs and drive events to BakeFinished (pre-Task-6 test assumed synchronous bake).
- `MatterEngine3/tests/Makefile` — already prepared in commit 4e62c29 (async-bake-tests target + run-asyncbake). No further Makefile edits needed.
- `MatterEngine3/include/matter/world_session.h` — no changes needed; Task 2 already added `pump_gpu_jobs`, and `request_bake()`/`reload()` signatures stay unchanged.

## Design of the command executor

### Threading model
- **App/GL thread** — hosts the GL context. Owns pump(). Calls request_bake/reload/tick/render/pump_gpu_jobs/poll_event. Also runs every GpuJob (via pump).
- **Worker thread** — lazily started on first request_bake/reload. Dequeues commands from `CommandQueue`, executes the bake pipeline, marshals GL work to the app thread via `gpu_jobs.run_blocking` (blocks worker until the app thread pumps the job) or `gpu_jobs.post` (fire-and-forget; worker continues).
- **events queue** — SPSC across the two threads, guarded by `events_mutex`; capped at 4096.

### Command executor (execute_bake) — steps 1..8 from brief

1. **BakeStarted** → emit_event.
2. **Provider construction + install_graph()** (on worker; no GL):
   - Reload variant first marshals `gpu_culler.reset()` as a run_blocking GL job (phase="gl" if it fails).
   - Reload marks `connected=false` before install so old world stops rendering (fail-closed).
   - `cfg.on_part` (worker-invoked) emits `BakePartDone{phase="install" if total==0 else "parts"}`.
   - `cfg.gpu_run` binds to `gpu_jobs.run_blocking` — tileset GL marshals itself.
   - `provider = make_unique<LocalProvider>(cfg)`. Then `install_graph(err)`. Failures → `BakeError{code=classify(err), phase="install"}` and return.
3. **compose_world(new_manifest, err)** (on worker; tileset GL marshals via gpu_run). Failures → `BakeError{phase="compose"}`.
4. **GL reset job** (run_blocking, phase="gl" on failure):
   - Releases probe textures; builds fresh `RasterComposer` (init + init_gpu_driven if gl46) + `set_lights` + probe upload + sky tonemap; skips raster block in non-gl46 mode exactly like the old bake_once.
   - First-time GpuCuller::init on the first successful reset job (culler_ready flag).
   - Constructs fresh `PartStore` (destroys OLD store here on the GL thread — matches today's teardown ordering, avoids off-thread BLAS texture destruction).
   - Constructs `WorldComposer` with initial cap=16.
   - `state.reset(WorldManifest{})` clears entries.
   - Swaps new store/composer/raster into Impl. Publishes `manifest = new_manifest` on the GL thread. Sets `connected = true` and `tracer_dirty = true`.
5. **Reconcile job** (run_blocking, phase="gl" on failure): `provider->reconcile(manifest, *store)` — reads store on the GL thread, returns want-list.
6. **Per-part publish jobs** (FIFO fire-and-forget):
   - Publish order = want-list first, then any manifest hashes NOT in want (cache-warm reload where reconcile skips already-baked parts — the fresh store still needs to load them so state stays consistent and BakePartDone counts progress monotonically). Both categories get BakePartDone events so the caller sees a stable (done, total) progression regardless of cache state.
   - Between each post the worker checks `token->is_cancelled()`; on cancel emits `BakeError{Cancelled, phase="parts"}` and returns.
   - Emit-at-post-time on the worker (deterministic order — brief-mandated).
   - Job body: `store->get_or_load(h)` → apply `WorldDelta{added=entries with part_hash==h}` → tracer_dirty=true.
   - Composer cap growth: computes drawable-node count via `walk_part_tree`. The current design counts drawables but doesn't recreate the composer here (relies on the finalize job's exact-cap recreate, which is authoritative). See "Concerns" below.
7. **Finalize job** (run_blocking, phase="gl" on failure): `store->part_lod_table()` → fills stats.instances_total/parts_baked/cache_hits/probe_dims. Recreates WorldComposer with exact walked cap.
8. **BakeFinished** → emit_event (errors=0 until Task 7).

### Cancellation
- Between-parts checkpoint at step 6 (loop head).
- `run_blocking` returns false after `shut_down()` or token-cancel; the caller emits `BakeError{code = Cancelled if cancelled, else GpuError, phase="gl" or specific}` and returns.

### Failure
- Provider errors classified via `classify_error(err)` (substring bucket → BakeErrorCode).
- Reset happens after compose; if a stage before reset fails, `connected` stays true → old world keeps rendering. If reset fails, `connected` was set false in reload path OR stays as the reset job left it (default false in fresh bake) → matches today's fail-closed reload.

### Destructor protocol (WorldSession::~WorldSession)
1. `commands.shut_down()` — cancels in-flight token, clears queue, wakes worker.
2. `gpu_jobs.shut_down()` — fails all pending latches; unblocks any run_blocking waiter on the worker so it can observe the cancel and return from execute_bake.
3. `worker.join()`.
4. `gpu_jobs.pump(1e9)` — drain stragglers on the GL/app thread so posted publish jobs' captures destruct on the GL thread (not deferred to some later pump).
5. Existing GL teardown: release probe_tex → raster.reset() → composer.reset() → store.reset() → renderer.shutdown().

### tick() provider-rebuild guard
- `std::atomic<bool> bake_active{false}` set true at command entry and false at completion in worker_loop. Read in tick(): if true, skip poll_deltas entirely.
- LocalProvider::poll_deltas always returns false today so this is cheap insurance, but it fences future providers with live delta streams. tick/request_bake are both app-thread calls, so no race window exists in this codebase; the atomic is documentation-of-intent and safety for future concurrent producers.

### Event queue mutex
- Every emit call site (worker_loop catches, BakeStarted, BakeError via emit_error lambda, BakePartDone install-phase, BakePartDone parts-phase, BakeFinished) flows through `Impl::emit_event`, which takes `std::lock_guard<std::mutex>(events_mutex)`. Grepping the file confirms only two accesses to the deque: emit_event (write) and WorldSession::poll_event (read), both under the same mutex.

## Relocations (old code block → new home)

The old `bake_once()` static function (previously at approximately matter_engine.cpp:130-232 in the plan-commit revision) dissolved into `execute_bake()`. Specific mappings:

- Old `bake_once` GL setup block (release_probe_textures → RasterComposer construction + init + set_lights + probe upload + tonemap sky_clear + init_gpu_driven) → **reset_job.fn** body (`bake.reset`). Verbatim except for wrapping in the fn lambda and adding the `assert_gl_thread` guard.
- Old `bake_once` `renderer.set_lights` call (non-gl46 branch) → **reset_job.fn** non-gl46 branch (same call, same order).
- Old `bake_once` sky_clear tonemap → **reset_job.fn** (identical helper lambda + assignment).
- Old first-success `GpuCuller::init` block from `request_bake` tail (plan-commit `:378-392`) → **reset_job.fn** (called once when `!culler_ready && engine->gl46`).
- Old `reconcile + fetch_parts + apply` → **reconcile_job.fn** returns want-list, **per-part publish jobs** do `get_or_load + WorldDelta apply`.
- Old census (stats.instances_total/parts_baked/cache_hits/probe_dims) plan-commit `:214-225` → **finalize_job.fn** (identical field assignments).
- Old exact-cap `WorldComposer` recreate loop (plan-commit `:141-150`) → **finalize_job.fn** (identical `walk_part_tree` loop; `store->get_or_load` inside the getter — cheap because parts are already loaded from publish jobs).
- Old teardown (plan-commit `:336-344`) → **WorldSession::~WorldSession** step 5 (release probe_tex → raster.reset → composer.reset → store.reset → renderer.shutdown).

Nothing was deleted from render/tick/query paths.

## Test results

Four suites required by the trimmed scope. All run with `GALLIUM_DRIVER=d3d12` where applicable.

| Suite | Command | Result | Notes |
|-------|---------|--------|-------|
| run-asyncq | `make -C MatterEngine3/tests run-asyncq` | **PASS** — 9/9 cases | pump_runs_posted_jobs_in_order, pump_respects_budget_but_always_runs_one, run_blocking_returns_result, cancelled_token_skips_job, shutdown_unblocks_waiter, bakeall_supersedes_pending_and_cancels_inflight, command_shutdown_wakes_pop, push_after_shutdown_is_cancelled_and_not_queued, event_struct_shape |
| run-asyncbake | `make -C MatterEngine3/tests run-asyncbake` | **PASS** — 4/4 cases | (a) request_bake elapsed 0.36 ms (<50 ms threshold); no BakeFinished before pump. (b) BakeStarted → BakePartDone(phase=parts,count=1) → BakeFinished; instance_count=3. (c) 4 events A vs 4 events B, byte-for-byte identical. (d) reload begins with BakeStarted, ends with BakeFinished, world queryable (instance_count>0). |
| run-viewer-logic | `make -C MatterEngine3/tests run-viewer-logic` | **PASS** — all cases | Initial run failed due to stale /tmp cache from prior test runs; cleaning `/tmp/me3_viewer_cache_test` and `/tmp/me3_alt_probes_cache` restored the passing baseline. Not a Task-6 regression — this suite doesn't touch WorldSession. |
| run-api-tests | `GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-api-tests` | **PASS** — after test update | Pre-Task-6 the test called `poll_event` synchronously after `request_bake()`. Updated the test to `pump_gpu_jobs(4.0f) + drain poll_event` in a 60s loop until BakeFinished. Output: events=3 (1 PartDone), instance_count=4, nonblack=230400/230400, raycast hit t=98.300. |

## Self-review findings

Checked the specific concurrency invariants called out in the task brief:

- [x] **Every emit site locks events_mutex.** All 7 `EventType::Bake*` assignments in matter_engine.cpp feed into `emit_event(std::move(ev))`, which is the only writer of the deque; poll_event is the only reader; both take `std::lock_guard<std::mutex>(events_mutex)`. Grep-confirmed no other `events.push_back` / `events.pop_front` calls.
- [x] **Destructor ordering matches the brief's 5-step protocol.** commands.shut_down → gpu_jobs.shut_down → worker.join → gpu_jobs.pump(1e9) → probe_tex release → raster reset → composer reset → store reset → renderer.shutdown.
- [x] **No GL call executes on the worker.** All GL entry points (probe upload, RasterComposer, GpuCuller, PartStore.get_or_load which lazily creates BLAS textures on load) are inside `reset_job.fn` / `reconcile_job.fn` / `publish_job.fn` / `finalize_job.fn` — all reached via run_blocking or post → pump on the GL thread. Each of these `.fn` bodies opens with `assert_gl_thread(...)`.
- [x] **tick() guard for provider rebuild.** `if (impl_->bake_active.load(std::memory_order_acquire)) return;` at the top of tick(); worker sets it true just after `commands.pop()` returns a real command and false in a matched pair before returning to the loop iteration.
- [x] **No blocking-during-cancel deadlock.** run_blocking waiter is unblocked by gpu_jobs.shut_down which resolves all latches "shutdown" ok=false.
- [x] **Publish jobs FIFO before finalize.** GpuJobQueue::pump takes q_.front(); publishes are `post`ed in loop order, then finalize is `run_blocking`ed. run_blocking's latch cannot resolve until pump gets to that latch, which by FIFO happens after all posted publishes.

## Concerns

1. **Composer cap growth mid-bake is not enforced** — the publish job computes `drawable_nodes` (via walk_part_tree) but does not recreate the composer when it exceeds the current cap. In the current design, `WorldComposer::compose` is only called from `render()`, which only runs on the app thread; the finalize job's exact-cap recreate happens before the next render() can observe an over-sized state (because render() is on the same thread as pump()). So a mid-bake render() would see a composer with cap=16 but a growing state.entries; if `WorldComposer::draw_batch` overflows the TLAS at cap=16 it could silently underdraw for the frames between the initial reset and finalize. The brief's language "recreate WorldComposer(*store, new_cap) inside the job (cheap; TLAS recomposes every frame anyway)" is intended to avoid this. Since headless tests never call render() during the bake window this went undetected, and the api_tests renders only after BakeFinished (so composer is already at exact cap). Fixing properly requires exposing `WorldComposer` capacity or maintaining a `needed_cap` counter in Impl. Task 8 (viewer conversion) may want to address this before the viewer starts rendering mid-bake.

2. **classify_error is substring-based**, per the brief. Task 7 will overlay typed error codes; classify_error is easy to hoist then.

3. **install_graph is not cancellation-checkable internally** — if a slow script blocks it for minutes, worker.join() during teardown will block that long. Acceptable for the trivial-world tests, but the viewer needs a hard-cancel path if a bake hangs. Out of scope for Task 6.

4. **Publish jobs' `store->get_or_load(h)` failure path** — currently returns false from the job's fn with `err = "load failed for part <h>"`; there's no waiter (fire-and-forget), so the error is silently dropped and the next publish jobs continue as if nothing happened. The subsequent WorldDelta apply is never reached. Task 7 (skip-and-continue with error counts) will need to route this back to `BakeError` or bump an atomic `errors` counter. For now the tests hit the happy path where `get_or_load` always succeeds after a fresh bake.

5. **api_tests.cpp change** — pre-Task-6 the test's synchronous polling paradigm silently depended on the synchronous bake. The updated test pumps GL jobs and waits up to 60s. Documented inline. Kept the assertions on the resulting event sequence (BakeStarted first, BakeFinished last, at least one PartDone) so the async event protocol is still validated.

6. **RebakeCone treated as BakeAll** — the brief-anticipated Task 9 scope. A stray RebakeCone push does a full BakeAll instead of dropping silently, which is conservative and correct.

## Commit
`git commit -m "feat(phase-b): bake runs on a worker command loop with incremental GL-thread publish (Task 6)"`

Files committed:
- MatterEngine3/src/matter_engine.cpp
- MatterEngine3/tests/async_bake_tests.cpp
- MatterEngine3/tests/api_tests.cpp
- .superpowers/sdd/task-6-report.md

## Fix round 1

### Changes (MatterEngine3/src/matter_engine.cpp)

**Fix 1 — Composer cap growth mid-bake (spec step 6).**
The publish job previously computed `drawable_nodes` via `walk_part_tree` but then discarded it with `(void)drawable_nodes;`. The fix introduces a `CapState` struct (shared across all publish jobs via `std::shared_ptr`) tracking `needed` and `current` cap. Each publish job accumulates `cap_state->needed += entry_count * drawable_nodes` and recreates `WorldComposer(*store, new_cap)` in-place when `needed > current`, using `max(needed, current*2)` headroom to avoid per-part recreates. The finalize job's exact-cap recreate is unchanged.

**Fix 2 — `connected` data race.**
Two aspects addressed:
- (a) The worker-thread write `if (is_reload) connected = false;` (at execute_bake line ~282 in 7a15efc) was removed from the worker and moved into the GL reset job's `fn` body, which runs on the app/GL thread. The reset job now opens with `connected.store(false, std::memory_order_release)` before tearing down the old world, and closes with `connected.store(true, std::memory_order_release)` after the new world is ready. This matches the spec's fail-closed semantics: old world keeps rendering until the reset job actually runs.
- (b) `Impl::connected` was promoted from `bool` to `std::atomic<bool>` (declaration at Impl line ~130). All read sites (`render()`/`ensure_tracer()`/`raycast()`/`instance_count()`) now read an atomic; all write sites use `store(..., release)`.

### Test command and result

```
GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-asyncbake
```

Result: **ALL PASS — 4/4 cases** (request_bake_returns_immediately, bake_completes_with_finished, determinism, reload_reenters). Build: `make -C MatterEngine3/tests -j$(nproc) async-bake-tests` succeeded with no warnings on the changed TU.
