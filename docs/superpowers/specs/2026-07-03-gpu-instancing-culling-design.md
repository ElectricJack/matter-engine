# GPU-Driven Instancing & Culling

**Date:** 2026-07-03
**Status:** Approved design
**Target:** infrastructure package — GPU compute cull + HiZ occlusion + indirect
draw, proven on a new stress fixture. Meadow sweep must be no worse than the
frame-time package's Stage-3 numbers; scaling (not a ms figure) is the success
metric.

## Context

The frame-time package (spec `2026-07-03-frame-time-lod-design.md`) ended with
meadow at aerial 31 ms / corner 27 ms / midfield 23 ms / far 26 ms — and an
**empty-scene floor of 17 ms** that culling cannot touch. CPU cost is small
(resolve ~5 ms, build ~1 ms, draw ~2 ms); the residual is GPU triangle/fragment
work plus the fixed floor. That spec named GPU-driven culling/instancing as the
next package, ahead of imposters.

Decisions locked during design:

- **Goal is infrastructure, not a meadow ms target.** Success = the pipeline
  scales to 5–10× more instances with sub-linear CPU cost, demonstrated on a
  new stress fixture, while meadow stays no worse than today. The 17 ms
  empty-frame floor is a separate future package.
- **Occlusion first, indirect-draw scale second** — but they share plumbing
  (one compute pipeline), so both land in this package as staged milestones.
- **GL floor rises to 4.6** for the viewer (compute shaders, SSBOs,
  `glMultiDrawElementsIndirect`, `gl_BaseInstance` indexing). raylib itself
  stays at 3.3; newer entry points are called directly via glad. Fail-fast
  with a clear error on drivers reporting < 4.6. No fallback path after
  promotion.
- **Hybrid, not full GPU-driven:** the CPU `SectorLodResolver` stays (its
  sector index is what skips the dormant world cheaply and it already caches
  on the WorldState version). Only the per-cluster hot path — cull, LOD
  refine, batch build, draw submission — moves to the GPU.
