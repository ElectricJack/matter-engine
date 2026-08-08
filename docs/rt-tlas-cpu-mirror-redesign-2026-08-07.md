# RT/TLAS CPU-mirror redesign: from O(world) per frame to O(changed)

Design note — 2026-08-07
Scope: `MatterEngine3/src/render/vk_scene_renderer.cpp` (the ray-tracing mirror
that feeds the TLAS build). Documentation only; no code changed by this note.

## Problem

A StreamMountain fly-through (RTX 4090, RT on, trees, ~590 mid-flight frames,
"mostly loaded") measured a **26 ms / 38 fps** frame, p95 35 ms. The breakdown:

- **GPU ~18 ms**, of which 82% is the TLAS rebuild (`gpu_tlas_ms = 14.76`); the
  actual ray trace is ~1.1 ms.
- **CPU render thread ~25 ms** — this is the frame gate. Inside it,
  `draw.cull_render = 11.58 ms` and `build = 7.16 ms`.

The TLAS build cost was already addressed this session: `emit_ray_instances`
flipped the TLAS build flag `PREFER_FAST_TRACE → PREFER_FAST_BUILD`
(`vk_scene_renderer.cpp:10345`). A shallow one-level TLAS gets only a marginal
traversal-quality benefit from FAST_TRACE while paying a large build cost, so
this is expected to drop `gpu_tlas_ms` from 14.76 to ~5–7 ms. **That buys GPU
headroom but does not move the 26 ms frame**, because the CPU render thread — not
the GPU — is the gate.

The CPU gate is dominated by **full per-frame re-derivation of the RT mirror**,
done regardless of what actually changed frame-to-frame. Two functions own it:

- **`build_ray_geometry`** (`vk_scene_renderer.cpp:9743`). For every
  `rt_instances_` entry × every cluster it re-runs: skin-ownership exclusion
  (`animation_skin_raster_owns_cluster`), animation-bounds resolve
  (`resolve_animation_cluster_union`), `cluster_distance_to_eye` (an AABB
  transformed through `object_to_world`), the `max_draw_distance` test, and LOD
  rung selection (`select_cluster_lod_view`). That is O(instances × clusters)
  float math *every frame*. (The per-instance `part_slot_lookup` here is already
  a flat open-addressed probe (`vk_scene_renderer.cpp:6341`), **not** a `std::map`
  descent — the code comment at line 9762 is explicit about this. It is not the
  cost; the re-derivation loop is.)
- **`emit_ray_instances`** (`vk_scene_renderer.cpp:10163`). Rebuilds the full
  `VkAccelerationStructureInstanceKHR` vector and the parallel `GpuRtPartRecord`
  (`rt_parts`) table every frame, then **`memset`s the entire `rt_parts`
  allocation at its session high-water capacity every frame**
  (`vk_scene_renderer.cpp:10259–10261`) before copying the live records, plus an
  O(n) `memcmp` TLAS-reuse gate (line 10305–10311).

### The one fact that makes this fixable

Confirmed against source: **instance transforms are static per instance.** An
`RtInstance` carries `part_hash` and a fixed `transform[16]`
(`vk_scene_renderer.h:684`), and trees/sectors do not move. Frame-to-frame, only
two things change: **set membership** (which instances are resident) and **BLAS
refs** (which LOD rung each cluster traces). Everything else is invariant.

The obstacle to exploiting that is that `instanceCustomIndex` is currently
**positional**: it is assigned `part_records.size()` at push time
(`vk_scene_renderer.cpp:10193`) and indexes the parallel `rt_parts` table by that
same position. A middle insert or remove renumbers every following record, so
there is no stable identity to patch — which forces the full rebuild.

## Proposed design: a stable-slot, incrementally-maintained RT mirror

### 1. Stable slots via a free-list

Give each `(rt_instance, cluster)` pair a persistent slot drawn from a
renderer-owned free-list. Set `instanceCustomIndex = slot` and index `rt_parts`
by `slot`. Slots are allocated when a cluster first becomes resident+traced and
returned to the free-list when it stops being either.

