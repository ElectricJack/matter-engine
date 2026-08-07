# Getting streaming-sector publish off the render thread — design

**Date:** 2026-08-07
**Status:** spec, step 0 (measurement) in progress
**Goal:** the streaming bake must stop hitching the main render loop. Baking may
be as SLOW as it needs to be on background threads — the only defect is that it
touches the render thread at all. This is a **decoupling** problem, not a
speed-optimization one. Do not make the bake faster; make it not run on the
render thread.

Derived from a 2026-08-07 Fable analysis, grounded in the new ProfileLib
findings (`docs/superpowers/specs/2026-08-07-engine-profiler-design.md`): the
render-thread cost is `loop_pump`, and a `[gpu-job] stream.publish took 98.2 ms`
outlier was observed.

## A. Diagnosis — what runs on the app thread today, and why it's pinned

Pipeline: a worker bakes a sector — `bake.sector` → `bake.stagemem`/`bake.stageload`
(`matter_engine.cpp:4360-4402`) and `bake.prebuild` = `build_vulkan_part`
(`matter_engine.cpp:4501-4530`, fn at `6382`) — then posts ONE indivisible
`GpuJob` named `stream.publish` (`matter_engine.cpp:4537-4852`). `main.cpp:2775`
drains it via `pump_gpu_jobs`. `GpuJobQueue::pump`'s budget bounds job *starts*,
never durations (`async_bake.cpp:85-105`), so one oversized publish blows the
frame — the `loop_pump` spike.

**What the publish job does on the app thread** (guarded by `assert_gl_thread`,
`matter_engine.cpp:4543`):
1. Coordinator/ledger bookkeeping (`4580-4627`) — cheap.
2. `store->commit_staged(...)` (`4662`) — adopts worker-staged BLAS entries,
   O(entries). **But** the fallback `store->get_or_load(sector_hash)` (`4663`)
   runs a **full decode + LOD-ladder bake on the render thread** when staging
   failed (animated part, unreadable generation).
3. `state.apply(delta)` + `tracer.reset()` (`4688-4691`) — cheap.
4. `register_vulkan_part` → `VkSceneRenderer::ensure_part`
   (`vk_scene_renderer.cpp:6044-6318`) — the real payload:
   - index-range validation, O(indices) (`6118-6125`);
   - **RT buffer creation**: `create_buffer`+`map`+`memcpy`+`flush` for
     `rt_geometry` and `rt_index` (`6126-6164`) — two `vkAllocateMemory` + O(v+i)
     memcpys per part;
   - vertex/index copies into the CPU mirrors `vertex_staging_`/`index_staging_`
     (`6169-6181`);
   - `adopt_part_impostors` (`6174-6176`) — for impostor parts a **synchronous
     `submit_immediate`**: `vkQueueSubmit2` + `vkWaitForFences`
     (`vk_scene_renderer.cpp:5150` → `vk_resources.cpp:788` → `vk_context.cpp:2509`),
     a full CPU↔GPU round trip inside the pump;
   - **`rt_lods` material-id extraction** (`6221-6236`): for every cluster×LOD a
     loop over *every index* reading `vertices[indices[..]].material_index`, then
     sort+unique — O(Σ index_count), cache-hostile; plus `record.material_ids`
     over all vertices (`6239-6247`);
   - GpuCluster packing, `register_vt_part`, dirty-range recording,
     `command_template_dirty_` (`6248-6316`).
5. **Fallback build path**: when `prebuilt_part` is null (prebuild off, staging
   failed, or the diagnostic material override — `4486-4501`), the whole
   `build_vulkan_part` (~11.4 ms of a 13.9 ms publish) runs in-job via
   `ensure_vulkan_part` (`4795`, `6358`).
6. `vk_instance_cache.invalidate_expansion()` (`4819`) — cheap.

