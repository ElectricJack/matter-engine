# Phase B — Async Bake + Progress: Design

**Date:** 2026-07-08
**Status:** Approved design (brainstorm complete). Phase B of the engine+editor
roadmap (`2026-07-07-engine-editor-roadmap.md`), following Phase A kernel
extraction (merged 2026-07-08).

## Problem

Baking is fully synchronous. `WorldSession::request_bake()` runs the entire
pipeline — provider connect/reconcile, per-part sequential bake
(`local_provider.cpp` fetch loop), raster init, probe uploads — before
returning control. The viewer (`MatterViewer/main.cpp`, `open_and_bake()`)
blocks in a tight event-drain loop; the window is frozen for the whole bake.
Bake failures surface as string errors or, for uncaught `bad_alloc`, crashes.

Phase C's ExplorerDemo needs a loading screen over a live view of the world
assembling. That requires: a responsive main thread during bake, progress
events, partial results that render as they complete, and errors that don't
kill the session.

## Decisions (from brainstorm)

| Question | Decision |
|---|---|
| Parallelism | **One background worker thread now**; queue/job/cancel types designed so a worker pool can slot in later (after OOM hardening). No parallel part baking in Phase B. |
| Scope | **Everything routes through the job system** — full bake, `reload()`, and live-edit cone rebakes. One bake path. Includes wiring the previously test-only `LiveEditSession` into production (see Live-edit integration). |
| Cancellation | **Between-jobs (per-part) checkpoints now**; cancel flag plumbed through a job context so stages can add mid-stage checkpoints later. Triggers: session shutdown, superseding bake/reload. (Live-edit bursts are coalesced upstream by the existing debounce, not by cancellation.) |
| GPU jobs | **Coarse jobs, no mid-job slicing.** A long shader compile hitches the pump once; per-program job granularity is the knob. `GL_KHR_parallel_shader_compile` is a demo-polish follow-up, not Phase B. |
| Per-part failure | **Skip-and-continue** (behavior change): a failed part emits a structured error and the bake proceeds; `BakeFinished` reports an error count. Previously one bad part failed the whole bake. |

## Architecture

Three new pieces inside the kernel, owned by `WorldSession`'s impl:

### 1. Bake worker (command loop)

One `std::thread`, started lazily on first bake request, joined on session
destruction. Drains a mutex+condvar **command queue**:

- `BakeAll` — the existing `bake_once()` flow
- `Reload` — same flow, triggered by `reload()`
- `RebakeCone(changed_files)` — live-edit incremental rebake
- `Shutdown`

Commands execute the **existing sequential code paths largely unmodified** —
this is deliberately not a job-graph decomposition. The worker is the only
thread that mutates the part graph and provider state.

### 2. GPU job queue

Thread-safe queue of `GpuJob { fn, name, cost_hint }`. The worker posts all
GL work here: world reset, raster init, GPU-driven shader compile, probe
texture uploads, GpuCuller init, per-part mesh upload-and-publish.

The app drains it on the GL thread via a new public method:

```cpp
void WorldSession::pump_gpu_jobs(float ms_budget);  // e.g. 4.0f per frame
```

Whole jobs run until the budget is spent; no mid-job slicing. When the worker
needs a job's result to continue (e.g. raster init), it blocks on a future;
fire-and-forget jobs (part uploads) don't block the worker.

### 3. Cancel token / job context

`BakeJobContext { std::atomic<bool> cancel; ... }` travels down the bake path.
Checkpoints: between parts in the fetch loop, between cone entries in
live-edit rebake, before every GPU handoff. Stage internals untouched in
Phase B.

### Public API changes

- `request_bake()` / `reload()` — unchanged signatures; now enqueue a command
  and return immediately.
- `pump_gpu_jobs(float ms_budget)` — new; app calls once per frame.
- `poll_event()` — unchanged shape; the event queue becomes the thread-safe
  progress channel. Events gain `phase` and structured error fields (below).
