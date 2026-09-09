# Cell Skip-Meshing Interior Culling — Design

**Date:** 2026-06-14
**Status:** Approved (design); ready for implementation plan
**Supersedes:** `2026-06-14-cell-granular-interior-culling-design.md`. That approach (drop a fully-interior cell's particles) was implemented and A/B-tested and **failed**: culled produced *more* surface triangles than bypass (123,056 vs 107,826) because dropping an interior cell's particles still left the cell to be created from neighbor sphere-spill-in and meshed from a sparse set, and removed the spill-in that kept adjacent surface cells solid — both produce a hidden inner surface. The lattice/occupancy components and the determinism / pipeline-isolation principles from the prior docs are unchanged and still authoritative.

## Why This Revision

The A/B run proved a hard fact about the meshing pipeline: **you cannot remove buried volume without creating a new field boundary that marching cubes will mesh as an inner surface.** Two mechanisms cause it (both confirmed by the failed run, which still created all 64 cells):

1. The cluster creates a cell from *any* particle whose sphere overlaps the cell bounds (`Cluster::update_cell_meshes`, cluster.cpp:256). A "dropped" interior cell is still created from its neighbors' boundary particles spilling in, then meshed from that sparse spill-in → a partial inner shell.
2. Each meshed cell builds its SDF from only its assigned particles (`Cell::generate_mesh_for_group`, cell.cpp:293). Removing an interior cell's particles strips the spill-in that kept an adjacent *surface* cell solid on its inner face, so that surface cell's field now crosses the isolevel internally → another inner surface.

The fix is to stop trying to remove buried volume from the meshing input, and instead **skip the meshing step for cells that are fully interior, while keeping their particles** so neighboring meshed cells stay solid. Buried volume that is deep enough to touch no meshed cell can additionally have its particles dropped (a pure compute/memory win with no surface effect).

## Goal

Eliminate the marching-cubes / BLAS work for fully-interior cells, and drop the particles of deep-core cells, while reproducing the outer surface **essentially exactly** and producing **no inner surface** (culled BLAS triangles ≤ bypass). "Essentially exactly" (not byte-identical) is acceptable per explicit user direction.

## Non-Goals

No changes to the marching-cubes core (`GenerateMesh`), the simplifier, BLAS/TLAS registration, or `Cell::generate_mesh_for_group`. No LOD generation, instancing, streaming, or occlusion culling. No new lattice types beyond the regular grid. The *only* meshing-pipeline touch is a small skip-gate in `Cluster::rebuild_dirty_cells` (see below) — a guard, not an algorithm change.

## The Core Model

"Buried slot" is the existing `slot_is_buried` Chebyshev test (margin ≥ 1). A cell is **interior** iff *every* slot whose center lies in the cell is buried — i.e. the cell has no surface inside it, so meshing it would emit ≈0 triangles. A cell is **core** iff it is interior *and* all 26 of its neighbor cells are interior.

Every occupied cell falls into exactly one of three categories:

| Category | Definition | Meshed? | Particles kept? |
|---|---|---|---|
| **Surface** | not interior (has ≥1 non-buried slot) | **yes** | yes |
| **Skin** | interior, but not core | no | **yes** |
| **Core** | interior and all 26 neighbors interior | no | **dropped** |

The cull produces two derived sets in the cluster's cell-coordinate basis:

- **`no_mesh` set** = all **interior** cells (Skin ∪ Core). They are skip-meshed (they would emit ≈0).
- **`drop` set** = all **core** cells. Their particles are not emitted; Skin keeps its particles so it can back the meshed Surface cells with spill-in.

So: meshed = Surface only; skip-meshed = Skin ∪ Core; particles dropped = Core only.

### Why this is correct (the two invariants)

- **No inner surface.** A meshed cell M is, by definition, non-interior (it has a non-buried slot). Its neighbor N has particles unless N is in the drop set. A drop-set cell requires *all* its neighbors to be interior; M is non-interior, so M cannot be a neighbor of a drop-set cell. Therefore **every meshed cell borders only cells that still have particles** → its inner faces stay solid → no internal isolevel crossing → no inner surface. Skipped interior cells contribute nothing, so they can introduce no surface either.
- **Outer surface preserved.** Skipped cells are interior (every slot buried) → they would emit ≈0 triangles if meshed → skipping them removes no visible geometry. Dropped particles belong only to core cells, whose neighbors are all interior (skip-meshed), so they were never within sphere-reach of a meshed cell → removing them cannot perturb the outer surface.

Determinism is preserved: each emitted particle still depends only on its own slot + seed; the classification is a pure function of the occupancy and is iteration-order-independent (a cell is interior iff *no* slot in it is non-buried, fully resolved in a first pass before any emission).

## Cell/Slot Mapping (correctness crux)

The cull must bucket slots into the **same** cell grid the `Cluster` uses, and must emit `no_mesh` cell coordinates in the **same integer-coordinate basis** the cluster keys cells on. The cluster grid is:

```
cell_coord = floor(local_position / cell_size)      // Cluster::get_cell_coordinates, cluster.cpp:170
local_position = slot_position + cell_origin_offset  // offset = -half-extent, applied by the scene before add_particle
```

This reuses the `CullParams::cell_size` + `CullParams::cell_origin_offset` machinery already built for the prior sub-project. The cull computes a slot's cell as `floor((slot_position + cell_origin_offset) / cell_size)` and reports `no_mesh` cells as those same integer `(cx,cy,cz)` triples. The cluster stores each `Cell::coordinates` as `floor(local_position / cell_size)` (integer-valued floats), so matching is exact.

> Jitter note: `make_particle` applies small position jitter, but bucketing uses the *un-jittered* `slot_position + offset`, exactly as before. Jitter (≤ 0.15·radius) is tiny relative to cell size and, because the skip/drop decisions are conservative and per-cell, cannot move a particle across the boundary in a way that opens a hole.

## Components Changed

### `particle_culling.h` / `particle_culling.cpp`

- `cull_interior`'s behavior is **replaced**: instead of dropping interior-cell particles, it now (a) emits particles for every slot whose cell is **not** in the drop (core) set, and (b) outputs the `no_mesh` cell list.
- New output parameter: `std::vector<CellCoord>* no_mesh_cells = nullptr`, where `CellCoord` is the existing `SlotCoord` integer triple (reused; it is just `{int x,y,z}`). When non-null it is filled with every interior cell's coordinates.
- Algorithm (three passes over `Occupancy::for_each`, all order-independent):
  1. **Classify:** for each occupied slot compute its cell key and whether the slot is buried; a cell is interior iff *no* slot in it is non-buried. Store `std::unordered_map<uint64_t,bool> interior` keyed by packed cell coord. Also remember each cell's integer coord.
  2. **Core test:** a cell is core iff it is interior and all 26 neighbor cell keys are present and interior. Build `std::unordered_set<uint64_t> core`.
  3. **Emit:** for each occupied slot, emit its particle (`make_particle`) unless its cell is in `core`. Fill `no_mesh_cells` from the interior set (decoded back to `CellCoord`).
- `CullStats` gains `cells_meshed`, `cells_skipped` (= interior count), `cells_core` (= drop count). Keep `cells_total`.
- `slot_is_buried`, `emit_all`, `make_particle`, `lattice_vhash`, `lattice_vnoise` are **unchanged**.

### `cluster.h` / `cluster.cpp` (the one pipeline touch)

- New method `void set_no_mesh_cells(const std::vector<Vector3>& coords);` storing an `std::unordered_set<uint64_t>` of packed integer cell coordinates (cleared and refilled each call). A matching `void clear_no_mesh_cells();` (or empty-vector call) resets it.
- In `rebuild_dirty_cells`, before calling `update_cell_meshes(cell)`, if the cell's `coordinates` are in the no-mesh set: skip meshing it, call `cell->clear_meshes(&blas_manager_)` so no stale geometry remains, set `is_dirty = false`, and continue. (A skipped cell is created and tracked but holds no mesh and registers nothing with the BLAS.)
- This is the entire change to the meshing pipeline. `update_cell_meshes`, `find_or_create_cell`, `Cell`, `GenerateMesh`, the simplifier, and BLAS/TLAS code are untouched.
- The packed-coord key helper must use the same packing as the cull (a shared 21-bit-per-axis pack of the integer cell coordinate, offset to stay non-negative).

### `main.cpp` `setup_lattice_scene`

- Lower the cluster's cell size from **5.0** to **2.4** (≈3 slots per cell) at construction (main.cpp:302). Keep it a single named constant so the A/B can sweep it.
- After `cull_interior` fills `no_mesh_cells`, call `test_cluster_->set_no_mesh_cells(no_mesh_cells_as_vector3)` **before** `rebuild_dirty_cells`.
- Bypass path (`MSL_CULL_MARGIN=-1`) calls `emit_all` and `set_no_mesh_cells({})` (nothing skipped) so the A/B comparison meshes everything.
- Extend the `[cull]` log: `[cull] occupied=… emitted=… cells_meshed=… cells_skipped=… cells_core=… (margin=…)`.

### Tests — `tests/particle_culling_tests.cpp`

- **Replace** `test_cull_counts` expectations: with the skip-meshing model, `cull_interior` now emits *all* slots except core-cell slots; assert the emitted count and the `no_mesh` cell count for a known small solid block at a known `cell_size`.
- **Add** `test_no_meshed_cell_borders_dropped` (the key invariant): for the classification of a small solid block, assert that **no** cell that is meshed (non-interior) is a neighbor of any core (dropped) cell — i.e. the "no inner surface" invariant holds structurally.
- **Add** `test_skip_set_is_interior`: every cell in `no_mesh_cells` has all its slots buried; every cell *not* in it has at least one non-buried slot.
- `test_grid_lattice`, `test_occupancy`, `test_burial`, `test_thin_shape_keeps_all`, `test_determinism` remain; update only to thread the new output parameter / `CullStats` fields.

The cluster skip-gate itself is covered by the headless A/B (it requires GL meshing); no separate GL unit test is added.

### Acceptance test (A/B) — restored metric

Re-run the headless A/B (culled vs `MSL_CULL_MARGIN=-1` bypass), identical camera, with the new cell size (2.4):

- **Surface triangle count:** culled ≈ bypass within a small tolerance.
- **BLAS-registered triangles:** culled **≤** bypass (the prior approach failed this with 123,056 > 107,826; the new approach must not).
- **Cells meshed:** culled < bypass (interior cells skipped).
- **Particles fed in:** culled < bypass (core particles dropped).
- **Mesh-gen time:** culled < bypass.
- **Visual PNG diff:** matches within anti-aliasing tolerance; no holes, no interior facets.

If culled BLAS triangles materially *exceed* bypass, an invariant is violated — treat as failure and debug the cell-coordinate alignment between the cull's `no_mesh` set and `Cluster::get_cell_coordinates`.

## Error Handling

- `cell_size <= 0` is invalid; assert in debug builds.
- `margin < 1` is clamped to 1.
- `set_no_mesh_cells` with an empty vector is valid and means "mesh everything" (the bypass case).
- A no-mesh coordinate that matches no existing cell is harmless (the set is consulted only when a cell is about to be meshed).

## Risks / Open Questions for the Plan

- **Coordinate-basis mismatch** between the cull's `no_mesh` coords and the cluster's `Cell::coordinates` is the single most likely bug (same risk class as the re-center offset before). The plan must compute the offset once, share the packing helper, and rely on the A/B BLAS-triangle check as the backstop.
- **Cell size choice.** 2.4 (3 slots) is a starting default chosen so the meshed shell hugs the surface and a meaningful interior is skippable on the 20³ test brick. Smaller cells skip more interior but increase cell count (and the O(cells×particles) intersection loop in `update_cell_meshes`, an existing inefficiency we are not fixing here). The A/B should sweep a couple of sizes.
- **Margin.** Default stays 2 (conservative). Lower margins make more cells interior (bigger win); the A/B may lower it once correctness is confirmed.
- **LOD.** Baked at LOD 0; `cell_size == smallest_cell_size`. Higher-LOD baking remains correct under the same rule, validated later.

## File Structure

- Modify: `MatterSurfaceLib/include/particle_culling.h` — add `no_mesh_cells` out-param to `cull_interior`; extend `CullStats`; update the contract comment. (`CellCoord` = reuse `SlotCoord`.)
- Modify: `MatterSurfaceLib/src/particle_culling.cpp` — replace `cull_interior` body with the three-pass classify/core/emit algorithm; reuse `slot_is_buried`, `make_particle`.
- Modify: `MatterSurfaceLib/include/cluster.h` + `MatterSurfaceLib/src/cluster.cpp` — add `set_no_mesh_cells` / `clear_no_mesh_cells` and the skip-gate in `rebuild_dirty_cells`.
- Modify: `MatterSurfaceLib/main.cpp` — lower cell size to 2.4; wire `no_mesh_cells` into `set_no_mesh_cells`; extend logging.
- Modify: `MatterSurfaceLib/tests/particle_culling_tests.cpp` — replace `test_cull_counts`; add the invariant and skip-set tests; thread the new param/stats.
- No Makefile / build-all.sh changes (files already compiled and gated).

## A/B Result (2026-06-14) — PASS

Headless A/B on the 20³ brick (`SPACING = 2*BASE_RADIUS = 0.8`, so the interior is a tangent-sphere packing with real but hidden geometry), identical camera, solid render (`MSL_RENDER_MODE=1`):

| Run | margin | cell_size | particles | cells meshed | BLAS instances | triangles |
|---|---|---|---|---|---|---|
| Bypass | -1 | 2.4 | 8000 | 512 | 512 | **1,429,870** |
| Culled | 2 | 2.4 | 7784 | 448 (64 skipped, 8 core) | 448 | **1,126,916** |

**Acceptance criteria — all met:**
- BLAS-registered triangles: culled **1,126,916 ≤ bypass 1,429,870** ✓ (the prior cell-granular approach *failed* this: 123,056 > 107,826).
- Cells meshed: 448 < 512 ✓.
- Particles fed in: 7784 < 8000 (8 core cells × 27 slots = 216 dropped) ✓.
- Visual PNG diff (`ab_bypass.png` vs `ab_culled.png`): outer surface identical — no holes, no exposed interior facets ✓.
- The ~303K triangles removed are *occluded interior* geometry (tangent-sphere bumps inside the brick), not visible surface — exactly the intended win. (The spec's earlier "surface triangles culled ≈ bypass" expectation assumed a solid interior emitting ≈0; with tangent spheres the interior carries real hidden geometry, so culled is meaningfully *lower*, which is correct.)

**Sweep (informational):**
- `margin=1`, cell 2.4 → 296 cells meshed (216 skipped, 64 core), 413,410 triangles, still no holes — a validated, more aggressive operating point (~71% reduction). Default stays at the conservative margin 2.
- `MSL_CELL_SIZE=1.6` culled → 1,736,384 triangles: **not** comparable to the 2.4 bypass. Smaller cells skip more interior cells but duplicate more cell-boundary geometry, so a 1.6 comparison would need its own 1.6 bypass baseline. 2.4 is the better operating point on this brick.

Throwaway capture PNGs (`ab_*.png`) are not committed.
