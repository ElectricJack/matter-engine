# Cell-Granular Interior Particle Culling — Design

**Date:** 2026-06-14
**Status:** Approved (design); ready for implementation plan
**Supersedes:** the culling-granularity portion of `2026-06-14-occupancy-interior-culling-design.md`. The lattice/occupancy components and the determinism, error-handling, and pipeline-isolation principles from that doc are unchanged and still authoritative.

## Why This Revision

The original design culled at **per-slot** granularity: a slot was dropped if every slot in its Chebyshev margin box was occupied. The A/B acceptance test proved this is the wrong granularity.

What the A/B run found, and what reading the meshing code confirmed:

1. **Each meshing cell builds its SDF from only its own particles.** `Cell::generate_mesh_for_group` (cell.cpp:293) feeds `GenerateMesh` only the particles assigned to that cell. A particle is assigned to every cell whose bounds its *sphere* overlaps (`Cluster::update_cell_meshes` → `intersects_sphere`, cluster.cpp:256), so cells share particles only within ~one particle radius of a shared boundary.
2. **A fully-packed interior cell already emits zero triangles.** Marching cubes only produces a surface where the field crosses the isolevel. In the full-density (bypass) run the visible shell (~53,913 triangles) comes entirely from boundary cells; deep cells emit nothing.
3. **Per-slot culling punched holes inside otherwise-solid cells.** Dropping individual buried slots made a kept cell's field cross the isolevel internally, so marching cubes wrapped each cavity in a hidden **inner** surface. Measured result: culled margin-2 produced *more* triangles (68,800) than bypass (53,913), and the extra inner triangles were uploaded to VRAM and registered with the BLAS (137,600 BLAS triangles culled vs. 107,826 bypass). Raising the margin only shrinks the cavity; it never removes it.

The fix is to move the keep/drop decision from the slot to the **meshing cell**, and make it **all-or-nothing per cell**.

## Goal

Eliminate interior particles before they reach the (unchanged) meshing pipeline, cutting particle count and mesh-generation compute, while reproducing the outer surface **essentially exactly** (same triangle count within a small tolerance) and producing **no inner cavity**. "Essentially exactly" rather than "byte-identical" is acceptable per explicit user direction.

## Non-Goals

Unchanged from the prior design: no changes to the meshing pipeline (`Cluster`, `Cell::generate_mesh_for_group`, `GenerateMesh`, simplifier, BLAS registration); no LOD generation; no instancing/streaming/occlusion culling; no new lattice types beyond the regular grid.

## The Core Rule

> Keep **all** particles of a meshing cell if the cell contains **any** non-buried slot. Drop **all** particles of a cell only when **every** slot in it is buried (the existing `slot_is_buried` Chebyshev test, margin ≥ 1).

Two properties make this correct:

- **Atomicity (no cavity).** A kept cell keeps *all* its slots, so its field is identical to the full-density field for that cell — no internal isolevel crossing is introduced, so no inner surface forms.
- **Margin (no cross-cell perturbation).** A dropped cell is one where every slot is buried with margin ≥ 1, i.e. the cell and a one-slot halo are fully occupied. The particles we drop are deep enough that their spheres do not reach into any surface cell, so dropping them cannot perturb a neighboring kept cell's field. The default margin stays at **2** (conservatively safe; the A/B test may lower it).

Because every cell we drop would have emitted zero triangles anyway, the outer surface is reproduced and the win is pure: fewer particles fed in, fewer cells created, less marching-cubes compute. Interior cells are never created (the `Cluster` creates cells on demand from particles), so they are never meshed, uploaded, or BLAS-registered.

## Cell/Slot Mapping (correctness crux)

The cull must bucket slots into the **same** cell grid the `Cluster` uses, or atomicity is meaningless. The `Cluster` grid is:

```
cell_coord = floor(local_position / cell_size)      // Cluster::get_cell_coordinates, cluster.cpp:170
cell_size  = smallest_cell_size * (1 << lod_level)  // get_current_cell_size
```

At this sub-project's target LOD 0, `cell_size == smallest_cell_size`.

The bucketing must use the **final local position** each particle is added with — i.e. *after* the scene's half-extent re-centering offset is applied — because that is the position `Cluster::add_particle` sees and grids on. The cull therefore:

1. Determines, per cell, whether the cell is buried (every slot buried) or a surface cell (any slot non-buried), bucketing each slot by `floor(final_local_position / cell_size)`.
2. Emits particles for every slot in surface cells; emits nothing for buried cells.

`cell_size` is a new field on `CullParams`. The caller passes the scene's `smallest_cell_size`.

> **Phase-alignment note for the plan:** because a particle's sphere can overlap into a neighbor cell, a slot whose center sits in a buried cell but whose sphere reaches a surface cell is already covered — that slot's cell is buried only if the one-slot halo is also occupied (margin ≥ 1), which means its sphere reaches only into other buried cells. No special boundary handling is required beyond keeping margin ≥ 1.

## Components Changed

### `particle_culling.h` / `particle_culling.cpp`

