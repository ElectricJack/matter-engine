# Sector Streaming Fill Throughput: Where The Time Actually Goes (2026-07-30)

Status: the **worker-side part prebuild landed** on this branch (publish
13.9 ms → 1.46 ms, StreamMountain fill 18 → 23 sectors/s at stock settings).
This document records the whole measurement chain so the next pass — in
particular the **GPU tape-classification work on the sibling branch** — does
not rediscover any of it.

Everything below was measured on StreamMountain, RTX 4090 / 24 cores, editor
built `-O2 -DNDEBUG`, artifacts to the per-pid transient dir.

## TL;DR for the GPU-classification agent

The thing you are moving to the GPU was **97% of the sector publish cost**, and
publish was **the limiter on whole-world fill** — not the bake, not dispatch.

- `vt_classify_chart_vertices` cost **8.77 ms per sector** on the render thread.
- The vertex/index repacking it sits next to costs **0.21 ms**. It is noise.
- That cost is now **on the bake worker**, not the render thread
  (`build_vulkan_part`, see "What landed"). If your GPU path removes the
  classification entirely, the remaining prebuild is ~1 ms of CPU and the split
  still pays for itself — but **the urgency is gone**, so do not contort the
  GPU design to preserve the worker prebuild.
- **Why it was so expensive** (this is the part worth keeping): in the DEFAULT
  demand-driven VT path, `part.lod_chart_meshes` is left **empty**, so the
  legacy-parity block in `build_vulkan_part` (`matter_engine.cpp`, search
  `Fail-closed parity for the LEGACY path`) takes its `else` branch and
  **re-classifies every LOD rung from scratch** purely to derive the per-vertex
  material argmax. Only `MATTER_VT_EAGER=1` populates the weights that branch
  would otherwise reuse. If the GPU version keeps that parity block, feed it the
  weights rather than recomputing them.
- There are **four** `vt_classify_chart_vertices` call sites in
  `matter_engine.cpp`. Two are in the part build (eager-VT copy path, and the
  legacy-parity block); two are in the on-demand VT request servicing. The same
  vertices can be classified more than once per sector across those paths —
  worth auditing while you are in there.

## The measurement chain (each step relocated the bottleneck)

Stock StreamMountain filled at **18 sectors/s**. The bake pool was at **~21%
utilisation**, so the bake was never the constraint.

| what was tested | result | conclusion |
|---|---|---|
| raise `max_inflight` 16 → 32 | 18/s → 18/s | not latency-bound; the publish stage is throughput-limited |
| widen GL pump 14 → 120 ms | 18/s → 27/s | publish per-frame count was the cap |
| + 12 executors | → 31/s | inflight/completion pool (32) becomes the wall |
| **move part build to worker** | **18/s → 23/s at stock knobs** | publish stops being the limiter |
| + 12 executors | → 46/s | bake pool becomes the limiter, correctly |

### Why the pump budget was the cap

`GpuJobQueue::pump(ms_budget)` bounds how many jobs **start**, never how long
one **takes** (the progress guarantee runs a started job to completion). A
`stream.publish` averaged **14.1 ms** against a **14 ms** busy-frame budget, so
the pump started **exactly one publish per frame** and fill rate collapsed to
the frame rate: ~18 fps → 18 sectors/s. At 1.46 ms it now starts ~8 per frame,
which is why the pump budget was left at its original 4/14 ms.

### Publish cost breakdown (n=1302, before the change)

```
load   (commit_staged)          0.58 ms   4.1%
state / tracer / culler / cache ~0.00 ms  ~0%
vulkan (ensure_vulkan_part)    13.32 ms  95.9%
  ├─ CPU conversion            11.43 ms  89.5% of vulkan
  │    ├─ surfaces() classify   8.77 ms   <-- the real cost
  │    └─ vertex/index repack   0.21 ms
  └─ GPU renderer.ensure_part   1.33 ms  10.4% of vulkan
```