The static geometry **transfer** is NOT in the pump: cluster/vertex/index buffers
are HOST_VISIBLE|HOST_CACHED, and `upload_scene_buffers`' `kAppend` path
(`vk_scene_renderer.cpp:8939-9001`) memcpys only dirty ranges into persistently
mapped memory during render. BLAS/TLAS builds are lazy inside render recording
(`9708+`), not at publish.

**Why it's pinned (the real constraints):**
1. **`VkSceneRenderer` is single-threaded by construction** — essentially no
   synchronization (one atomic, zero mutexes). `ensure_part` mutates
   `vertex_staging_`/`index_staging_`/`cluster_staging_`/`parts_`/`slot_of_`/the
   free-range lists — the exact structures `update_instances`,
   `upload_scene_buffers`, and RT selection read every frame. This, not Vulkan,
   is the primary pin.
2. **One externally-synchronized `VkQueue`.** Only graphics-family queues are
   created, extras reserved for Streamline (`vk_context.cpp:1094-1099,1320-1334`);
   **no transfer queue**. `vkQueueSubmit2` is called from the frame loop (`2114`)
   and `submit_immediate` (`2510`) with **no lock** — same-thread-by-convention
   is the only thing making it legal. Any off-thread submit is a spec violation.
3. **The free-range reuse proof is in render-thread frame serials.** In-place
   writes into live host-visible buffers are safe only because a freed range "sat
   unreferenced for a full in-flight window" measured in `static_frame_serial_`
   (`8002-8010`, comment `6312-6316`). Allocation must be ordered against frame
   retirement — on the thread that advances the serial.
4. **NOT a constraint:** per-thread command pools. `submit_immediate` makes its
   own transient pool per call (`vk_resources.cpp:731-749`); `vkCreateBuffer`/
   `vkAllocateMemory`/`vkMapMemory` are thread-safe at device level. A worker may
   legally create, map, fill, and flush its own buffers — it just may not touch
   the queue or the renderer.

**Quantification.** The `matter_engine.cpp:206` "~96%" comment predates the
worker prebuild; with `MATTER_STREAM_PREBUILD` on (default), the
`build_vulkan_part` half (~82%) is already off-thread. What remains per sector is
`ensure_part` (`g_pub_gpu_ms`) + `commit_staged` — a few ms typically but
**unbounded**: scales with Σ(vertices+indices), two `vkAllocateMemory` calls.
The 98 ms outlier is consistent with any of (a) the fallback build firing,
(b) `get_or_load` full decode in-job, (c) an impostor fence-wait, (d) driver
allocation hiccups. `MATTER_STREAM_PUBLISH_PROFILE` (`4643-4654`) splits this —
step 0.

## B. Design options (evaluated)

1. **Split the publish job at the register/activate seam.** A registered part
   with no manifest instance is invisible until `state.apply` adds the instance.
   So `stream.publish` splits into job A (`commit_staged` + `register_vulkan_part`)
   and job B (ledger + `state.apply` + tracer + `invalidate_expansion`), pumped in
   order. The pump's ≥1-job progress guarantee now overruns by the *largest half*,
   not the sum. Delicate part: `PublicationTransaction`/rollback (`4550-4578`) must
   span two jobs. No new Vulkan risk. Halves the worst hitch; removes no cost.
2. **PreparedPart handoff (the big win).** Move everything in `ensure_part` that
   is a pure function of `VkScenePart` to `bake.prebuild` on the worker: index
   validation, `rt_lods` material-id extraction + sorts, `record.material_ids`,
   GpuCluster packing, `dense_rt_lod_offsets`, mesh-lod counts — **and** the RT
   buffer pair (worker calls `create_buffer`/`map`/`memcpy`/`flush` itself; legal
   off-thread, no queue). `ensure_part` gains an **adopt** path that only:
   allocates ranges, memcpys vertices/indices into the mirrors, pushes
   `parts_`/`slot_of_`, records dirty ranges, calls `register_vt_part`. Same
   single-owner handoff pattern as `staged_load`/`prebuilt_part` today. Low risk —
   proven twice already; gate with a byte-compare verify env. Residual app-thread
   cost: two memcpys (O(bytes)) + O(clusters) bookkeeping.