- `MAX_LOD = 9` (matches the ratio-2 ladder's maximum rung count).
- Full `mat4` per instance (no TRS compression — bandwidth optimization
  deferred).
- Previous-frame HiZ (temporal); a 1-frame conservative-cull pop after camera
  cuts is accepted.

## Architecture

Three components change; one is new.

**Unchanged — `SectorLodResolver`.** Still produces a per-frame
`ResolvedInstance[]` (part_hash, transform, base LOD). Stage-1 sector-cache
behaviour untouched.

**Extended — `PartStore`.** On first load of a part, in addition to today's
CPU-side `LoadedCluster`/`RasterMeshData`, it:

1. Packs **all** the part's LOD meshes (every cluster × every rung) into a
   single indexed VBO+IBO — the *per-part mesh atlas* — with a CPU-side
   `MeshRange[part_id][mesh_idx] = {first_index, index_count, base_vertex}`
   table.
2. Appends the part's cluster metadata to a persistent global `ClusterMeta`
   SSBO and records the part's `cluster_range_start/count`. Parts that have
   meshes but **no clusters** (the whole-part path) get one *synthetic*
   ClusterMeta covering the whole part (part AABB + part-level ladder
   thresholds — the selection formula is identical to the resolver's
   `lod_select.cpp`, so the pick matches the CPU path).
3. Precomputes a flattened **expansion table**: the compositional
   `children` recursion in today's `build_batches` (depth ≤ 8) resolved once
   per part into a list of `(relative_transform, sub_part_id)` entries
   (identity + self for leaf parts). The GPU never recurses; the CPU
   instance-copy loop emits one `GpuInstance` per expansion entry, composing
   `instance_transform × relative_transform` — the same multiply
   `build_batches` does today, table-driven.

**New — `GpuCuller`.** Owns the GPU pipeline; knows nothing about parts, LOD
schemas, or raylib. Holds persistent state (ClusterMeta SSBO, mesh atlases by
reference, HiZ pyramid) and per-frame state (instance SSBO, draw-command SSBO,
DrawInstance SSBO). Each frame: (1) build HiZ from the previous frame's depth,
(2) dispatch cull+LOD compute, (3) issue one
`glMultiDrawElementsIndirect` per active part.

**Modified — `RasterComposer`.** `build_batches`, its fingerprint cache, and
`ensure_mesh`'s per-batch `UploadMesh` path are deleted (at Stage 5). `draw`
keeps setting global uniforms (sun/sky/material table/probe volume) on the
raylib-managed shader, then delegates geometry submission to
`GpuCuller::render(resolved, cam)`.

## Data Model

Persistent buffers (populated at part load, never freed — parts don't unload
today):

- **Per-part mesh atlas** — one VBO+IBO per part_hash; attributes position,
  normal, color, texcoord (same as `raster.vs`).
- **`ClusterMeta` SSBO** — global, append-on-load. Per-cluster record
  (std430, 128 B with padding):

```glsl
struct ClusterMeta {
    vec4  aabb_min_radius;    // xyz = local AABB min, w = bounding radius
    vec4  aabb_max_pad;       // xyz = local AABB max, w unused
    float thresholds[9];      // MAX_LOD = 9, finest -> coarsest
    uint  lod_mesh_idx[9];    // index into the part's MeshRange table
    uint  lod_count;
    uint  part_id;            // dense engine-side id
    uint  cluster_index;      // debug
    uint  pad;
};
```

Per-frame buffers (orphaned/rewritten each frame; soft cap 500k instances ×
up to `MAX_CLUSTERS_PER_INSTANCE` clusters, grown on demand, never shrunk):

- **`GpuInstance` SSBO** — memcpy'd from resolver output:

```glsl
struct GpuInstance {
    mat4  transform;            // engine row-major convention, transposed on
                                // upload exactly as row_major_to_matrix does
    uint  part_id;
    uint  base_lod;             // resolver's part-level pick (debug/HUD only;
                                // cluster-level selection is authoritative)
    uint  cluster_range_start;  // into ClusterMeta SSBO (fixed per part)
    uint  cluster_range_count;
};
```

- **`DrawElementsIndirectCommand` SSBO** — one slot per
  `(cluster_global_index, lod_level)` bucket:
  `bucket = cluster_global_index * MAX_LOD + lod`, where
  `cluster_global_index` is the cluster's slot in the ClusterMeta SSBO.
  (Buckets must be per-cluster, not per-part: each cluster×LOD is a distinct
  mesh with its own `first_index/index_count`.) Because ClusterMeta is
  appended per-part, a part's commands are contiguous — one
  `glMultiDrawElementsIndirect` per part spans
  `cluster_range_start*MAX_LOD .. (start+count)*MAX_LOD`. Cleared/seeded each
  frame from `MeshRange`.
- **`DrawInstance` SSBO** — per-visible-instance transform written by compute;
  vertex shader indexes it with `gl_BaseInstance + gl_InstanceID`.

**Instance allocation strategy:** start with **P1** — per-part
over-allocation: part P with `N_p` observed active instances reserves
`N_p × clusters_p × MAX_LOD` DrawInstance slots (each bucket's
`base_instance` seeded into that region), `atomicAdd` on the bucket's
`instance_count`. Meadow fits in ~25 MB (grass: 40k × 1 cluster × 9 × 64 B).
The 500k-instance stress fixture will exceed the ~200 MB comfort line — the
agreed trigger to switch to **P2**: single global atomic + count/prefix-scan/
place compaction (two cull dispatches + tiny scan). The swap is internal to
`GpuCuller`.

## Compute Pipeline

One clear pass (seed indirect commands from `MeshRange`), one cull pass.
Workgroup size 64; one thread per `(instance, cluster_local)` tuple, dispatched
as `total_instances × MAX_CLUSTERS_PER_INSTANCE` with early-out on
out-of-range cluster indices.

Per-thread cull logic:

1. Load `GpuInstance` and its `ClusterMeta`.
2. Transform the 8 local-AABB corners by the instance transform; frustum-test
   against 6 planes (Gribb-Hartmann, extraction math lifted verbatim from
   `raster_composer.cpp` / `raster_cull.h`).
3. **HiZ occlusion** (Stage 3+, runtime-toggleable): project the world AABB to
   NDC, take the tightest screen rect + nearest depth; pick the mip where the
   rect covers ~1–2 texels; cull if the pyramid's farthest-stored depth is
   nearer than the AABB's nearest depth. Depth convention (standard vs
   reversed Z) verified against the main pass during implementation; pyramid
   reduction op flips accordingly.
4. **LOD select:** the exact `raster_cull.h::cluster_lod_select` formula —
   projected size = radius × scale / distance × pixel_budget; scan thresholds
   finest→coarsest, pick first ≥ threshold, else coarsest. Matching current
   CPU behaviour, cluster-level selection is authoritative; the resolver's
   `base_lod` matters only for clusterless parts, whose synthetic ClusterMeta
   reproduces the part-level ladder so the same formula yields the same pick.
5. Emit: `slot = atomicAdd(cmd[bucket].instance_count, 1)`; write
   `DrawInstance[cmd[bucket].base_instance + slot]`.

**Parity requirement:** frustum math, projected-size formula, threshold scan
direction, and pixel_budget handling are ported verbatim. A headless unit test
dispatches the compute shader on a meadow-derived fixture and requires
bucket-level equivalence with the CPU `build_batches` output (identical
instance counts per bucket; per-bucket transform sets equal). HiZ is disabled
in the parity test via its runtime toggle.

## HiZ Pass & Render Integration

**Pyramid:** R32F mip chain (~11 mips at 1280×720, ≈5 MB), rebuilt each frame
from the main pass's depth attachment via a `hiz_downsample.comp` dispatch per
mip (2×2 max-reduce). Resized with the framebuffer. Frame N culls against
frame N−1's pyramid; the first frame after a camera warp may over-cull for
one frame (accepted; a warp-disable flag is the fallback if it's visually
noisy).