After: `load 0.50 + vulkan 0.96 (cpu 0.00, classify 0.00, gpu 0.96) = 1.46 ms`.

## What landed

`ensure_vulkan_part` was split into three pieces in `matter_engine.cpp`:

- **`build_vulkan_part(part_hash, loaded, force_lod, surface, out_part)`** —
  everything except the two renderer calls. Touches no renderer and no GPU
  state; reads only the global material registry, which is written at world
  load and read-only while sectors stream.
- **`register_vulkan_part(renderer, part_hash, part, drawable, error)`** — the
  two renderer calls, nothing else.
- **`ensure_vulkan_part`** — unchanged signature, now just `build` + `register`.
  Every non-streaming caller keeps the old behaviour.

`bake_and_stage_sector` calls `build_vulkan_part` on the bake executor, right
after `stage_load`, and hands the finished `VkScenePart` to the publish job.

The classification needs a `VtSurfaceClassifier`, which the publish job derives
from app-thread state. The worker reproduces it:

| input | how the worker gets it |
|---|---|
| `tape`, `field`, `tape_hash` | `world_surface` / `world_field`, install_world-owned and fenced by `quiesce_bake_pool()` exactly like the bake's other inputs |
| `local_to_world` | the sector's own `tx/tz * sector_size` translation — the same matrix the publish job builds |
| `world_anchored` | from the count of manifest entries sharing this part hash. The job counts it *after* applying this sector's entry, and sector variants are unique by construction, so the count is 1 |

Falls back to building inside the job when `stage_load` returned `!ok`, or when
`MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL` is set (its override mutates the
global material registry; concurrent executors must not race on it).

### The correctness gate

`MATTER_STREAM_PREBUILD_VERIFY=1` rebuilds each part the old way on the render
thread — classifier derived from live manifest state — and byte-compares
vertices, indices, `surface_materials`, `surface_tape_hash`,
`vt_deferred_rung_mask` and cluster count against what the worker produced.

**Verified: 1,600 sectors, 0 mismatches.** Re-run this after any change to
`build_vulkan_part` or to how the classifier is derived. It roughly halves fill
throughput while enabled (it does the work twice), which is expected.

`MATTER_STREAM_PREBUILD=0` forces the old path from the same binary.

## Instrumentation added (all env-gated, all off by default)

| env var | prints |
|---|---|
| `MATTER_STREAM_FILL_PROFILE=1` | `[stream.rate]` every 200 steps + one `[stream.fill]` summary at first all-holes-filled |
| `MATTER_STREAM_PUBLISH_PROFILE=1` | per-sector `[stream.publish]` with `load/state/tracer/culler/vulkan[cpu,vloop,classify,gpu]/cache` |
| `MATTER_STREAM_PREBUILD_VERIFY=1` | `[stream.prebuild.verify] match=N MISMATCH=N` |
| `MATTER_STREAM_PREBUILD=0` | disable the worker prebuild |
| `MATTER_STREAM_INFLIGHT=N` | override `max_inflight` (default 16) |

Reading `[stream.rate]`: **`pool` at its cap = bake-bound. `inflight` pinned at
`max_inflight` with an idle `pool` = the publish/acknowledge stage is the
limiter.** That one line is what turned three wrong guesses into the right answer.

## Dead ends — do not repeat these

- **"The 50 ms worker tick + batch-of-`stream_worker_count` dispatch cap is the
  bottleneck."** It is not, for any world where publishes are expensive. That
  path's ceiling is `stream_worker_count / 50 ms` = **80 sectors/s** at the
  default 4, and StreamMountain never got near it. An adaptive-wait +
  deeper-batch change was written, measured, and **reverted** — no effect.
  It would only matter for a world whose publishes are already cheap.
- **Raising `max_inflight` alone.** 16 → 32 changed nothing (18/s → 18/s). It
  pins at whatever you set because the drain rate downstream is fixed.