3. **Worker writes directly into reserved mapped ranges (removes the memcpys).** A
   cheap app-thread "reserve" job allocates ranges (must stay render-thread — the
   frame-serial proof) and returns stable destination pointers; the worker fills
   GPU range + CPU mirror. Hazards: mirror `std::vector` resize invalidates
   pointers, and the `kFull` recreate path (`9003+`) swaps buffers under the
   worker — needs a growth epoch/pin. Real blast radius in the allocator. Only if
   step 2 leaves memcpy visible.
4. **Dedicated upload thread + second same-family queue, timeline-gated.** One
   extra graphics-family queue (Streamline plumbing at `1190-1334` shows how)
   owned by an upload thread with its own pools; `submit_immediate`-style work
   (impostor atlases `5150`, VT/tileset page uploads `5596`, device-local material
   staging) submits there and signals a timeline semaphore the render submit
   waits on. Same-family ⇒ **no queue-family ownership transfer**, only a barrier.
   Moderate risk (new sync in a device-lost-scarred codebase). For *terrain sector
   streaming* it buys little — the static path never submits — so gate on
   `MATTER_STREAM_PUBLISH_PROFILE` showing the immediate-submit waits inside
   `loop_pump`.
5. **Time-slice one large job.** Coroutine-izing `ensure_part` — worst
   effort/benefit; option 1 gets the same effect at the natural seam. Rejected.

**What genuinely must stay on the render thread:** range allocation (frame-serial
proof), `parts_`/`slot_of_`/mirror mutation, `state.apply` + expansion
invalidate, `register_vt_part` (VT runtime is render-read), and the deferred
`command_template` rebuild (already amortized, `6305-6310`). After option 2 these
are O(clusters)+O(bytes-memcpy) — sub-millisecond for a 64 m sector. So of the
publish work, everything O(vertices+indices) (≈ all of the unbounded cost) is
movable; the unavoidable residue is the pointer/generation swap tier, and it's
small.

## C. Recommended incremental path

- **Step 0 (measurement) — DONE 2026-08-07. Result below reorders the plan.**
  Cold StreamMountain fill, `MATTER_STREAM_PUBLISH_PROFILE=1`,
  `MATTER_GPU_JOB_SLOW_MS=5`, perf harness. `loop_pump` avg **24.3 ms**, peak
  **62 ms** — severe render-thread hitch confirmed. Two distinct regimes:
  - **Cold-fill mega-spikes (94–1437 ms): dominated by `load=`.** e.g.
    `sector(1,0) load=1419.3 vulkan=0.6 → job 1436.8 ms`; second sector
    `load=232.8`. This is the render-thread `get_or_load`/`commit_staged` decode
    fallback — exactly Fable's step-1 target. CONFIRMED as the worst hitches.
  - **Steady-state jobs (14–21 ms): NOT explained by the sub-timers.** A typical
    line reads `load=0.1 state=0 tracer=0 culler=0 vulkan=0.4 [cpu=0 vloop=0.5
    classify=0 gpu=0.4] cache=0`, summing to ~0.5 ms — yet the *same job* takes
    14–18 ms (`took 14.3 ms`). So **~13–17 ms per steady-state publish is
    UNACCOUNTED by the existing `[stream.publish]` split.** The vertex/material
    work that step 2 (PreparedPart) moves off-thread is only ~0.5 ms here — so
    step 2, as originally scoped, would NOT fix the steady-state hitch. The real
    cost is elsewhere in the publish lambda (coordinator bookkeeping,
    `register_vt_part`, an impostor `submit_immediate` fence-wait, event-hub
    publish, or graph-snapshot publish) — none of which the current timers cover.

  **Consequence — insert Step 0.5.** Before step 1/2, instrument the publish
  lambda with **nested `PROFILE_SCOPE`s** (using ProfileLib's new parent/child
  nesting) under a `publish` parent to attribute the missing ~13 ms.

