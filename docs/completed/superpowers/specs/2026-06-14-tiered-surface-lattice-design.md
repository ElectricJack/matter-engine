# Tiered Surface Lattice + Detail-Driven Mesh Resolution — Design

**Date:** 2026-06-14
**Status:** Approved (design)
**Branch:** feat/lattice-tint

## Problem

The lattice brick reads as low-fidelity and repetitive ("looks like a wall"). Two
root causes:

1. Every occupied slot emits a single particle at one uniform size, so the
   outer surface is a regular periodic blob field.
2. Every meshed cell is sampled at a fixed 16³ marching-cubes grid
   (`cell.cpp:304`, `divisionPow = 4`), so even where finer detail exists it
   cannot be resolved.

We want the **highest-resolution model to look great and render decently fast** —
not a multi-LOD distance system. Detail should be concentrated where it is
visible (the surface) and the mesh grid should follow the detail actually
present in each cell.

## Goal

Give surface regions finer particles via a **tiered (subdividable) lattice**, and
make each cell choose its mesh resolution from **the finest detail present in
that cell**. Strip the hardcoded multi-LOD level system (keep mesh
simplification). Structure the tier concept so a future "draw shapes into a
lattice at a chosen resolution" API can author tiers instead of deriving them.

## Non-Goals (this round)

- The shape-drawing API (sphere/capsule/block/line-segment primitives,
  additive/subtractive draw, multi-lattice objects, bake-and-reuse). Captured in
  `docs/superpowers/backlog.md`.
- Making the interior visible (the brick stays opaque; interior cells remain
  skip-meshed / core particles dropped).
- New particle SHAPES (boxes, rotated cubes). Spheres only.
- Distance-based LOD. Removed, not replaced.

---

## Architecture

Detail flows in one direction:

```
depth-from-surface  ──►  tier (emit)  ──►  EmittedParticle.detail_size
                                              │
                                              ▼
                                        StaticParticle.detail_size
                                              │
                                              ▼
                                  cell picks divisionPow (mesh time)
```

- **Tier** is the load-bearing concept. Today it is *computed* from how deep a
  slot sits below the surface. Later, shape-drawing primitives will *author* it.
  Same field, different source — that is the forward seam.
- **`detail_size`** (nominal lattice spacing at a slot's tier, `S / 2ᵗ`,
  pre-variation) is the single new per-particle field. It means "how fine the
  matter in this region is" and is robust to per-particle size variation (raw
  radius is a noisy signal because size-variation perturbs it).

The base `Occupancy` is unchanged structurally and represents **tier 0**
(spacing `S`, radius `R`). All refinement lives in the emit path
(`particle_culling.cpp`); the mesher (`surface.c`) is unchanged; the cell only
gains logic to pick `divisionPow`.

---

## Components

### Phase 0 — Strip the LOD system (prerequisite)

The cluster operates at a single cell resolution. `simplification_ratio_` and
the simplifier stay (genuinely useful decimation lever for later).

**`include/cluster.h` / `src/cluster.cpp`:**
- Remove `current_lod_level_`, `set_lod_level`, `get_lod_level`,
  `get_current_cell_size`. Keep `smallest_cell_size_` as *the* cell size
  (`get_smallest_cell_size`/`set_smallest_cell_size` remain).
- `force_rebuild_all_cells` keeps its rebuild behavior but drops LOD-level
  references in logging.
- `rebuild_dirty_cells`: the interior skip-mesh becomes **unconditional** —
  delete the `current_lod_level_ == 0 &&` guard so `no_mesh_cells_` always
  applies. (Interior culling now always runs, which is desired.)
- Keep `simplification_ratio_`, `set_simplification_ratio`,
  `get_simplification_ratio`, `set_no_mesh_cells`, `clear_no_mesh_cells`.

**`main.cpp`:**
- Remove the ImGui "LOD Level" slider, number-key handlers 1–5, the `MSL_LOD`
  capture knob, the boot-time `set_lod_level(1)` and the `set_lod_level(0)`
  calls, and LOD fields in status prints.
- Keep the simplification slider, `MSL_RATIO`, and all other knobs.
- The cull's `no_mesh_cells` keying already uses the single cell size
  (`get_smallest_cell_size()`); no change needed there beyond removing LOD refs.

### Phase 1 — Tiered emit (`particle_culling.cpp` / `particle_culling.h`)

