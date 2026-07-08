# Phase B: Async Bake + Progress Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the bake pipeline onto a cancellable worker thread with structured progress events and a GL-thread job queue pumped by the app, so the viewer stays interactive during a full bake and the world visibly assembles — plus wire the test-only `LiveEditSession` into production.

**Architecture:** Command-loop worker (approach 1 from the spec). One long-lived bake thread drains `{BakeAll, Reload, RebakeCone, Shutdown}` commands, executing the existing sequential provider code with cancel checkpoints between parts. All GL work marshals to the app thread via a `GpuJobQueue` drained by a new `WorldSession::pump_gpu_jobs(ms)`. Spec: `docs/superpowers/specs/2026-07-08-phase-b-async-bake-design.md` — **read it before starting any task.**

**Tech Stack:** C++17 (`std::thread`/`mutex`/`condition_variable` — no new dependencies), Make, raylib, OpenGL 4.6, QuickJS-ng.

## Global Constraints

- **Every GPU/viewer run needs `GALLIUM_DRIVER=d3d12`** (WSLg; without it Mesa falls back to llvmpipe GL 4.5 and the viewer FATALs).
- **MatterSurfaceLib is read-only** (genuine bug fixes only, surfaced as a scope decision in your report).
- **Scripted viewer runs must self-terminate** — use `MatterEngine3/tools/viewer_shots.sh`; never leave a viewer window running when a task ends.
- **The `STATS,` printf line format is append-only** (`MatterViewer/main.cpp:335`) — scripts parse fields by position.
- **No parallel scaffolding.** Extend the real pipeline; no demo/preview binaries.
- **Windows target:** the final verify task does a full clean rebuild (`rm -rf` the windows obj dir first). Individual tasks only need the Linux build unless stated.
- **Threading contract (spec):** all public `WorldSession` methods are app/GL-thread-only. The worker thread is kernel-internal. GL calls only ever run on the app thread.
- **Event struct is append-only** — new fields go after existing ones; existing field meanings don't change.
- Viewer binary runs with cwd = `MatterViewer/`. All paths below are relative to the repo root unless absolute.
- Commit after every task (specific paths only, never `git add -A`).

## Interface Reference (used across tasks)

Facts a task implementer needs but cannot see from their own task alone:

- **Bake call chain today (all synchronous):** `WorldSession::request_bake()` (`MatterEngine3/src/matter_engine.cpp:346`) → `Impl::bake_once(err)` (`:125-231`) → `LocalProvider::connect()` (`MatterEngine3/src/provider/local_provider.cpp:159-640`: ScriptHost create :218, `PartGraph::install` :288 — the heavy script-eval/bake, scatter/place :420-445, per-root `part_flatten::flatten_part` :328-365, expand :440, instance refs :378-416, **tileset GPU bake :450-513**, CPU probe bake :515-632) → `reconcile` → `fetch_parts` (`:653-670`, per-part `store.get_or_load(h)` + `cfg.on_part` callback) → `state.reset(manifest)` → composer-cap walk (`matter_engine.cpp:141-150`) → GL init block (`:154-210`: `release_probe_textures`, `RasterComposer` create + `init`, `set_lights`, `upload_probe_textures`, sky tonemap, `init_gpu_driven`) → stats/lods/connected (`:214-230`). `GpuCuller::init` on first success (`:378-392`).
- **GL inside the provider (must marshal):** `tileset::run_tileset_phase()` at `local_provider.cpp:492` (compute-shader tileset bake, `tileset_bake_gpu.cpp:100-246`) and `tileset_provider::load_slot()` at `:503` (GL texture upload). Everything else in the provider path is CPU-only (probe bake is CPU ray-marching; `get_or_load` is disk + CPU LOD work; ScriptHost uses a fresh isolated JSContext per call, no thread_local state).
- **cwd hazard:** `local_provider.cpp:202-215` brackets `PartGraph::install` in a process-wide `chdir(cache_root)` so `HostBaker`/ScriptHost write `parts/<hash>.part` relative paths. Task 3 removes this.
- **Types:** `viewer::WorldManifest{world_root_hash, instances: vector<WorldManifestEntry{instance_id, part_hash, transform[16], module}>, lights, probes}`; `viewer::WorldDelta{added: vector<WorldManifestEntry>, removed: vector<uint32_t>}`; `viewer::WorldState{reset(manifest), apply(delta), entries(), version()}` (`MatterEngine3/src/provider/world_source.h:18-52`). `viewer::LoadedPart{lod_blas, bound_radius, thresholds, children, lod_mesh_data (CPU-only), clusters, expansion}` (`MatterEngine3/src/render/part_store.h`). `PartStore::get_or_load(hash) -> const LoadedPart*` registers BLAS in the store's own BLASManager — LoadedParts are NOT portable across stores.
- **Live-edit seams** (`MatterEngine3/src/live_edit_interfaces.h`): `PartId = std::string` (module name), `ResolvedHash = std::string`; `GraphResolver{parts_for_file, ancestors, topo_order, roots_over, reresolve}`, `Baker{bake(p, h, budget_ms) -> BakeOutcome}`, `Flattener{reflatten(root)}`, `ErrorSink{report(LiveEditError)}`. `LiveEditSession` (`src/live_edit.h`) drives them; `run_rebuild(paths)` does cone/topo/fail-closed (called in topo order children-first: `reresolve` each, bake if not cached, then `reflatten` each affected root).
- **ScriptHost** (`MatterEngine3/src/script_host.h`): `resolve_hash(source, params_json, child_hashes*, n) -> uint64_t` (0 = fail); `bake_source(source, params_json, opts, child_hashes*, n, child_modules*, child_params*) -> BakeResult`; `eval_requires(...)`; `set_shared_lib_root(dir)` folds transitive `import 'shared-lib/x'` sources into hashes.
- **`part_graph`** (`MatterEngine3/src/part_graph.h/.cpp`): `PartGraph(ModuleResolver&, Baker&)`, `install(roots) -> InstallResult{ok, error, baked[], hits, root_hashes[]}` — DFS resolve + children-first bake, memo discarded after install. `FileModuleResolver{load_source, get_requires}` reads `<schemas_dir>/<module>.js`. `HostBaker{resolve_hash, cached, bake, bake_lod_variants}` (`part_graph.h:159-176`). `RecordingBaker` decorator at `local_provider.cpp:231-263` (records retopo settings during resolve).
- **Watchers** (`MatterEngine3/src/file_watcher.h`): `FileWatcher{add_watch(dir), poll(out_events), now_ms()}`; `InotifyWatcher` (Linux, real); `WinDirWatcher` throws (stubbed). `FakeWatcher` for tests.
- **Tests:** `MatterEngine3/tests/Makefile` — per-binary explicit source lists linking `../src/*.cpp` objects directly (not the .a), `run-<suite>` targets, headless suites use `GRAPHICS=OPENGL_33`. Temp-world pattern: `meadow_bake_tests.cpp` / `dev_live_edit_tests.cpp` (fresh `/tmp` sandbox, tiny schema .js files written by the test). `build-all.sh:195-212` lists the headless run targets.
- **Headless public-API sessions:** `EngineContext::create` with `desc.allow_gl_lt_46 = true` skips the GL4.6 gate (no GL touched; `engine->gl46=false`), and `bake_once` then skips the raster/GL init block. A tiny world with no tileset-flagged roots never reaches the tileset GL path → full bake with zero GL, no window. This is the async-test harness recipe.
- **Viewer:** `MatterViewer/main.cpp` — `open_and_bake()` helper `:166-185` (blocking event drain), FIFO pump `:241-284`, `session->tick()` `:287`, render block `:306-330`, STATS `:332-343`, reload handling `:371-391`.
- **New kernel-internal types built by this plan:** `matter_async::{CancelToken, GpuJob, GpuJobQueue, CommandQueue, Command, CommandKind}` + `register_gl_thread/assert_gl_thread` (Task 1, `MatterEngine3/src/async_bake.h`); `Event.phase/code/errors` + `matter::BakeErrorCode` (Task 2); `LocalProviderConfig.gpu_run/test_fault_hook` (Tasks 4/7); `LocalProvider::install_graph()/compose_world()` split (Task 5); `part_graph_snapshot::Snapshot` + production seams (Task 9).