- **Step 0.5 (done 2026-08-07) — instrumented all 8 publish regions, measured,
  and Fable-validated. This REORDERS the fix.** Per-frame max from the trace:
  `publish.load` **173 ms max / 613 ms total** (steady jobs 77-106 ms);
  `publish.vulkan` 18 ms / 129 ms; `ledger` 0.06 ms; `begin/reserve/apply/cache/
  commit` ≈ 0. Worst frame 220 ms.

  **Root cause (Fable, code-cited).** Not the `get_or_load` fallback — on the hot
  path `staged_load->ok` is true, so `commit_staged` (part_store.cpp:1498) runs.
  Its comment "O(entries), no BVH rebuilt" is right about no BVH and wrong about
  the constant: `commit_staged` → `BLASManager::adopt_from` → `register_prebuilt`
  (blas_manager.cpp:232-283) **deep-copies per newcomer** — two full `Tri`
  copies, two full `TriEx` copies, a BVH node array = **~380 B alloc+copy per
  triangle, per rung, on the render thread**. A terrain ladder is hundreds of
  thousands of triangles → hundreds of MB/publish. The copy discipline exists for
  UNALIGNED FILE buffers; the staged entries are aligned engine-owned vectors the
  caller then discards (blas_manager.cpp:326-328). The 173 ms spikes are
  `build_expansion` calling `get_or_load` for shared CHILD variants (first
  reference decodes on the render thread; memoized after) — this is the one place
  step 1's fallback logic genuinely applies.

  **Instrumentation caveats Fable caught (recorded so we don't misread):**
  (a) `publish.cache` = `invalidate_expansion` only DROPS the flat set; the
  O(world) expansion rebuild lands NEXT FRAME in the render path, uninstrumented
  — the issues/bfb5f13e cascade, and the ~30 ms gap between the 220 ms worst
  frame and load+vulkan. (b) `bake.*` scopes span frames, so their per-frame
  maxima are artifacts — quote totals, not spikes. (c) `zone_ns` is inclusive of
  children — a per-frame sum double-counts the `publish` parent.

  **The fix, reordered (measure-first):**
  1. Confirm — split `publish.load` into `.commit`/`.fallback`; add
     `commit.adopt` vs `commit.expansion` scopes + a triangle `PROFILE_COUNT`
     inside `commit_staged`. One cold fill proves adopt dominance before touching
     shared `blas_manager`.
  2. **Move, don't copy.** A consuming `BLASManager::adopt_from(BLASManager&&,
     remap)` that STEALS the staged `unique_ptr<BLASEntry>` (pointer move + new
     handle + two map inserts). `commit_staged` becomes O(entries). Expected
     `publish.load` steady 100 ms → low single digits; fixes hitch AND fill
     throughput (the pump runs one whole publish/frame regardless of budget,
     main.cpp:2775). Keep `register_prebuilt`'s copies for the real file-decode
     path only.
  3. Pre-warm the sector's child hashes on the worker (the staged part carries
     `children`) so `build_expansion` never cold-`get_or_load`s on the render
     thread. This is where the original step 1/2 machinery applies — to children.
  4. Measure worst-case `frame_ms` (not just `publish.*`): once load is fixed,
     the deferred expansion cascade (caveat a) is the next ceiling.

  Original steps 1-3 below stand but are RE-AIMED: the render-thread cost was
  inside the "already solved" staged-commit path (a redundant copy at the seam),
  not the coordinator or the sector-level fallback.

- **RESOLVED 2026-08-07.** Four hypotheses died to measurement before the real
  cause held (my coordinator guess, Fable's adopt-copy, Fable's deep-walk, my
  "walk is the cost" — the last one because my first pre-warm attempt was placed
  in `install_world`, which runs BEFORE the reset job creates `store`, so it
  was a silent no-op; the `prewarm=0` anomaly exposed it). The true cause: a
  sector has ~1640 direct placements of ~92 distinct child variants, and the
  FIRST streamed sector cold-decodes those ~92 on the render thread inside
  `commit_staged` → `build_expansion` → `get_or_load` (109 us/node = decode, not
  walk). The variants load as flats (walk leaves), so it is neither a deep walk
  nor a flat-admission failure.

  **The fix (shipped):** pre-warm the child-variant catalog into the store inside
  the GL-thread reset job, right after `store.swap` (matter_engine.cpp), over the
  `sector_child_hashes` that `install_world` populated. Gated to the world-kind
  streaming publish via `PublishPipelineParams::prewarm_child_catalog` (a
  Fable-review finding: the shared `publish_pipeline` would otherwise warm a
  stale catalog into closed-world/cone stores and pin it resident); plus a
  cancel-token check in the loop.

  **Measured:** `commit.expansion` 469 ms → 2.9 ms total (max 152 ms → 0.07 ms);
  `expansion.coldload` during fill → 0; worst `frame_ms` 220 ms → 48 ms; no
  validation errors / device-lost. Fable code-review: thread-safety, ordering,
  fallback, render-equivalence all PASS.

  **Next ceiling:** `publish.vulkan` (`ensure_part`/`register_vulkan_part`) at
  ~21 ms max / 120 ms total — the ORIGINAL PreparedPart target (original step 2
  below). Deferred follow-up (Fable): the reset pre-warm serializes ~92 disk
  decodes on the GL thread; staging them on a worker (`stage_load` is
  worker-safe) and committing O(entries) on the GL thread would shrink the
  reset cost — worth it only if connect latency becomes a concern (it is a
  loading screen today). The deferred expansion-rebuild cascade (issues/bfb5f13e)
  remains uninstrumented and is the next thing to look at after `publish.vulkan`.
- **Step 1 — kill the fallback-on-render-thread paths + split the job.**
  (a) When `prebuilt_part` is null for a *streamed* sector, don't build in-job —
  fail the publication back to the streamer for re-request (or route the build to
  the worker); the render thread must never run `build_vulkan_part`/`get_or_load`
  for streaming. (b) Split `stream.publish` at the register/activate seam
  (option 1). Measure: `loop_peak_pump_ms` (`main.cpp:721-733`) drops to ~max
  single-half; `frame_ms` jitter during fill shrinks; `bake.sector` unchanged
  (throughput must not regress — fill-time-to-quiet is the guard).
- **Step 2 — PreparedPart (option 2).** Move validation, rt_lods/material-id
  extraction, GpuCluster packing, and RT buffer create+fill into `bake.prebuild`;
  add the adopt path in `ensure_part`. Gate with `MATTER_STREAM_PREPARED_VERIFY`
  (clone of `MATTER_STREAM_PREBUILD_VERIFY`, `4708-4759`) that rebuilds the old
  way in-job and byte-compares. Measure: `g_pub_gpu_ms` per sector (target < 2 ms),
  `loop_pump_ms` flat during fill. Guards: the Vulkan smoke suite's ~16 modes,
  `invalid_part_records` (the device-lost confirmation signal), and a shot-replay
  before/after pixel diff.
- **Step 3 (only if step 0/2 demand).** Direct-into-mapped-range worker writes
  (option 3) and/or the second-queue upload thread (option 4) for impostor/VT
  immediate submits. Each contingent on the profiler showing the specific
  residue; neither started on spec.

**Endpoint after step 2:** worker does bake + stage + full GPU-resource
preparation; the render thread's per-sector work is a range allocation, two
bounded memcpys, and a generation bump — with no new queues, no QFOT, and no
change to the frame-serial safety proof that keeps this engine's streaming
allocator device-lost-free.