**Depth and tier.**
- `int slot_depth(const Occupancy& occ, SlotCoord c, int max_depth)`: the largest
  `k ≤ max_depth` such that every slot within Chebyshev radius `k` of `c` is
  occupied. `depth 0` means an immediate box-neighbor is empty (outermost shell).
  Reuses the same Chebyshev-box neighborhood as `slot_is_buried` (which is
  `slot_depth(...) >= margin`).
- `int slot_tier(int depth, int max_tier)`: `max_tier - min(depth, max_tier)`.
  Outermost shell → `max_tier` (finest); each layer inward drops one tier; at or
  below `max_tier` layers deep → tier 0.

**Sub-particle emission.** A tier-`t` slot emits a `2ᵗ × 2ᵗ × 2ᵗ` block of
sub-particles (tier 0 = the single particle of today):
- For each sub-offset `o ∈ {0 .. 2ᵗ-1}³`:
  - **Fine integer coord** `f = c·2ᵗ + o` (used for per-particle uniqueness via
    `lattice_vhash`).
  - **Fractional lattice coord** `cf = c + (o + 0.5) / 2ᵗ` (used for continuous
    fields — radius clusters and marble veins — via `lattice_vnoise` / `fbm3`, so
    those fields stay continuous across tiers and merely gain finer detail).
  - **Position** `= cf · S` then jitter (jitter magnitude scales with the tier's
    spacing `S/2ᵗ` so sub-particles jitter proportionally, not by the coarse
    amount).
  - **Base radius** `= base_radius / 2ᵗ`, then existing cluster+fine size
    variation applied on top.
  - **`detail_size` = `S / 2ᵗ`** (the tier's nominal spacing, pre-variation).
  - **materialId / tint** derived exactly as today, but evaluated at `f` (hash)
    and `cf` (noise) instead of the coarse `c`.

`make_particle` is refactored to take the fine coord `f`, fractional coord `cf`,
and `detail_size`, so tier-0 (single particle, `f = c`, `cf = c + 0.5`,
`detail_size = S`) reproduces today's output when `max_tier = 0`.

**Where it plugs in.** In `cull_interior` Pass 3 (and in `emit_all`), replace the
single `make_particle` per non-core slot with: compute `slot_tier`, then loop the
sub-offsets emitting one particle each.

**`CullParams` additions:**
```cpp
int   max_tier = 0;       // 0 = today's behavior (single particle per slot)
float spacing  = 0.0f;    // lattice spacing S (GridLattice::spacing())
```
(`spacing` is needed to compute `detail_size` and sub-particle positions; passed
from `main.cpp` via `GridLattice::spacing()`.)

**`EmittedParticle` addition:**
```cpp
float detail_size;   // nominal lattice spacing at this particle's tier (S / 2^tier)
```

### Phase 2 — Detail-driven mesh resolution

**`StaticParticle` (`cluster.h`) addition:**
```cpp
float detail_size = 0.0f;   // tier-0 spacing / 2^tier; 0 => fall back to tier 0
```
Threaded through both `add_particle` overloads (new overload or extend the
tinted one) and set from `EmittedParticle.detail_size` at scene build.

**`Cluster` (`cluster.h`) addition:** `float base_detail_size_` (the lattice
tier-0 spacing `S`) with `set_base_detail_size(float)`, set once from the scene.
Threaded into `rebuild_meshes` / `generate_mesh_for_group` alongside `max_pow`
so the cell can recover the finest tier from `detail_size`.

**Cell resolution selection (`cell.cpp::generate_mesh_for_group`).** Replace the
hardcoded `bounds.divisionPow = 4` with a value derived per cell from the
**finest tier present**, relative to the lattice's tier-0 spacing:
- The cluster knows the lattice's tier-0 spacing `S` (`base_detail_size_`, set
  once from the scene). It threads `base_detail` and `max_pow` into
  `rebuild_meshes` → `generate_mesh_for_group`.
- Find the **smallest `detail_size`** among the cell's particles for this group
  (ignore zeros / absent, which fall back to tier 0).
- Recover the finest tier: `tier = max(0, lround(log2(base_detail /
  detail_size_min)))`. (`detail_size = S/2ᵗ` ⇒ `log2(S / detail_size) = t`; the
  ratio form is robust to whatever absolute `S` / cell size the scene uses.)