- **Widening the GL pump budget.** Works (18 → 27/s) but buys throughput with
  frame hitching; it treats the symptom. Reverted once the publish got cheap.

## Known-adjacent issues found but NOT fixed

1. **`world_tracer: load_v2 failed` for essentially every streamed sector** —
   2,261 in one ~4 min run, one per distinct sector hash. Sector artifacts live
   in the per-pid transient dir; `world_tracer`'s `scratch_dir_`
   (`world_tracer.cpp`, `if (!scratch_dir_.empty())`) does not appear to be
   finding them. If so, **RT tracing is silently skipping every streamed
   sector** — a correctness issue, not just the wasted `TLASManager(65536)`
   allocation per miss. Worth its own investigation.
2. **The terrain LOD ladder is OFF by default** (`matter_engine.cpp`, disabled
   2026-07-29). Every `[stream.bake]` line reads `lod=5`, i.e. a sector 2 km out
   bakes the **full voxel mesh** exactly like one underfoot. Real bake cost is
   **47.7 ms mean** (median 45.5, p90 66.1, n=2334). `MATTER_TERRAIN_LOD=1`
   should cut both bake and publish cost for distant sectors; untested here.
3. **`std::getenv` inside the per-mesh loop** in `build_vulkan_part`
   (`MATTER_VK_DIAGNOSTIC_MATERIALS`). Trivial, free to hoist.

## Next constraint

At 12 executors, `inflight` climbs to 13–16 and `pool` drops to 4–5: the wall is
`max_inflight` (16) and the fixed 32-slot completion pool
(`PublicationCompletionCapacity::kCapacity`). Below that, the 47.7 ms bake is
the real limit — which is where the terrain LOD ladder and the script-host
findings below start to matter.

## Parked: per-sector script-host cost

Measured but **not** acted on, because at 47.7 ms of bake per sector this is
noise. `make -C MatterEngine3/tests run-sector-profile` reproduces the table
(`sector_setup_profile.cpp`, args `[iterations] [parts-dir]`).

Fixed per-bake JS host cost is **~1.8–2.8 ms**, near-constant regardless of
sector content — 40–47% of a synthetic coarse-tile bake, but only ~6% of a real
StreamMountain sector. Composition (scatter case):

| item | ms | recoverable without giving up a fresh runtime? |
|---|---|---|
| 30× `std::filesystem::exists` on child artifacts | 0.95 | yes, entirely |
| parse/compile of `WorldSector.js` + 2 imports | 0.45 | yes — QuickJS `JS_WriteObject`/`JS_ReadObject` bytecode replay measured at 0.02 ms |
| `JS_NewRuntime` + intrinsics + part-base + ~90 bindings | 0.62 | no |
| re-reading shared-lib from disk (fold cache never hits) | 0.28 | yes, entirely |
| `JS_FreeContext`/`JS_FreeRuntime` | 0.16 | no |

Two specific defects if anyone picks this up:

- `script_host.cpp` stats **all 30 child artifacts on every bake** to classify
  them animated-vs-static (search `load_animation_link`). The child hash set is
  frozen at world install, and the path probed is the sector transient dir where
  child parts never live — 30 guaranteed misses per sector. If that path ever
  *did* resolve, `preflight_v2_file` reads the entire child `.part` and
  FNV-hashes it, so this is a landmine as well as a cost.
- `bake_and_stage_sector` constructs `script_host::ScriptHost` as a **stack
  local per sector**, so the mutex-protected fold cache never hits. One instance
  per pool executor fixes it — not one shared instance, since `bake_source`
  writes `last_merged_params_`/`last_buffer_`/etc. unlocked.

Also relevant: the resolved hash is computed **before** `build()` runs, from
(folded source, merged params, child hashes) — it is not a hash of the output.
So any cross-bake state leak would produce **different geometry under an
identical hash**, cached content-addressed and never re-baked. That is what the
fresh-runtime-per-bake discipline is buying, and why reusing a JS runtime needs
a byte-comparison gate rather than a hash check.