- `CullParams` gains `float cell_size;` (meshing cell size used for bucketing).
- `cull_interior` is **replaced outright** with the cell-atomic algorithm. The per-slot emission path is removed (not kept as an alternate mode).
- `slot_is_buried`, `emit_all`, `make_particle`, and the deterministic `lattice_vhash`/`lattice_vnoise` helpers are **unchanged**.
- Algorithm:
  1. First pass over occupied slots (`Occupancy::for_each`): compute each slot's cell key `floor(slot_final_pos / cell_size)`; record per cell whether it has seen any non-buried slot (`!slot_is_buried(occ, c, margin)`). Store in a `std::unordered_map<uint64_t, bool> cell_keeps`.
  2. Second pass over occupied slots: if the slot's cell is marked keep, emit its particle via `make_particle`.

  Two passes keep the decision independent of iteration order (a cell is kept iff *any* of its slots is non-buried, which the first pass fully resolves before any emission). Determinism is preserved: each emitted particle still depends only on its own slot + seed.

  Note: `make_particle` applies jitter, so the *emitted* position can differ slightly from `slot_position`. Bucketing for the cell key must use the same position basis the caller will pass to `add_particle`. The plan resolves this by having the cull compute the cell key from `slot_position` plus the same re-centering offset the caller applies; jitter is small relative to `cell_size` and, because culling is atomic per cell and conservative (keep-if-any), a jittered particle near a cell boundary cannot create a hole. (See Open Questions.)

### `main.cpp` `setup_lattice_scene`

- Apply the half-extent re-centering offset to slot positions *before* (or consistently with) cell bucketing, and pass the scene's `smallest_cell_size` as `CullParams::cell_size`.
- Logging extends from `[cull] occupied=… emitted=…` to also report cells kept / cells dropped, e.g. `[cull] occupied=8000 emitted=… cells_kept=… cells_dropped=… (margin=2)`.
- `MSL_CULL_MARGIN = -1` still bypasses culling via `emit_all` (unchanged).

### Tests — `tests/particle_culling_tests.cpp`

- **Replace** `test_cull_counts` (per-slot expectations) with cell-atomic expectations. For a small block at a known `cell_size`, assert kept-particle count equals "all particles in any cell touching the surface band" and that no buried cell contributes.
- **Add** the anti-cavity invariant test: for any cell that emits at least one particle, it emits **all** of that cell's occupied slots (no partial cell).
- `test_grid_lattice`, `test_occupancy`, `test_burial`, `test_thin_shape_keeps_all`, `test_determinism` remain; `test_determinism` and `test_thin_shape_keeps_all` are updated only as needed to pass the new `cell_size` field.

### Acceptance test (Task 4 A/B) — metric restored

Re-run the headless A/B (culled vs. `MSL_CULL_MARGIN=-1` bypass) with identical camera. Expected, and now achievable:

- **Surface triangle count:** culled ≈ bypass within a small tolerance (target: equal or within a few %, where any difference is from boundary-cell jitter, not an inner surface).
- **BLAS-registered triangles:** culled ≤ bypass (no inner-surface inflation).
- **Particles fed in:** culled ≪ bypass.
- **Mesh-gen time:** culled < bypass.
- **Visual PNG diff:** matches within anti-aliasing tolerance.

If culled surface triangles materially *exceed* bypass, the atomicity guarantee is violated — treat as a failure and debug the cell/slot mapping.

## Error Handling

Unchanged from the prior design, plus: `cell_size <= 0` is invalid; clamp/assert in debug builds (a non-positive cell size makes bucketing meaningless). `margin < 1` is still clamped to 1.

## Risks / Open Questions for the Plan

- **Jitter vs. cell boundary.** Resolve by bucketing on the un-jittered `slot_position + recenter_offset` (the slot's nominal cell), not the jittered emit position. Because culling is keep-if-any and atomic, this is safe; the plan should state it explicitly and the anti-cavity test should guard it.
- **Re-center offset consistency.** The single most likely bug is a mismatch between the offset used for bucketing and the offset used at `add_particle`. The plan must compute the offset once and use it in both places, and the A/B triangle-count check is the backstop that catches a misalignment.
- **LOD.** Bricks are baked at LOD 0 here; `cell_size == smallest_cell_size`. Higher-LOD baking (larger cells) drops *more* interior cells and remains correct under the same rule, but is validated later.

## File Structure

- Modify: `MatterSurfaceLib/include/particle_culling.h` — add `cell_size` to `CullParams`; update `cull_interior` contract comment.
- Modify: `MatterSurfaceLib/src/particle_culling.cpp` — replace `cull_interior` with the cell-atomic two-pass algorithm.
- Modify: `MatterSurfaceLib/main.cpp` — pass `cell_size`; extend cull logging.
- Modify: `MatterSurfaceLib/tests/particle_culling_tests.cpp` — replace `test_cull_counts`; add anti-cavity test; thread `cell_size` through existing tests.
- No Makefile / build-all.sh changes (files already compiled and gated from the prior sub-project).
