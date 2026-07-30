# Sector Bake Time: Where The 5,000-Sector Fill Actually Goes (2026-07-30)

Follow-on to `streaming-fill-throughput-findings-2026-07-30.md`. That pass moved
the `VkScenePart` build to the bake worker and concluded the fill was
publish-bound. **It is not any more, and the number it left standing — "the bake
is 47.7 ms" — was measuring about a third of the real per-sector cost.**

Same rig: StreamMountain, RTX 4090 / 24 cores, editor `-O2 -DNDEBUG`, artifacts
to the per-pid transient dir. StreamMountain's full streamed disc is
**6,547 sectors** (rings out to 2922 m at 64 m sectors) — measured, not
estimated.

## TL;DR

At stock settings the disc takes **~5 minutes**. The cost is not the bake:

| stage | ms/sector | share |
|---|---:|---:|
| `bake_source` (what the old profile measured) | 62.6 | 34% |
| **`PartStore::stage_load`** | **119.7** | **65%** |
| `build_vulkan_part` prebuild (last pass's work) | 2.0 | 1% |
| **executor task total** | **184.3** | |

and inside `stage_load`, **the LOD ladder re-bake is 105.7 ms — 57% of
everything**. The bake writes a full-res voxel mesh to disk, `stage_load` reads
it straight back (10.9 ms) and re-derives a three-rung QEM ladder from it.

Measured end to end:

| config | rate | full 6,547-sector disc |
|---|---:|---:|
| stock (terrain LOD off, 4 executors) | 22/s | **~5.0 min** |
| terrain LOD off, 12 executors | 51/s | ~2.1 min |
| **terrain LOD on, 12 executors** | **136/s** | **48.3 s** (measured to completion) |

## What the previous doc got wrong, and why

1. **`stage_load` was never measured.** `MATTER_STREAM_BAKE_PROFILE` times
   `bake_source` only. `part_store.h` estimated `stage_load` at "20-40 ms"; it
   is 119.7. Everything downstream of that estimate was mis-attributed.

2. **`[stream.rate]`'s `pool` column cannot indicate bake-boundness.**
   `bake_pool_outstanding()` returns queue+active, and the dispatcher breaks out
   at `queue + active >= stream_worker_count` — so it reads exactly
   `stream_worker_count` whenever any work is pending. "pool at its cap =
   bake-bound" is unfalsifiable. Real executor utilisation is **82-86%**, not
   the "~21%" the old doc inferred from `bake_source` time ÷ wall time.

3. **Per-sector stderr logging perturbs the run badly enough to invert
   conclusions.** With `MATTER_BAKE_PROFILE=1` + `MATTER_STREAM_BAKE_PROFILE=1`,
   bakes over a *matched sector population* measured **81.6 ms** against
   **44.9 ms** quiet, and fill throughput halved. All numbers here come from
   runs with per-sector lines off; the new counters aggregate into atomics and
   render once per `[stream.rate]` (~1 line per 12 s).

## Full breakdown, stock settings (n=1860, 4 executors, 85% busy)

```
executor task                        184.3 ms
├─ bake_source                        62.6 ms  34%
│    ├─ build (JS build() call)      ~35    ms      <- 60% of bake_source
│    │    ├─ terrainVolume mesher     30.0  ms      6000 tris out
│    │    ├─ heightAt/slopeAt          0.3  ms      136 calls
│    │    ├─ biomeAt/moistureAt        0.5  ms      197 calls
│    │    └─ JS interpretation        ~4    ms      (residual)
│    ├─ save (.part write)            ~12   ms
│    ├─ mesh (finalisation)            ~7   ms
│    └─ host fixed                     ~4   ms      fold/ctx/eval/merge/free
├─ stage_load                        119.7 ms  65%
│    ├─ read (.part decode)            10.9 ms      reads back what save wrote
│    ├─ prep                            0.5 ms
│    ├─ LOD ladder re-bake            105.7 ms      57% OF THE WHOLE TASK
│    └─ tail (raster mesh, clusters)    2.6 ms
└─ build_vulkan_part prebuild           2.0 ms   1%
```

The scatter loop is **not** a cost: 333 field queries per sector for 0.8 ms
total. `build` is essentially one `terrainVolume` call plus JS overhead.

### Inside the ladder — the 92/8 split (n=614)

```
rung   decimate  reproject  chart  blas   total    tris
  0        0.0        0.0     2.5   7.2    9.6 ms   6133 -> 6133   (eps 0: full detail)
  1       22.2       37.0     0.5   1.2   60.9 ms   6131 -> 1086
  2       24.0       18.8     0.3   0.6   43.8 ms   6131 ->  595
                                        --------
                                         114.3 ms
```

Three findings here:

- **The rung that renders costs 9.6 ms; the two coarse rungs cost 104.7 ms.**
  A sector 2.9 km out will only ever draw rung 2, and still pays rungs 0 and 1.
  A near sector draws rung 0 and pays 105 ms for rungs it will not use until
  the camera backs away. Baking only the rung a sector will actually render at
  is worth ~60-105 ms/sector; the cost is that a camera approach then needs a
  demand-driven promotion (the chart-VT registration path already has this
  shape).
- **`reproject_triex` is the single largest line item** (37.0 + 18.8 =
  55.8 ms/sector), ahead of QEM decimation (46.2 ms). Rung 1 carries the
  one-time `ReprojectSource` BVH build over the 6131-triangle full-res surface;
  rung 2 reuses it, which is why its reproject is proportionally cheaper.
- **Both coarse rungs decimate from full-res, so decimation costs ~2x what it
  needs to** (22.2 vs 24.0 ms for wildly different outputs — QEM cost tracks the
  *input* size). Cascading rung 2 from rung 1's output instead of from full-res
  nearly halves it, at the price of changing the error-bound semantics from
  absolute-to-original to compounding.

## Why more threads stop helping (measured 4 / 12 / 20)

| executors | rate | busy | task ms | inflight | pool | limiter |
|---:|---:|---:|---:|---:|---:|---|
| 4 | 22/s | 82% | 149 | 4 | 4 | bake pool |
| 12 | 51/s | 85% | 194 | 8 → 96 | 12 | crosses over to publish |
| 20 | 51/s | **56%** | 218 | 96 (pinned) | **0** | publish; executors starve |

Three separate effects:

1. **The ceiling is the render thread, at ~51 sectors/s.** At 20 executors the
   pool is empty (`pool=0`) and `inflight` is pinned at the cap: everything is
   baked and queued for publish. Publishes are pumped once per frame, and the
   main thread is running **~10 FPS** during a fill —
   `[stream.frame] build=32-87 ms draw=30-98 ms instances≈90k`. That is the
   throughput wall, and no number of executors moves it.
2. **Per-task cost inflates ~46% from 4 → 20 threads** (149 → 218 ms). The
   ladder is memory-bandwidth-bound: `decimate` 11.1 → 14.1 ms/rung,
   `reproject` 15.0 → 21.7 ms/rung on identical work. Parallel efficiency at 20
   threads is well under half.
3. **Two caps were silently binding.** `MATTER_STREAM_WORKERS` was clamped to
   16, so a `=20` sweep quietly measured 16. `PublicationCompletionCapacity::
   kCapacity` was 32, bounding `max_inflight` and therefore throughput by
   Little's law. Both raised (32 / 128) on this branch.

### `build_ms` is not the number the last pass fixed

The main-thread `build_ms` in the editor overlay reads 32-87 ms during a fill.
That is **`VkSceneRenderer`'s per-FRAME instance build**, proportional to the
~90k *resolved* instances — not the per-PUBLISH `stream.publish` job that went
13.9 → 1.46 ms. They are different code paths that happen to share a thread.
`static_upload_dirty_ == kAppend` (the streaming fast path in
`vk_scene_renderer.cpp`, `issues/render-streaming-build-cpu`) already removed
the O(world) static rewrite; what remains is per-frame dynamic instance work,
and it is now the fill's ceiling. Note it is already ~43 ms at 522 resident
sectors — it does **not** grow with the disc, so this is a fixed per-frame cost,
not a cascade.

## The one-line lever available today

`904554a5` ("Default tweaks") flipped the heightfield terrain LOD ladder from
default-on to default-off, so every sector — including ones 2.9 km out — bakes
a full-res voxel mesh and then QEM-decimates it into a full ladder. Only ~700
of the 6,547 sectors fall inside StreamMountain's own LOD-5 band (961 m); the
world still declares its tuned `terrainBands` out to 10095 m.

`MATTER_TERRAIN_LOD=1`, marginal cost of the far sectors that make up the bulk
of the disc:

| | ladder off | ladder on |
|---|---:|---:|
| marginal task/sector | 173 ms | **31 ms** |
| of which ladder | 104 ms | **1.8 ms** |
| triangles into the ladder | 6001 | 1076 |

With 12 executors that is **48.3 s for the complete 6,547-sector disc**, versus
~5 minutes stock — **6.2x**. Under that config the bottleneck flips back to
`bake_source` (52.6 of 72.1 ms), i.e. the `terrainVolume` mesher.

## Ranked next levers

1. **Turn the terrain LOD ladder back on** (or fix whatever quality regression
   caused `904554a5`). 6.2x, no code change. Everything else is worth less.
2. **Bake only the rungs a sector will render.** ~60-105 ms/sector at stock
   settings, ~92% of the ladder. Needs demand-driven rung promotion.
3. **Skip the disk round-trip.** `save` (12 ms) writes a `.part` that
   `stage_load` immediately reads back (11 ms) — 23 ms/sector for data that was
   just in memory. Streamed sector variants are transient by design and get
   **zero** resolve-cache hits, so the artifact has no reader but the very next
   line of code. Handing the in-memory mesh straight to staging skips both.
4. **Raise the default executor count from 4.** `min(4, cores/4)` is 4 on a
   24-core machine; 12 more than doubles throughput (22 → 51/s). The 4 is a
   race-validation artifact, not a measured optimum.
5. **Cascade coarse rungs** instead of decimating each from full-res (~20 ms).
6. **Per-frame `build_ms`** (~45 ms, ~10 FPS during fill) — the real ceiling
   above ~51 sectors/s, and it caps the fill no matter what the workers do.

## Instrumentation added (all rendered under `MATTER_STREAM_FILL_PROFILE=1`)

Aggregate counters, printed once per `[stream.rate]` line. **Deliberately not
per-sector** — see the perturbation finding above.

| line | contents |
|---|---|
| `[stream.task]` | task total, bake / stage / prebuild split, executor busy% |
| `[stream.stage]` | `stage_load` split: read / prep / ladder / tail |
| `[stream.ladder]` | per rung: decimate / reproject / chart / blas + tri counts |
| `[stream.build]` | terrain mesher vs `heightAt`/`slopeAt`/`biomeAt` calls |
| `[stream.frame]` | last frame's resolve / build / draw + resolved instances |

Backing APIs: `lod_bake::ladder_census()`, `dsl::terrain_verb_census()`,
`PartStore::StagedPart::{read,prep,ladder,tail}_ms`. Counters always accumulate;
cost is a few `steady_clock` reads per sector and per rung, plus two per world-
field DSL call (~333/sector ≈ 17 µs against a 150 ms task — 0.01%).

Suites green after the changes: `run-sectorcoord`, `run-sectorstream`,
`run-partstore`, `run-partstore-race`, `run-baketrace`.

## Method note

Launch from a shell that has **not** exported the MSYS2 UCRT64 PATH, set env
vars in the child's own environment, and echo what was delivered — otherwise
vars silently fail to reach a native Windows exe. Compare bake costs only over
**matched sector populations**: dispatch is nearest-first and near sectors carry
more scatter, so "mean over the run" drifts with how far the fill got. Two
early conclusions here were wrong until both controls were in place.