- Threading contract: **all public methods are app/GL-thread-only.** The
  worker is invisible to the app.

## Data flow & partial renderability

Per-part flow during any bake command:

1. Worker runs CPU stages (script eval → voxelize → mesh → flatten) — today's
   `get_or_load` path — producing a CPU-side artifact.
2. Worker posts an **upload-and-publish** GPU job and moves to the next part
   without waiting.
3. `pump_gpu_jobs()` runs the job on the GL thread: uploads buffers, then
   inserts the part into the part_store. Publication and rendering both
   happen on the app thread → the renderer never sees a half-published part;
   **no locking in store or renderer**.
4. WorldComposer already recomposes instances per frame from the store, so
   the part appears next frame — the world visibly assembles.

World reset (`state.reset(manifest)`) runs as the first GPU job of a
`BakeAll`/`Reload`, so clearing also happens on the app thread. The lazy
raycast BVH rebuilds on demand and sees the currently-published subset.

Note: bake wall-clock now depends on the app pumping (4ms @ 60fps ≈ 240ms of
GL time per second — ample; uploads are cheap, big compiles are one-time).

## Cancellation & supersession

- New `BakeAll`/`Reload` **cancels the in-flight command and clears all
  pending commands** (a full bake moots queued cone rebakes).
- `RebakeCone` queues FIFO; live-edit's existing debounce coalesces bursts
  upstream.
- Cancelled work emits `BakeError{code: Cancelled}`; the worker moves on.
  Published parts stay published — publishes are atomic, the world is always
  renderable, just possibly incomplete.

**Shutdown without deadlock:** the worker may be blocked on a GPU future while
the app tears down and stops pumping. Session destructor: (1) set cancel +
enqueue `Shutdown`, (2) fail all pending GPU futures with cancelled status so
the worker unblocks, (3) join, (4) drain worker-posted GPU jobs on the GL
thread (destructor runs on it). No app-side ceremony for quit-mid-bake.

## Live-edit integration

**Reality check (found during planning):** `LiveEditSession` (watcher →
debounce → cone rebake, the SP-5 work) exists in the kernel with full test
coverage but was never wired into production — its seam interfaces
(`live_edit::GraphResolver` / `Baker` / `Flattener`, `live_edit_interfaces.h`)
have no production implementations. Production "live edit" today is FIFO
`reload` → full `bake_once`, where content-addressed caching provides
cone-like behavior implicitly. **Phase B wires `LiveEditSession` for real**
(decided 2026-07-08):

- New production seam implementations bridging `LiveEditSession` to the
  provider/script-host machinery: `GraphResolver` over the part graph
  (`part_graph.h` resolve/reverse-map/topo), `Baker` over
  `script_host`/`HostBaker`, `Flattener` over the root re-flatten path.
- File watcher + debounce run in app-thread `tick()` (watcher polling is
  cheap); when the debounce fires, `tick()` enqueues
  `RebakeCone(changed_files)`.
- **Cone computation and rebake run in the worker** (the worker is the sole
  graph mutator → no shared-graph locking).
- **The 2000ms dev budget retires** — it bounded main-thread stalls, which no
  longer exist. A cone rebake runs to completion as one cancellable command
  (cancel checkpoints between cone entries).
- Fail-closed unchanged: script error → last-good artifact kept. The internal
  `LiveEditError` folds into the public event stream as
  `BakeError{code: ScriptError, part, message}`.
- Rebaked parts publish via the same upload-and-publish job; the world state
  refresh after a cone rebake re-resolves affected manifest entries to the
  new part hashes.

## Error handling

New public structured error, carried in the existing event struct:

```cpp
struct BakeError {
  enum Code { Cancelled, OutOfMemory, ScriptError, GpuError, IoError, Internal };
  Code code;
  std::string part;     // offending part, if known
  std::string phase;    // bake phase, if known
  std::string message;
};
```