---

### Task 1: Async primitives — `GpuJobQueue`, `CommandQueue`, `CancelToken`, GL-thread guard

**Files:**
- Create: `MatterEngine3/src/async_bake.h`, `MatterEngine3/src/async_bake.cpp`
- Modify: `MatterEngine3/Makefile` (add `src/async_bake.cpp` to the explicit `ME3_CPP` list ~line 77-98 and its mirror object in `ME3_OBJ` ~line 125-143)
- Test: `MatterEngine3/tests/async_queue_tests.cpp`, `MatterEngine3/tests/Makefile` (new binary + `run-asyncq` target, headless)

**Interfaces:**
- Produces (consumed by Tasks 4, 6, 7, 10):

```cpp
#pragma once
// Phase B async-bake primitives. Kernel-internal — NOT part of the matter/ API.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace matter_async {

struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool is_cancelled() const { return cancelled.load(std::memory_order_relaxed); }
};

// One unit of GL-thread work. fn returns false + fills err on failure.
struct GpuJob {
    std::string name;
    std::function<bool(std::string& err)> fn;
    std::shared_ptr<CancelToken> token;  // if set and cancelled, job is skipped (fails "cancelled")
};

// Thread-safe FIFO of GL jobs. Worker posts; the app thread pumps.
class GpuJobQueue {
public:
    void post(GpuJob job);                              // fire-and-forget
    bool run_blocking(GpuJob job, std::string& err);    // post + wait; false on fail/cancel/shutdown
    // App/GL thread: run whole jobs until ms_budget elapsed or queue empty.
    // Always runs at least one job when work is pending (progress guarantee).
    // Returns the number of jobs executed (skipped-cancelled jobs count).
    int pump(double ms_budget);
    void shut_down();      // fail all pending + future jobs; unblock all waiters
    bool idle() const;     // nothing pending
private:
    struct Pending;        // job + optional completion latch (mutex/cv/done/ok/err)
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> q_;
    bool shut_down_ = false;
};

enum class CommandKind { BakeAll, Reload, RebakeCone, Shutdown };
struct Command {
    CommandKind kind = CommandKind::BakeAll;
    std::vector<std::string> changed_files;   // RebakeCone only
    std::shared_ptr<CancelToken> token;       // filled by CommandQueue::push
};

// Single-consumer queue with supersession: BakeAll/Reload cancels the
// in-flight command's token and clears ALL pending commands. RebakeCone
// queues FIFO. Shutdown cancels everything and wakes the consumer.
class CommandQueue {
public:
    std::shared_ptr<CancelToken> push(Command c);
    bool pop(Command& out);   // blocks; false once shut down and drained
    void shut_down();
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Command> q_;
    std::shared_ptr<CancelToken> in_flight_;  // token of the command last popped
    bool shut_down_ = false;
};

// GL-thread guard. register_gl_thread() is called once by EngineContext::create;
// assert_gl_thread aborts with `where` in debug builds when called off-thread.
// Both are no-ops in NDEBUG builds.
void register_gl_thread();
void assert_gl_thread(const char* where);

} // namespace matter_async
```

**Steps:**

- [ ] **Step 1: Write failing unit tests** — `MatterEngine3/tests/async_queue_tests.cpp`, modeled on the assert-based style of `dev_live_edit_tests.cpp` (plain `assert` + `printf("ok <name>\n")` per case, `main()` runs all). Cases:
  1. `pump_runs_posted_jobs_in_order` — post 3 jobs appending to a vector; `pump(1000)` runs all 3 in order, returns 3.
  2. `pump_respects_budget_but_always_runs_one` — post 2 jobs that each sleep 5 ms; `pump(0.1)` returns 1 (min-one guarantee), second `pump(0.1)` returns 1.
  3. `run_blocking_returns_result` — worker `std::thread` calls `run_blocking` with a job returning false + err "boom"; main thread pumps; join; assert waiter saw false/"boom".
  4. `cancelled_token_skips_job` — post job with cancelled token; pump; assert fn never ran and pump returned 1.
  5. `shutdown_unblocks_waiter` — worker blocks in `run_blocking`; main calls `shut_down()` without pumping; join succeeds, waiter got false.
  6. `bakeall_supersedes_pending_and_cancels_inflight` — push RebakeCone ×2, pop one (in-flight), push BakeAll; assert: in-flight token cancelled, next pop returns the BakeAll, queue then empty.
  7. `command_shutdown_wakes_pop` — consumer thread in `pop`; `shut_down()`; join; pop returned false.