The enabling Vulkan fact: an instance whose
`accelerationStructureReference == 0` is **inactive** per the Vulkan spec —
traversal skips it entirely. So the instance array is allowed to contain
**holes**: a freed slot is a zeroed instance record (and a zeroed `rt_parts`
entry) that the hardware ignores. This removes the renumbering problem
completely — a slot's index never changes for the life of its `(instance,
cluster)`.

The shader side needs **no change**. `load_rt_surface` reads
`rt_parts[gl_InstanceCustomIndexEXT]` and already guards on `part.valid == 0u`,
returning an invalid surface and incrementing `invalid_part_records`
(`shaders_vk/rt_surface_common.glsl:314–319`). A zeroed (freed) slot reads
`valid == 0` and is handled by the existing path; a freed instance record is
never traversed at all, so it will not even reach the shader.

### 2. Persist and patch, don't rebuild

Keep the instance vector and the `rt_parts` mirror alive across frames as
renderer-owned state. Each frame, patch **only**:

- slots whose **membership** changed (an instance/cluster entered or left the
  resident set — derived from `update_instances` deltas), and
- slots whose selected **BLAS rung** changed (`accelerationStructureReference`
  and the `rt_parts` addresses move to the new LOD's geometry).

This converts O(world) per-frame work into O(changed). It also **kills the
full-capacity `memset`**: a slot is zeroed exactly once, at the moment it is
freed, instead of the whole high-water allocation being cleared every frame. The
O(n) `memcmp` reuse gate becomes unnecessary — the renderer already knows whether
anything changed, because patching *is* the change set.

### 3. Amortize the selection math

The remaining per-frame cost in `build_ray_geometry` is the LOD-rung recompute
(`cluster_distance_to_eye` + `select_cluster_lod_view`). RT shadow/GI rung
selection needs no per-frame exactness, so gate it:

- **Distance-band hysteresis** — recompute a cluster's rung only when its
  distance-to-eye crosses a band edge (the switch distances the rung ladder
  already defines), not every frame; or
- **Round-robin** — refresh 1/N of the resident set per frame.

Either bounds the transcendental/transform math to the clusters that could
plausibly have switched.

> **Warning — do NOT key "changed" on `instance_generation_` or any
> publish-bumped counter.** This is the engine's documented *generation-gate
> trap*: `instance_generation_` bumps on every publish (`vk_scene_renderer.cpp`
> has ~a dozen `++instance_generation_` sites, including the dynamic-merge path),
> and publishes happen every frame while the camera moves — so a
> generation-keyed gate is a no-op exactly when it matters. Key on **actual
> membership deltas and rung transitions**, nothing else.

### 4. Correctness backstop

- The **`rt_geometry_epoch_` bump stays the full-rebuild fallback**, exactly as
  today. It is bumped on any BLAS build (`vk_scene_renderer.cpp:10088`), part
  release, and `reset()` — every event that can free or relocate bottom-level
  memory a cached structure points at. When the epoch changes, discard the
  incremental mirror and rebuild from scratch.
- The **TLAS itself still fully rebuilds whenever anything changed** — a TLAS
  build consumes the entire instance buffer; there is no partial TLAS update on
  this path (and the BLAS keeps `MODE_BUILD`, not `MODE_UPDATE`). But with
  FAST_BUILD already landed that rebuild is cheap on the GPU. **The win here is
  that *producing the inputs* drops from O(world) to O(changed)** — that is the
  CPU render-thread work, which is the frame gate.

## Risks / watch-outs

- **Generation-gate trap** (see §3). The single most likely way to get a
  silently-broken gate. Any counter that bumps on publish is disqualified as the
  change signal.
- **Stale-tail hazard.** An SSBO's length in the shader is its *buffer capacity*,
  not the live count (`ensure_buffer` never shrinks and grows by a factor). A
  stale record is *structurally valid* (`valid == 1`, `vertex_stride == 88`, a
  real `primitive_count`) while its addresses dangle at freed geometry — the
  device-lost trap the current full `memset` exists to prevent
  (`vk_scene_renderer.cpp:10247–10258`). The free-list design must **zero a slot
  on free** so the tail (freed slots + never-allocated capacity) always reads
  `valid == 0`. This is a hard invariant, not an optimization: skipping the
  per-frame `memset` is only safe if freed slots are individually cleared.
- **Keep the epoch fallback.** Freed BLAS memory must still force a full rebuild
  via `rt_geometry_epoch_`; the incremental path is an accelerator layered on top
  of it, never a replacement.

## Verification plan

**Metrics that should move** (StreamMountain, same fly-through, RT on):

- `cpu_build_ms`: 7.16 → ~2–3
- `cpu_draw_cull_render_ms`: 11.58 → lower (rung math amortized, no full mirror
  re-derivation)
- median frame: 26 ms → **sub-20 ms** (target)
- `gpu_tlas_ms`: already ~5–7 from the FAST_BUILD flag (independent of this
  change)
- `invalid_part_records`: must stay at its current baseline — a spike means a
  freed slot was traversed or left non-zero (the stale-tail invariant broke).

**Debug validator.** In debug builds, after producing the incremental mirror,
also run a from-scratch `build_ray_geometry`/`emit_ray_instances` pass into a
scratch buffer and assert the two `instances`/`rt_parts` images are equal on the
live slots (and that every non-live slot is zeroed). This catches missed deltas
and dangling tails at the source rather than as a device-lost crash three frames
later.

**Pixel-equality gate.** RT output must be bit-identical before/after. Use the
shot-replay loop (`MATTER_REPLAY`) with a cold `.cache` so the same bake feeds
both runs; the incremental mirror is a pure reorganization of how the same TLAS
inputs are produced, so a matching render is the real acceptance test.

## Scope / effort

Medium. The work is contained to `vk_scene_renderer.cpp` plus a small amount of
renderer-owned state (the free-list, the persistent instance/`rt_parts` mirror,
per-slot last-rung/last-distance bookkeeping). No shader change, no format
change, no descriptor-layout change — the `valid == 0` guard and
`invalid_part_records` counter already tolerate holes. Rough estimate: a few
days including the debug validator and a replay pass.

**This is the actual frame-time lever.** The FAST_BUILD flag alone reduces GPU
TLAS cost but the CPU render thread remains the gate at ~25 ms; only moving the
mirror production from O(world) to O(changed) moves the 26 ms frame.