- **bad_alloc safety net** (the ROADMAP item that lands here): worker command
  loop gets a top-level catch — `bad_alloc` → `{OutOfMemory}`, anything else
  → `{Internal}`. Nothing escapes the worker thread. The session survives;
  the world keeps whatever was published. Existing inner catches
  (part_flatten, script_host, tileset_bake_gpu) remain as finer-grained
  reporters.
- **Per-part failures: skip-and-continue.** Emit `BakeError{part}`, keep
  baking, end with `BakeFinished` carrying the failed-part count in a new
  `errors` field on the event struct. A demo world with one broken schema
  still loads everything else.
- **GPU init failures stay bake-fatal** (`{GpuError}`, command aborts —
  nothing renders without shaders). Per-part upload failures follow
  skip-and-continue.

## Exit criteria

1. Viewer stays interactive during a full cold bake — camera flies, window
   responds, parts visibly pop in (loading-screen UI polish is Phase C).
2. Post-bake output identical to sync baseline — Phase A screenshot gate
   (5-shot pixel match) passes against the same refs.
3. Determinism — same seed produces the same progress-event sequence,
   verified headless.
4. Live-edit regression — file-watcher cone rebake works through the job
   system; fail-closed behavior intact.
5. No crash on induced OOM — structured `BakeError`, session survives, world
   renders whatever completed.
6. Existing gates green — full test sweep, grep-gate, Windows viewer.exe
   link, FIFO smoke.

## Testing

1. **Determinism test** — headless harness (pump loop + event drain) bakes
   Meadow twice with the same seed; assert identical `{type, part, done,
   total}` sequences. Order is naturally deterministic: one worker,
   sequential parts, FIFO queues.
2. **Async behavior unit tests** — new test binary in `MatterEngine3/tests`:
   `request_bake()` returns immediately; events arrive while pumping;
   superseding bake cancels in-flight (`Cancelled` then fresh `BakeStarted`);
   destructor mid-bake joins cleanly (shutdown-deadlock regression test).
3. **OOM injection** — test hook throws `bad_alloc` at part N; assert
   `BakeError{OutOfMemory, part}`, bake continues, session renders survivors.
4. **Screenshot gate** — existing 5-shot pixel gate against Phase A refs
   after a fully-pumped async bake (async output ≡ sync baseline).
5. **Live-edit regression** — existing live-edit tests through the
   `RebakeCone` path.
6. **Existing gates** — full sweep, grep-gate, Windows link, FIFO smoke.

Guards added during implementation:

- Debug-build `assert_gl_thread()` at raster/upload entry points — catches
  any GL call sneaking onto the worker, permanently. (Also serves as the
  audit for anything inside per-part baking secretly touching GL, e.g.
  imposter bakes.)
- One-off ThreadSanitizer run of the new unit tests during implementation
  (not a permanent gate).

## Risks

| Risk | Mitigation |
|---|---|
| Hidden GL call inside per-part CPU bake | `assert_gl_thread()` guard; audit during planning |
| Worker/GL-thread deadlock on shutdown | Explicit destructor protocol + dedicated regression test |
| Long shader compile hitches the pump | Accepted for Phase B; per-program job granularity is the knob; `KHR_parallel_shader_compile` deferred to demo polish |
| Skip-and-continue masks systemic failures | `BakeFinished` carries error count; every skip emits a structured event |
| OOM still possible (deferred deep fixes) | Safety net converts crash → `BakeError`; per-part flatten budget / streaming flatten remain backlog |

## Non-goals (Phase B)

- Parallel part baking (worker pool) — deferred until OOM hardening.
- Mid-stage cancellation checkpoints / QuickJS interrupt handler.
- Loading-screen UI (Phase C owns it).
- Deep OOM fixes: per-part flatten budget, streaming flatten (backlog).
- `GL_KHR_parallel_shader_compile` (demo-polish follow-up).