- [ ] **Step 2: Add the test binary to `MatterEngine3/tests/Makefile`** following an existing headless suite's pattern (explicit sources: `async_queue_tests.cpp ../src/async_bake.cpp`; `run-asyncq: async_queue_tests` target). Run `make -C MatterEngine3/tests run-asyncq` — expected: compile FAILURE (async_bake.h missing).
- [ ] **Step 3: Implement `async_bake.h/.cpp`** per the header above. Implementation notes: `Pending` holds the job plus `bool done/ok; std::string err; std::condition_variable cv` shared via `shared_ptr` so `run_blocking` waits on it; `pump` uses `std::chrono::steady_clock` around each job, loops while `elapsed < ms_budget || ran == 0`; `shut_down` marks every pending done/failed("shutdown") and notifies. `register_gl_thread` stores `std::thread::id` in a file-static atomic; `assert_gl_thread` compares under `#ifndef NDEBUG` and `fprintf(stderr,...)+abort()` on mismatch.
- [ ] **Step 4: Run** `make -C MatterEngine3/tests run-asyncq` — expected: all 7 cases print ok, exit 0.
- [ ] **Step 5: Add `src/async_bake.cpp` to `MatterEngine3/Makefile`** (ME3_CPP + ME3_OBJ lists), then `make -C MatterEngine3 -j$(nproc)` — expected: `libmatter_engine3.a` builds clean.
- [ ] **Step 6: Commit** — `git add MatterEngine3/src/async_bake.h MatterEngine3/src/async_bake.cpp MatterEngine3/tests/async_queue_tests.cpp MatterEngine3/tests/Makefile MatterEngine3/Makefile && git commit -m "feat(phase-b): async bake primitives — GPU job queue, command queue, cancel token (Task 1)"`

---

### Task 2: Public API surface — event fields, `BakeErrorCode`, `pump_gpu_jobs`, `WorldDesc.enable_live_edit`

**Files:**
- Modify: `MatterEngine3/include/matter/events.h`, `MatterEngine3/include/matter/world_session.h`, `MatterEngine3/src/matter_engine.cpp`
- Test: compile-level; extend `MatterEngine3/tests/async_queue_tests.cpp` with one struct-shape case

**Interfaces:**
- Produces (consumed by Tasks 6-10 and the viewer):

```cpp
// events.h — replace file body with:
#pragma once
#include <string>

namespace matter {

enum class EventType { BakeStarted, BakePartDone, BakeFinished, BakeError };

// Structured bake-error classification (Phase B). None on non-error events.
enum class BakeErrorCode { None, Cancelled, OutOfMemory, ScriptError, GpuError, IoError, Internal };

struct Event {
    EventType type = EventType::BakeStarted;
    std::string module;        // BakePartDone/BakeError: part module name (may be empty)
    int done = 0, total = 0;   // BakePartDone counters (total 0 = indeterminate phase)
    std::string message;       // BakeError: error detail
    // --- Phase B additions (struct is append-only) ---
    std::string phase;         // "install" | "compose" | "parts" | "gl" | "cone" | ""
    BakeErrorCode code = BakeErrorCode::None;   // BakeError classification
    int errors = 0;            // BakeFinished: failed-part count (skip-and-continue)
};

} // namespace matter
```

`world_session.h`: add to `WorldDesc`: `bool enable_live_edit = false;  // watch schemas/shared-lib dirs, cone-rebake on save (Linux inotify; no-op elsewhere)`. Add method after `render(...)`:

```cpp
    // Phase B: run queued GL-thread bake work for up to ms_budget milliseconds.
    // Call once per frame on the thread that owns the GL context. Whole jobs
    // only (no mid-job slicing); always makes progress when work is queued.
    void pump_gpu_jobs(float ms_budget);
```

Update the `request_bake()` comment: "Phase B: asynchronous — enqueues a bake and returns immediately. Progress arrives via poll_event(); GL-side work runs inside pump_gpu_jobs(). A new request_bake()/reload() supersedes (cancels) an in-flight bake." Same note on `reload()`.

**Steps:**

- [ ] **Step 1:** Apply the three header edits above. In `matter_engine.cpp`, add member `matter_async::GpuJobQueue gpu_jobs;` to `WorldSession::Impl` (include `"async_bake.h"`), and implement:

```cpp
void WorldSession::pump_gpu_jobs(float ms_budget) {
    impl_->gpu_jobs.pump((double)ms_budget);
}
```

  (Nothing posts to it until Task 6 — pump of an empty queue returns 0.)