**Draw path (per active part):** bind the part's atlas VAO, bind the
DrawInstance SSBO, `glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
part_cmd_offset, cluster_range_count * MAX_LOD, 0)`. Zero-instance buckets
draw nothing for free.

**Shaders:** `raster_gpu_driven.vs` duplicates `raster.vs` but reads its
instance transform from the SSBO via `gl_BaseInstance + gl_InstanceID`
instead of the raylib instance-attribute path. Fragment shading unchanged
(same lighting/probe code).

**Feature flag:** `MATTER_GPU_CULL` env var selects the path. Default off
through Stage 3; flipped to default-on at Stage 5, after which the CPU path
and old shader are deleted.

**rlgl hygiene:** `rlDrawRenderBatchActive()` flushes raylib's pending state
before raw GL calls; framebuffer/texture-unit state restored after (same
pattern as the probe path's `GL_TEXTURE_3D` binds on units 4–5).

## Stages

Each stage ends with the meadow sweep (`meadow_sweep.sh`) and, from Stage 4,
the stress sweep. CSV rows are committed.

- **Stage 0 — Prep.** Mesh atlas + ClusterMeta upload land in `PartStore`;
  nothing reads them yet. Meadow sweep within ±0.5 ms of the frame-time
  package's Stage-3 rows.
- **Stage 1 — Compute cull, CPU draw.** Frustum-only compute pass writes
  indirect commands + DrawInstance transforms; CPU reads both buffers back
  and reconstructs the batch list for the *existing* `DrawMeshInstanced`
  loop. The readback is throwaway bridge code (slow is fine) — it exists so
  cull correctness is proven before the draw path changes. Parity test is
  the gate.
- **Stage 2 — Indirect draw.** Real `glMultiDrawElementsIndirect` +
  `raster_gpu_driven.vs`, behind `MATTER_GPU_CULL=1`. Gate: pixel-identical
  screenshots vs Stage 1 on the 5-camera set.
- **Stage 3 — HiZ occlusion.** Pyramid build + compute sampling; HUD checkbox
  + FIFO `hiz on|off`. Gate: occlusion fixture test (big occluder in front of
  many clusters; `stat_culled_by_hiz` reports the expected count). Meadow
  gains expected modest (few good occluders) — the sweep is a regression
  gate here, not the perf demonstration.
- **Stage 4 — Stress fixture.** New deterministic world `stress_forest`
  (500k seeded tree instances over 2 km × 2 km, ~10 unique tree parts,
  terrain underneath), `MATTER_WORLD=stress_forest`, instance-count dial via
  env var. New `stress_sweep.sh` → `MatterEngine3/docs/perf/stress_sweep.csv`.
  Characterize 50k / 100k / 200k / 500k: CPU split must stay sub-linear;
  GPU cost tracks visible clusters. P1→P2 swap if bucket memory demands it.
- **Stage 5 — Promotion & cleanup.** `MATTER_GPU_CULL` default-on; delete
  `build_batches`, fingerprint cache, per-batch `ensure_mesh` uploads, old
  `raster.vs` instance-attribute path; update `MatterEngine3/docs/
  rendering.md` + `architecture.md`; final "stage5-promoted" sweep rows.

## Testing & Verification

Headless suites in `MatterEngine3/tests/`:

- **gpu_cull_parity_tests** — compute cull ≡ CPU `build_batches` on a meadow
  fixture (bucket instance counts identical; transform sets equal). HiZ off.
- **gpu_lod_tests** — projected-size/threshold parity incl. `pixel_budget`
  dial values.
- **hiz_downsample_tests** — pyramid reduce correctness on a hand-authored
  depth image.
- **mesh_atlas_tests** — `MeshRange` geometry byte-equal to `RasterMeshData`
  across every LOD of a fixture part.
- **stress_forest_determinism_tests** — same seed ⇒ identical layout.

Existing `viewer_logic_tests` guard the CPU path until Stage 5 removes it.

GPU-context tests (parity, hiz) need a GL context; they run under the same
windowed-test arrangement the viewer uses (skipped headless-CI if no context,
run locally as part of the stage gates).

Visual verification: 5-camera screenshot set diffed at Stage 2 (pixel-exact)
and eyeballed at Stage 3 (occlusion should not visibly change any image);
final interactive fly-through by Jack on a freshly built `viewer.exe`
(`make windows` after every stage, clean-rebuild after header changes).

## Out of Scope (tracked)

- This-frame two-phase occluder HiZ (previous-frame only in this package).
- Imposter far-field tier (queued after this package).
- The 17 ms empty-frame GPU floor (separate investigation).
- GPU-side sector/active-radius filtering (full GPU-driven — Approach 3).
- TRS instance compression, bindless textures, mesh shaders.
- Part unloading / ClusterMeta slot reuse.
- MatterSurfaceLib changes (read-only rule).