- `divisionPow = clamp(kBasePow + tier, kBasePow, max_pow)`, where
  `kBasePow = 4` (today's 16³ floor) and `max_pow` comes from `MSL_MAX_POW`
  (default 6 = 64³ ceiling).
- A pure tier-0 cell lands at the base 16³; a cell touched by tier-1 surface
  particles auto-bumps to 32³; tier-2 → 64³. Resolution follows the lattice
  detail, automatically, regardless of absolute spacing.

The existing per-LOD feature taper (`kFeatureCullVoxels`, `kFeatureVisVoxels`,
`kBlendVoxels`) continues to work — it is expressed in voxels, and voxel now
derives from the chosen `divisionPow`, so it self-adjusts.

### Phase 3 — Knobs & scene wiring (`main.cpp`)

- New env knobs: `MSL_MAX_TIER` (default 1; 0 = pre-feature behavior),
  `MSL_MAX_POW` (mesh resolution ceiling, default 6).
- Removed knob: `MSL_LOD` (Phase 0). No `MSL_SURFACE_POW` — resolution is
  derived, not set.
- Set `p.max_tier`, `p.spacing = grid.spacing()` on `CullParams`; pass
  `EmittedParticle.detail_size` into `add_particle`.

---

## Data Flow (end to end)

1. `setup_lattice_scene` builds `Occupancy` (tier-0 block) + `GridLattice`.
2. `cull_interior` classifies cells (interior skip / core drop, unchanged) and,
   per surviving slot, computes `slot_tier` and emits `(2ᵗ)³` sub-particles with
   `detail_size = S/2ᵗ`.
3. Each `EmittedParticle` (incl. `detail_size`) → `Cluster::add_particle` →
   `StaticParticle.detail_size`.
4. `rebuild_dirty_cells` meshes every non-`no_mesh` cell (unconditionally).
5. `generate_mesh_for_group` recovers the finest tier from the cell's smallest
   `detail_size` (vs. `base_detail = S`) and sets `divisionPow = clamp(4 + tier,
   4, max_pow)`, then meshes as today.

---

## Testing (`tests/particle_culling_tests`)

Pure functions, no GL:

1. **`slot_depth`**: outer-shell slot → 0; one-layer-in → 1; deep interior →
   `max_depth`. Single isolated slot → 0.
2. **`slot_tier` mapping**: `depth 0 → max_tier`; `depth ≥ max_tier → 0`;
   monotonic in between.
3. **Sub-particle count**: a tier-`t` slot emits exactly `(2ᵗ)³` particles;
   tier 0 emits 1.
4. **`detail_size`**: tier-`t` particles carry `detail_size == S / 2ᵗ`.
5. **Determinism**: same `(Occupancy, seed, max_tier)` ⇒ byte-identical emit
   (positions, radii, tints).
6. **Core still dropped**: a fully-buried core cell emits zero particles even
   with `max_tier > 0`.
7. **Regression guard**: `max_tier = 0` reproduces the pre-feature emit exactly
   (count, positions, radii, tints) vs. the current `cull_interior`.

Resolution selection (`tests/cell_bounds_tests` style, GL-free):

8. **divisionPow derivation** (given `base_detail = S`): `detail_size = S`
   (tier 0) → pow 4; `detail_size = S/2` → pow 5; `detail_size = S/4` → pow 6;
   `detail_size = S/8` clamps at `max_pow = 6`; zero/absent `detail_size` →
   base pow 4.

Acceptance (manual, renderer): with `MSL_MAX_TIER=1`, surface visibly finer and
less periodic; cull stats still drop core particles; capture renders in
comparable time to today (decent, not multi-minute regression).

---

## Risks / Mitigations

- **Particle-count blow-up**: tier 1 multiplies *surface-shell* slots by 8.
  Mitigated: only the outermost `max_tier` layers refine; interior stays tier 0;
  core still dropped. `max_tier` default 1 keeps it tame; knob to push to 2.
- **Mesh-time blow-up**: higher `divisionPow` on refined cells. Mitigated:
  derived per-cell (only refined cells pay), clamped by `MSL_MAX_POW`.
- **Seam between fine surface cells and coarse skin backing**: both feed the same
  per-cell shared SDF metaball field with smooth-min union (mixed radii already
  supported today), so the field still closes watertight.
- **Determinism drift**: fine coords (`f`) and fractional coords (`cf`) are pure
  functions of `(SlotCoord, sub-offset)`, preserving the "same design bakes
  identically" guarantee.

---

## Future Work

See `docs/superpowers/backlog.md` — "Draw-into-lattice authoring." The
`detail_size` / tier field defined here is the integration point: drawing
primitives set it directly rather than deriving it from depth.