- [ ] **Step 2:** Add a shape test to `async_queue_tests.cpp` (constructs `matter::Event`, sets `phase/code/errors`, asserts defaults `code==BakeErrorCode::None`, `errors==0`) — include `matter/events.h`; add `-I../include` if the suite lacks it. Run `make -C MatterEngine3/tests run-asyncq` — expected: pass.
- [ ] **Step 3:** `make -C MatterEngine3 -j$(nproc) && make -C MatterViewer -j$(nproc)` — expected: both build (viewer unaffected; it doesn't call the new method yet).
- [ ] **Step 4: Commit** — `git commit -m "feat(phase-b): public API — structured BakeErrorCode, event phase/errors fields, pump_gpu_jobs, enable_live_edit (Task 2)"` (add the 4 touched files + test).

---

### Task 3: Make the bake path cwd-independent (remove the `chdir` bracket)

The worker thread cannot `chdir` (process-wide; would corrupt relative paths — `shaders/`, `cache/` — on the render thread mid-frame).

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.cpp:202-215` (the chdir bracket), `MatterEngine3/src/part_graph.h/.cpp` (`HostBaker` parts_dir handling), `MatterEngine3/src/script_host.h/.cpp` (only if bake_source writes cwd-relative paths internally — trace `HostBaker::bake` → where the `.part` file path is composed)
- Test: `MatterEngine3/tests/` — extend the graph/bake suite (`run-graph` or the suite owning `HostBaker` tests) with a foreign-cwd case

**Steps:**

- [ ] **Step 1: Trace the write path.** Read `part_graph.cpp:301-330` (`HostBaker`) and follow where `parts/<hash>.part` is composed. Identify every path that assumes cwd == cache_root (bake write, `cached()` check, `bake_lod_variants`, any script-host artifact write).
- [ ] **Step 2: Write the failing test** — in the suite owning HostBaker/PartGraph tests: create a temp sandbox, `chdir("/")` (test-local; tests are single-threaded), run a `PartGraph::install` for a trivial one-part world with an **absolute** cache_root, assert `parts/<hash>.part` exists under the absolute cache_root. Run it — expected: FAIL (artifact written to `/parts/...` or bake error).
- [ ] **Step 3: Fix** — `HostBaker` already receives `parts_dir` (constructor arg, `part_graph.h:161`): make every composed path `parts_dir_ + "/" + ...` absolute (callers pass absolute; `LocalProvider` builds `abs_cache_root` already — see `flatten_one` usage of `abs_cache_root` at `local_provider.cpp:347`). Delete the `chdir` bracket at `local_provider.cpp:202-215`. If ScriptHost internally writes cwd-relative artifacts, plumb the absolute dir through `BakeOptions` rather than chdir.
- [ ] **Step 4: Run the full headless bake suites** — `make -C MatterEngine3/tests run-graph run-graph-integration run-script && make -C MatterEngine3/tests run-meadowbake` (use the exact existing target names from `build-all.sh:195-212`). Expected: all pass, including the new foreign-cwd case.
- [ ] **Step 5: Commit** — `git commit -m "fix(phase-b): cwd-independent bake writes — drop the install chdir bracket (Task 3)"`

---

### Task 4: GPU-executor seam in the provider (tileset bake marshaling point)

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.h` (config field), `MatterEngine3/src/provider/local_provider.cpp:450-513` (tileset phase + slot upload)
- Test: existing tileset suites (`run-tilesetmeadowmanifest` etc. from `build-all.sh`) — behavior-preserving refactor

**Interfaces:**
- Produces: `LocalProviderConfig` gains

```cpp
    // Phase B: run `fn` on the GL thread and wait for completion. Null => run
    // inline on the calling thread (synchronous callers, tests).
    std::function<bool(const char* name,
                       std::function<bool(std::string& err)> fn,
                       std::string& err)> gpu_run;
```

**Steps:**

- [ ] **Step 1:** Add the field. In `local_provider.cpp`, wrap the two GL sections — the `tileset::run_tileset_phase()` call (`:492`) and the `tileset_provider::load_slot()` + slot-binding block (`:503-510`) — each as one closure routed through a small local helper:

```cpp
    auto run_gl = [&](const char* name, std::function<bool(std::string&)> fn,
                      std::string& e) -> bool {
        if (cfg_.gpu_run) return cfg_.gpu_run(name, std::move(fn), e);
        return fn(e);   // inline (synchronous path unchanged)
    };
```

  Capture everything the closures need by reference — the provider blocks until `gpu_run` returns, so stack lifetime is safe. Prefer ONE closure per tileset (bake + upload together) so granularity is per-tileset (the spec's "granularity knob").
- [ ] **Step 2:** Add `matter_async::assert_gl_thread("tileset_bake")` at the top of `bake_tileset_gpu()` (`MatterEngine3/src/tileset_bake_gpu.cpp:100`) and `tileset_provider::load_slot` (`MatterEngine3/src/render/tileset_provider.cpp:107`). Also add guards at the other bake-path GL entry points: `RasterComposer::init`, `init_gpu_driven`, `upload_probe_textures`, `GpuCuller::init` (find each definition; one line each, include `"async_bake.h"`).
- [ ] **Step 3:** `make -C MatterEngine3 -j$(nproc)`, then run the tileset + GPU suites exactly as `build-all.sh` does (headless ones always; GPU ones with `GALLIUM_DRIVER=d3d12`). Expected: green — no behavior change with `gpu_run == null`.
- [ ] **Step 4: Commit** — `git commit -m "feat(phase-b): gpu_run executor seam in LocalProvider + assert_gl_thread guards (Task 4)"`

---

### Task 5: Split `LocalProvider::connect()` into `install_graph()` + `compose_world()`; install-phase progress

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.h/.cpp`
- Test: existing bake suites (behavior-preserving); one new assertion on install-phase progress events

**Interfaces:**
- Produces (consumed by Tasks 6, 9, 10):

```cpp
class LocalProvider : public WorldProvider {
public:
    // connect() == install_graph() + compose_world() (unchanged external behavior).
    bool connect(WorldManifest& out, std::string& err) override;
    // Heavy phase: ScriptHost + PartGraph::install (script eval, mesh, per-part bake).
    bool install_graph(std::string& err);
    // Post-install: scatter/place, expand, per-root flatten, instance refs,
    // tileset phase (via gpu_run), probe bake. Reusable after a cone rebake.
    bool compose_world(WorldManifest& out, std::string& err);
```

- `cfg.on_part` now ALSO fires during install for every freshly-baked node, with `total = 0` (indeterminate): extend `RecordingBaker` (`local_provider.cpp:231-263`) — its `bake(...)` override increments a counter and invokes `cfg_.on_part(module, ++n, 0)` before delegating. (Requires `bake()` to know the module — it's in the existing signature's `child_modules`-adjacent args; check `part_graph.h:165` — module is derivable from the source/params the decorator sees at `resolve_hash`; record last-resolved module in the decorator.)

**Steps:**

- [ ] **Step 1:** Mechanically split `connect()` at the seam after `PartGraph::install` + retopo-map completion (~`local_provider.cpp:263`... scatter begins ~`:270` with `read_manifest`): move `:159-~263` into `install_graph()`, the rest into `compose_world(out, err)`. Members that cross the boundary (ScriptHost, graph install results, `retopo_by_hash`, manifest roots) become provider members. `connect()` = `return install_graph(err) && compose_world(out, err);`.
- [ ] **Step 2:** Wire install-phase `on_part` via `RecordingBaker::bake` as described. Guard: only call when `cfg_.on_part` set.
- [ ] **Step 3:** Extend one existing headless bake test: install a fresh (cache-cold) tiny world with an `on_part` recorder, assert ≥1 callback with `total == 0` during install and the final fetch-phase callbacks still carry `total == want.size()`.
- [ ] **Step 4:** Run the headless bake suites (`run-graph run-graph-integration run-meadowbake run-tilesetmeadowmanifest` per `build-all.sh` names) — expected green.
- [ ] **Step 5: Commit** — `git commit -m "refactor(phase-b): split LocalProvider::connect into install_graph + compose_world; install progress (Task 5)"`

---

### Task 6: Worker command loop — async `BakeAll`/`Reload` with incremental publish

The core task. `request_bake()`/`reload()` enqueue and return; the worker executes `bake_once` logic with GL sections as jobs; parts publish incrementally so the world visibly assembles.

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp` (the bulk), `MatterEngine3/include/matter/world_session.h` (comments only)
- Test: `MatterEngine3/tests/async_bake_tests.cpp` (new binary + `run-asyncbake`, headless), `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: Task 1 primitives, Task 4 `gpu_run`, Task 5 split.
- Produces: the async event protocol all later tasks and the viewer rely on:
  - `request_bake()`/`reload()` → push `BakeAll`/`Reload`; **supersession**: `CommandQueue::push` cancels in-flight + clears pending.
  - Worker command execution order for BakeAll/Reload:
    1. emit `BakeStarted`
    2. `provider = make_unique<LocalProvider>(cfg)` (cfg.gpu_run bound to `gpu_jobs.run_blocking`), `install_graph()` on worker (`phase:"install"` progress via on_part)
    3. `compose_world(manifest, err)` on worker (`phase:"compose"`; tileset GL marshals itself via gpu_run)
    4. **GL reset job** (`run_blocking`): `release_probe_textures`; recreate `RasterComposer` + `init` + `init_gpu_driven` + `set_lights` + probe upload + sky tonemap (relocate `matter_engine.cpp:154-210` verbatim into the job); `GpuCuller::init` on first success (`:378-392` logic); clear `state` (use `state.reset(WorldManifest{})` then re-set lights/probes fields the composer reads — check what `reset` copies; entries are the only content); create `WorldComposer` with a small initial cap (16); set `connected = true`; `tracer_dirty = true`. Non-gl46 mode: skip the raster block exactly as `bake_once` does today.
    5. **Reconcile job** (`run_blocking`, returns want-list to the worker): `want = provider->reconcile(manifest, *store)` — store lives on the app thread, so reconcile reads it there. Create the fresh `PartStore` inside the GL reset job (step 4) and swap it in there too (old store destroyed on app thread — GL-safe, matches today's teardown ordering).
    6. **Per-part publish jobs** (fire-and-forget, FIFO): for each hash `h` in want order — worker checks `token->is_cancelled()` between posts (the between-parts checkpoint), emits `BakePartDone{module, done, total, phase:"parts"}` at post time (deterministic), posts job: `store->get_or_load(h);` then `WorldDelta d; d.added = manifest entries with part_hash == h; state.apply(d);` then `tracer_dirty = true;` and **composer cap growth**: `needed_cap += (entries for h) × drawable nodes` — when it exceeds the current cap, recreate `WorldComposer(*store, new_cap)` inside the job (cheap; TLAS recomposes every frame anyway). Compute drawable count from the freshly loaded part via `viewer::walk_part_tree` (all children now loadable from disk).
    7. **Finalize job** (`run_blocking`): `lods = store->part_lod_table()`; fill `stats` census fields (`instances_total/parts_baked/cache_hits/probe_dims` — relocate `:214-225`); final exact-cap composer recreate using the original cap walk (`:141-150`, everything already loaded → cheap).
    8. emit `BakeFinished{errors: <count>}` (errors stays 0 until Task 7 wires skip-and-continue).
  - Cancellation observed: between parts (step 6) and via `run_blocking` returning false after `shut_down`/token cancel → worker emits `BakeError{code: Cancelled, phase}` and returns to the command loop.
  - Failure: any stage fails → emit `BakeError{code: classify(err), message, phase}`; `connected` stays whatever the reset job left (fail before reset = old world keeps rendering; after = empty world — matches today's fail-closed reload).
  - **Event queue is now mutex-guarded**: add `std::mutex events_mutex` to Impl; every emit site and `poll_event` lock it. Keep the 4096 cap.
  - **Destructor protocol** (`WorldSession::~WorldSession`): (1) `commands.shut_down()` (cancels in-flight token), (2) `gpu_jobs.shut_down()` (unblocks any `run_blocking` waiter), (3) `worker.join()`, (4) `gpu_jobs.pump(1e9)` to drain stragglers on the GL thread, (5) existing GL teardown (`:336-344`).
  - Worker thread: `std::thread` member, started lazily on first `request_bake()`/`reload()`. Top-level per-command `try { ... } catch (std::bad_alloc&) { emit BakeError{OutOfMemory} } catch (std::exception& e) { emit BakeError{Internal, e.what()} }`.

**Steps:**

- [ ] **Step 1: Write the failing headless test harness** — `async_bake_tests.cpp`. Harness helper (reused by Task 7):

```cpp
// Tiny world sandbox: temp dir with schemas/<Box.js> (trivial voxel part, copy the
// minimal schema pattern from meadow_bake_tests.cpp), world_data/<World>/world.manifest
// placing Box a few times, empty shared-lib dir, fresh cache dir.
// Session: EngineDesc{cache_root=<tmp>/cache, allow_gl_lt_46=true} -> EngineContext
// -> open_world -> loop { session->pump_gpu_jobs(4.0f); drain poll_event into log; }
// until BakeFinished/BakeError or 60s timeout.
```

  Cases: (a) `request_bake_returns_immediately` — wall-clock `request_bake()` < 50 ms on a cache-cold world, and events subsequently arrive while pumping; (b) `bake_completes_with_finished` — sequence starts `BakeStarted`, ends `BakeFinished`, ≥1 `BakePartDone` with `phase=="parts"`, and `instance_count() > 0` after; (c) `determinism` — run the whole bake twice against two fresh caches; assert the recorded `{type,module,done,total,phase}` sequences are identical; (d) `reload_reenters` — after (b), `reload()` + pump loop → second `BakeStarted...BakeFinished`, world still queryable.
  Add binary + `run-asyncbake` to `tests/Makefile` (headless; link the needed `../src/*.cpp` set — start from the meadow-bake suite's list + `async_bake.cpp` + `matter_engine.cpp` + render sources it pulls; iterate until it links). Run — expected: FAIL (request_bake still synchronous blocks, or events missing phase).
- [ ] **Step 2: Implement** the worker/command loop conversion in `matter_engine.cpp` per the Interfaces block above. Key relocations, not rewrites: steps 4/7 lift `bake_once` code verbatim into jobs; `bake_once` itself dissolves into the command executor. Delete nothing that render/tick/query paths use. `tick()` keeps its provider `poll_deltas` call but must tolerate `provider` being rebuilt by the worker — guard: take a `std::shared_ptr<LocalProvider>` (promote the member) copied under a small mutex, or simpler: skip `poll_deltas` while a bake command is active (add `std::atomic<bool> bake_active`). LocalProvider::poll_deltas always returns false today (`local_provider.h:39`), so the guard is cheap insurance, not a behavior change.
- [ ] **Step 3:** `make -C MatterEngine3 -j$(nproc) && make -C MatterEngine3/tests run-asyncbake` — expected: all 4 cases pass.
- [ ] **Step 4:** Full headless regression: run every `run-*` target `build-all.sh` lists for MatterEngine3 — expected: baseline-only failures (none new).
- [ ] **Step 5: Commit** — `git commit -m "feat(phase-b): bake runs on a worker command loop with incremental GL-thread publish (Task 6)"`

---

### Task 7: Cancellation, shutdown, OOM injection, skip-and-continue

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp` (error classification, skip-and-continue), `MatterEngine3/src/provider/local_provider.h/.cpp` (`test_fault_hook`, per-part error policy), `MatterEngine3/src/part_graph.h/.cpp` (install continues past a failed node — see Step 3)
- Test: extend `MatterEngine3/tests/async_bake_tests.cpp`

**Interfaces:**
- Produces: `LocalProviderConfig.test_fault_hook: std::function<void(int part_index)>` — invoked once per part processed (install bakes and fetch loads); may throw. Skip-and-continue policy: a part that fails to bake/load emits `BakeError{code, module, phase}` and the bake proceeds; `BakeFinished.errors` = failed count.

**Steps:**

- [ ] **Step 1: Failing tests first** (extend `async_bake_tests.cpp`):
  1. `supersede_cancels_inflight` — start a bake on a cache-cold world; after the first `BakePartDone`, call `request_bake()` again; assert event stream shows `BakeError{code:Cancelled}` then a fresh `BakeStarted` → `BakeFinished`.
  2. `destructor_mid_bake_joins` — start a bake, destroy the session after the first event WITHOUT pumping further; the test must complete in < 10 s (deadlock regression).
  3. `oom_injection_skips_part` — `test_fault_hook` throws `std::bad_alloc` at part index 1; assert one `BakeError{code:OutOfMemory}`, then `BakeFinished` with `errors == 1`, and `instance_count() > 0` (survivors render).
  4. `broken_script_skips_part` — sandbox schema with a syntax error in one of two parts: `BakeError{code:ScriptError, module}`, `BakeFinished{errors:1}`, other part queryable.
  Run — expected: FAIL.
- [ ] **Step 2: Cancellation + shutdown** — already structurally in place from Task 6; make the checks real: worker checks the token between publish-job posts and between install/compose stages; classify `run_blocking` "shutdown"/"cancelled" failures as `Cancelled`. Verify test 1-2 pass.
- [ ] **Step 3: Skip-and-continue** — in the install path, a failed `HostBaker::bake` currently aborts `PartGraph::install`; change policy: record the failure (module + error) in `InstallResult` (new `failed[]` vector), skip the node's dependents' bakes but continue siblings (a parent whose child failed also lands in `failed[]` with cause "missing child"). In the fetch loop (`local_provider.cpp:653-670`), a `get_or_load` null return appends to a `failed` list instead of returning false. `compose_world` skips placing/flattening roots in `failed`. The worker emits one `BakeError` per failure (code `ScriptError` for script messages, `IoError` for load failures, `OutOfMemory` when the message says bad_alloc) and counts them into `BakeFinished.errors`. `test_fault_hook` fires per part in both loops; exceptions from it are caught by the same per-part handler (that's the injection point).
- [ ] **Step 4:** Run `run-asyncbake` (all cases) + full headless regression — green. **Check:** the existing suites that assert whole-bake failure on a broken part (if any exist in graph tests) — update them to the new policy deliberately and note it in the task report.
- [ ] **Step 5: Commit** — `git commit -m "feat(phase-b): cancellation + shutdown protocol + OOM safety net + per-part skip-and-continue (Task 7)"`

---

### Task 8: Viewer conversion — pump per frame, interactive bake, gates stay green

**Files:**
- Modify: `MatterViewer/main.cpp`
- Verify: screenshot gate vs Phase A refs, FIFO smoke

**Steps:**

- [ ] **Step 1:** Convert `open_and_bake()` (`MatterViewer/main.cpp:166-185`): create session + `request_bake()` + return immediately (no drain loop). Rename to `open_world_and_start_bake()`.
- [ ] **Step 2:** In the frame loop after `session->tick()` (`:287`), add `session->pump_gpu_jobs(4.0f);`. Add a non-blocking event drain right after: print `bake %d/%d %s` for `BakePartDone`, `bake finished (%d errors)` for `BakeFinished`, `bake error [%s]: %s` for `BakeError` (module + message). Keep printf-based — loading-screen UI is Phase C.
- [ ] **Step 3:** Reload handling (`:371-391`): `session->reload()` + clear the flag — no drain loop; events surface through the per-frame drain. Delete the now-dead blocking drains.
- [ ] **Step 4:** **Tooling compatibility check** — read `MatterEngine3/tools/viewer_shots.sh` (readiness poll at ~line 47-50): it waits/sleeps before shots; confirm the wait covers async bake completion. If it keys off a printed line, keep printing the same line at `BakeFinished`. If it's a fixed sleep, the async bake finishes within the same wall-clock (same work) — no change needed. Document which in the task report.
- [ ] **Step 5: Interactive verification (the exit-criterion-1 check):** `cd MatterViewer && rm -rf cache && GALLIUM_DRIVER=d3d12 MATTER_CMD_FIFO=/tmp/mv.fifo ./viewer` via the FIFO workflow — during the cold bake, send `cam` moves through the FIFO and confirm the window renders (sky + accumulating parts) and responds while parts pop in; then `quit`. Use a scripted run that self-terminates (pattern from `tools/meadow_sweep.sh`). Report what you observed.
- [ ] **Step 6: Screenshot gate:** `GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh phaseb /tmp/phaseb-shots` then `python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_<pose>.png /tmp/phaseb-shots/phaseb_<pose>.png` for all 5 poses — expected: MATCH on all. Compare STATS counters against `/home/jkern/phase-a-refs/ref_stats.log` (exact instance/triangle counters).
- [ ] **Step 7:** `bash MatterEngine3/tools/grep_gate.sh` — expected: clean (viewer only gained `matter/` API usage).
- [ ] **Step 8: Commit** — `git commit -m "feat(phase-b): viewer pumps GPU jobs per frame; interactive during bake (Task 8)"`

---

### Task 9: Live-edit production seams — graph snapshot + `GraphResolver`/`Baker`/`Flattener`

**Files:**
- Create: `MatterEngine3/src/part_graph_snapshot.h`, `MatterEngine3/src/live_edit_prod.h`, `MatterEngine3/src/live_edit_prod.cpp`
- Modify: `MatterEngine3/src/part_graph.h/.cpp` (install records the snapshot), `MatterEngine3/src/provider/local_provider.h/.cpp` (owns + exposes the snapshot), `MatterEngine3/Makefile` (new .cpp)
- Test: `MatterEngine3/tests/live_edit_prod_tests.cpp` (+ Makefile target `run-liveprod`, headless)

**Interfaces:**
- Produces (consumed by Task 10):

```cpp
// part_graph_snapshot.h
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace part_graph_snapshot {

struct Node {
    std::string module;                       // live_edit::PartId
    std::string source_path;                  // absolute <schemas_dir>/<module>.js
    std::string params_json;                  // canonical params at install
    std::vector<std::string> children;        // child module names (deduped)
    std::vector<std::string> shared_imports;  // shared-lib module names found in source
    uint64_t resolved_hash = 0;
    bool is_root = false;
};

struct Snapshot {
    std::map<std::string, Node> nodes;                           // by module
    std::map<std::string, std::vector<std::string>> by_file;     // abs source path -> modules
    std::map<std::string, std::vector<std::string>> by_import;   // shared-lib module -> importer modules
    std::vector<std::string> parents_of(const std::string& module) const;  // reverse-edge helper
};

} // namespace part_graph_snapshot
```

```cpp
// live_edit_prod.h — production seam implementations over the snapshot + ScriptHost.
// All methods run on the WORKER thread (the sole graph mutator).
class ProdGraphResolver : public live_edit::GraphResolver {
public:
    ProdGraphResolver(part_graph_snapshot::Snapshot& snap,
                      script_host::ScriptHost& host,
                      std::string schemas_dir, std::string shared_lib_dir);
    std::vector<PartId> parts_for_file(const std::string& path) override; // by_file, else by_import (shared-lib)
    std::vector<PartId> ancestors(const PartId& p) override;              // reverse-edge BFS
    std::vector<PartId> topo_order(const std::set<PartId>& subset) override; // children-first DFS over snapshot edges
    std::vector<PartId> roots_over(const std::set<PartId>& changed) override; // walk up to is_root nodes
    ResolvedHash reresolve(const PartId& p) override;  // reload source, resolve_hash with children's
                                                       // CURRENT snapshot hashes, update snapshot node,
                                                       // return as decimal string ("" on failure)
};
class ProdBaker : public live_edit::Baker { /* bake(p,h,budget) -> ScriptHost::bake_source with
    snapshot children hashes/modules/params; fail-closed (write only on success, HostBaker semantics);
    budget_ms <= 0 always (retired) */ };
class ProdFlattener : public live_edit::Flattener { /* reflatten(root) ->
    part_flatten::flatten_part(abs_cache_root, snapshot.nodes[root].resolved_hash, targets)
    with the provider's FlattenTargets/retopo settings for that hash */ };
```

- `PartGraph::install` gains an optional out-param `part_graph_snapshot::Snapshot* snap` — filled from the DFS (module, source path, children edges, params, final hashes, roots). `shared_imports` from a regex scan of each source for `from ['"]shared-lib/([^'"]+)['"]` and `import ['"]shared-lib/([^'"]+)['"]`. `LocalProvider` keeps the snapshot as a member, exposes `Snapshot& graph_snapshot()`, refreshed by every `install_graph()`.

**Steps:**

- [ ] **Step 1: Failing unit tests** — `live_edit_prod_tests.cpp` builds a real temp sandbox (three-part chain: `Root.js` requires `Mid.js` requires `Leaf.js`, plus `Shared.js` in shared-lib imported by `Mid.js` — reuse the schema-authoring pattern from `dev_live_edit_tests.cpp`/`meadow_bake_tests.cpp`), runs `install_graph()`, then asserts: snapshot has 3 nodes with correct edges/roots; `parts_for_file(<Mid.js abs path>) == {Mid}`; `parts_for_file(<shared Shared.js>) == {Mid}`; `ancestors(Leaf) == {Mid, Root}`; `topo_order({Root,Leaf,Mid})` is children-first; `roots_over({Leaf}) == {Root}`. Then behavior: edit `Leaf.js` on disk (append a param tweak that changes output), `reresolve(Leaf)` returns a NEW hash ≠ install hash; `ProdBaker.bake(Leaf, new_hash, 0)` succeeds and `parts/<new_hash>.part` exists; `reresolve(Mid)`/`reresolve(Root)` cascade to new hashes (children-first order); `ProdFlattener.reflatten(Root)` writes the new root's `.flat.part`. Run — FAIL.
- [ ] **Step 2:** Implement snapshot recording in `PartGraph::install` (the DFS already touches every node; record at resolve time, set hashes at bake/memo-hit time).
- [ ] **Step 3:** Implement the three seam classes in `live_edit_prod.cpp` per the interface block. `reresolve` string↔u64: decimal via `std::to_string`/`strtoull`.
- [ ] **Step 4:** `make -C MatterEngine3/tests run-liveprod` — green. Then the existing `run-live` (dev_live_edit) suite — untouched, green.
- [ ] **Step 5: Commit** — `git commit -m "feat(phase-b): production live-edit seams — graph snapshot + GraphResolver/Baker/Flattener (Task 9)"`

---

### Task 10: Watcher wiring + `RebakeCone` end-to-end

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp` (watcher + debounce in tick, RebakeCone command executor), `MatterEngine3/src/live_edit.h/.cpp` (expose `rebuild(paths)`), `MatterViewer/main.cpp` (env opt-in)
- Test: `MatterEngine3/tests/async_bake_tests.cpp` (inotify end-to-end case, Linux)

**Interfaces:**
- Consumes: Task 9 seams, Task 6 command loop, Task 2 `WorldDesc.enable_live_edit`.
- Produces:
  - `live_edit::LiveEditSession::rebuild(const std::set<std::string>& paths) -> RebuildReport` — `run_rebuild` made public under this name (`tick()` keeps working for the existing test suite by delegating to it).
  - App-thread side (in `WorldSession::tick()`, only when `enable_live_edit` and Linux): an `InotifyWatcher` watching `schemas_dir` + `shared_lib_dir`, plus a 150 ms debounce (copy the pending-paths/quiet-window logic from `LiveEditSession::tick`, ~15 lines). When the window closes, push `Command{RebakeCone, paths}`.
  - Worker `RebakeCone` executor: build the seams over the provider's snapshot + a fresh ScriptHost (`set_shared_lib_root`), `LiveEditSession sess(...) ` with `LiveEditConfig{debounce_ms: 0, bake_budget_ms: 0}` (**budget retired**), call `rebuild(paths)`; every `RebuildReport.errors` entry → `BakeError{code: ScriptError (or FlattenFailed→Internal), module: e.part, message, phase:"cone"}`; on `succeeded`: `compose_world(manifest, err)` then the same publish flow as BakeAll steps 4-8 from Task 6 (reset job → reconcile → per-part publish → finalize → `BakeFinished`). Fail-closed: on `!succeeded`, emit errors and DO NOTHING else — old world keeps rendering (last-good artifacts intact).
  - Cancel checkpoints between cone entries: pass the command token into the executor; check between `rebuild`'s per-part iterations is Phase-later (spec: between-jobs granularity) — the RebakeCone command checks before rebuild, before compose, and inherits per-part checks in the publish flow.
  - Viewer: `MATTER_LIVE_EDIT=1` env sets `wdesc.enable_live_edit = true` (in `MatterViewer/main.cpp` where `WorldDesc` is filled). Windows/non-Linux: field ignored with a one-line printf notice.

**Steps:**

- [ ] **Step 1: Failing end-to-end test** (in `async_bake_tests.cpp`, guarded `#ifdef __linux__`): sandbox world from Task 6 harness with `enable_live_edit=true`; full bake; record `instance_count` + a `raycast` hit's `part_hash`; then rewrite `Box.js` changing geometry size; pump `tick()+pump_gpu_jobs()` up to 30 s; assert: a `BakeStarted`(or first cone event)…`BakeFinished` pair arrives without calling `reload()`, and the raycast `part_hash` CHANGED (new resolved hash). Also a fail-closed case: write a syntax error into `Box.js` → `BakeError{code:ScriptError}` arrives, world still queryable with the OLD hash; then fix the file → recovers. Run — FAIL.
- [ ] **Step 2:** Implement per the Interfaces block. Note `LiveEditSession` construction needs a `FileWatcher&` — pass a tiny `NullWatcher : FileWatcher` (poll returns 0) since the worker path uses `rebuild(paths)` directly, not `tick()`.
- [ ] **Step 3:** `make -C MatterEngine3/tests run-asyncbake` — green (including Task 6/7 cases). `run-live` + `run-liveprod` — green.
- [ ] **Step 4:** Manual viewer check: `MATTER_LIVE_EDIT=1` + FIFO run, edit a Meadow schema (e.g. tweak a grass param), watch the world update without touching the FIFO; self-terminate the run. Report observations.
- [ ] **Step 5: Commit** — `git commit -m "feat(phase-b): inotify live-edit wired through RebakeCone worker command (Task 10)"`

---

### Task 11: Final verification — all exit gates

**Files:** none created (report only: `.superpowers/sdd/` per the executing skill's convention)

**Steps:**

- [ ] **Step 1: Full test sweep** — `./build-all.sh test` from repo root (add the three new run targets — `run-asyncq run-asyncbake run-liveprod` — to `build-all.sh`'s MatterEngine3 list first, in this task). Expected: baseline-only failures (TBB first-load flake reruns once; nothing new).
- [ ] **Step 2: Screenshot gate** — rerun Task 8 Step 6 verbatim (5/5 MATCH vs `/home/jkern/phase-a-refs/`, STATS counters exact).
- [ ] **Step 3: Determinism gate** — `make -C MatterEngine3/tests run-asyncbake` twice in a row; the determinism case must pass both.
- [ ] **Step 4: ThreadSanitizer one-off** — rebuild `run-asyncq` + `run-asyncbake` binaries with `-fsanitize=thread -O1 -g` (tests Makefile supports flag injection via `EXTRA_CFLAGS`/`CXXFLAGS` — check and use the existing hook; if none, a one-off manual compile line is fine). Run both; report any races found and fix them before proceeding. Not a permanent gate — do not commit sanitizer flags.
- [ ] **Step 5: grep-gate + dependency rule** — `bash MatterEngine3/tools/grep_gate.sh` clean.
- [ ] **Step 6: Windows clean rebuild** — `rm -rf` the windows obj dir (per `MatterViewer/Makefile` windows target layout), then `make -C MatterViewer windows`. Expected: `viewer.exe` links. If `std::thread` fails to link under MinGW, add `-static -lwinpthread` (or `-pthread`) to the windows LDFLAGS — note it in the report. `WinDirWatcher` is stubbed: confirm `enable_live_edit` is compile-time or runtime-guarded off on Windows (no throw path reachable).
- [ ] **Step 7: FIFO smoke** — scripted self-terminating run: bake cold, cam moves during bake, shot after finish, `reload` via FIFO, second shot, `quit`. Expected: no crash, shots non-empty.
- [ ] **Step 8: Commit** any build-all.sh / Makefile fixes — `git commit -m "test(phase-b): final gates — sweep, screenshots, TSan one-off, windows link (Task 11)"`

---

## Self-Review Notes (kept for the record)

- **Spec coverage:** async worker + queues (T1/T6), pump_gpu_jobs (T2/T8), tileset GL marshaling (T4), structured BakeError + OOM net (T2/T7), skip-and-continue (T7), partial renderability (T6 publish flow), cancellation/supersession/shutdown (T1/T6/T7), live-edit production wiring + budget retirement (T9/T10), determinism + screenshot + TSan + Windows gates (T6/T8/T11). Spec's `assert_gl_thread` guard: T4.
- **Known judgment calls encoded above:** per-part publish jobs run `get_or_load` on the GL thread inside the pump budget (LoadedParts are not portable across stores — BLAS handles are store-local); the heavy work (script eval/mesh/flatten) already happened on the worker during install, so publish jobs are disk-load + BLAS registration only. Composer cap grows incrementally with a final exact-cap rebuild in the finalize job.
- **Risk watch during execution:** any GL call the guards catch on the worker = stop and marshal it (that's what the guards are for); `tick()`/query thread-safety relies on ALL session-state mutation happening in GL-thread jobs — reviewers should reject any worker-side write to `store/state/composer/raster/stats/lods/sky_clear/connected`.
