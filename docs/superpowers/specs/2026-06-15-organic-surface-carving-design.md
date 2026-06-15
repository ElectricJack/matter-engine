# Organic Surface Carving — Design

**Date:** 2026-06-15
**Status:** Approved (design phase)
**Project:** MatterSurfaceLib

## Summary

Deliberately recreate the organic character that a meshing bug accidentally
produced — surface divots/pits, cracks/crevices, coarse lumpiness, and genuine
concave (inward-curving) surfaces — now that the isosurface is clean and
watertight. The look reference is a cinderblock: medium-scale, particle-sized
features, a handful per cell, rough but solid.

This is achieved with two independent mechanisms:

1. **Subtractive carve particles** — a separate particle array, smooth-CSG
   subtracted from the additive union, producing divots, crevices, and true
   concavity. This is the only mechanism that yields inward curvature.
2. **Low-frequency radius modulation** — coherent noise scaling the additive
   particle radii, producing coarse lumpiness. Pure input-side; no mesher change.

## Goals

- Reproducible, tunable organic surface: divots (round pits), crevices (linear
  cuts), coarse lumps, and concave surfaces.
- Watertight by construction (no accidental through-holes).
- No regression to the additive surface quality recently restored by the
  nearest-N query fix (`sh_query_radius_nearest`).
- All effects driven by the existing noise toolkit (`lattice_vhash`,
  `lattice_vnoise`, `fbm3`) and exposed as env knobs like the rest of the scene.

## Non-Goals

- Through-holes / caves / sponge topology (effect 3 was explicitly not wanted).
- Per-feature artistic placement / authoring tools — procedural only.
- A fine high-frequency erosion layer riding on the carves ("Approach B"). Noted
  as a future follow-up; intentionally deferred.

## Background — current pipeline (as of this design)

- **Additive field.** `CalculateScalarAndMaterial` (`src/surface.c`) queries the
  nearest additive particles via `sh_query_radius_nearest` (buffer 64) and
  combines them with an exponential smooth-min (log-sum-exp) union:
  `f = fmin - k·ln(Σ exp(-(f_i - fmin)/k))`, `k = blendWidth`. Material id comes
  from the hard-min particle. A hard-union early-out (`blendWidth <= 1e-5` or a
  single particle) returns exact `fmin`.
- **Existing subtraction primitive.** `ApplyClipField` (`src/surface.c:856`)
  already performs hard CSG subtraction against a separate `clipParticles` array:
  `scalarValue = max(scalarValue, -fO)`, keeping the group's own material. It is
  threaded through `GenerateMesh` / `GenerateMeshWithConfig` /
  `ComputeSurfaceNormals` and gathered per-cell by `build_clip_particles`
  (`src/cell.cpp:273`). The normals path (`src/surface.c:303-337`) mirrors the
  scalar clip so divot/clip walls shade correctly. Carve mirrors this proven
  pattern.
- **Per-cell meshing & continuity.** Each cell builds its own spatial hash and
  marching-cubes grid. Cross-cell continuity (watertightness) holds because
  `Cluster::update_cell_meshes` (`src/cluster.cpp:295-301`) assigns every
  particle to **all cells its sphere overlaps** (`intersects_sphere`), so a
  boundary particle is present in both neighbouring cells' fields; distant
  particles' smooth-min contributions decay exponentially and are negligible.
- **Sign convention.** `scalarValue` is signed distance; NEGATIVE = inside,
  POSITIVE = outside. Marching cubes contours at isovalue 0. Both clip and carve
  only ever *raise* `scalarValue` (push the surface inward), so they compose
  cleanly.

## Design

### 1. Field math — smooth subtraction

For each carve particle `j` with signed distance `s_j = |p − c_j| − r_j`, the
carved field is the smooth-max (smooth CSG subtraction) of the additive union
`f_add` against the negated carve distances:

```
f_carved = k_c · ln( e^(f_add / k_c) + Σ_j e^(−s_j / k_c) )
```

- Far from carves: `s_j` large positive → `e^(−s_j/k_c) ≈ 0` → `f_carved ≈ f_add`
  (surface untouched).
- Near a carve centre: `−s_j` dominates → `f_carved` rises (toward outside) → the
  surface retreats inward, leaving a smooth concave bite.
- `k_c` (`MSL_CARVE_BLEND`) is the carve fillet radius; `k_c → 0` recovers a hard
  `max` (razor-edged crater).

`materialId` is **not** modified by carve — a divot exposes the same material
(cinderblock = one material, textured).

**Numerical stability.** Factor out the max exponent
`m = max(f_add, max_j(−s_j))`:

```
f_carved = m + k_c · ln( e^((f_add − m)/k_c) + Σ_j e^((−s_j − m)/k_c) )
```

so the largest exponent is 0 — no `expf` overflow, matching the additive union's
`fmin` subtraction trick.

**Hard-union early-out path** (`blendWidth <= 1e-5` or single particle): carve
becomes the plain `max(f_add, max_j(−s_j))`, applied before `ApplyClipField` in
that branch. With `carveCount == 0` it is a no-op, keeping the
unclipped+uncarved path byte-identical.

**Ordering:** `f_add` → carve (`ApplySubtractField`) → clip (`ApplyClipField`).
Both only raise `scalarValue`, so order is not load-bearing; carve-then-clip is
the logical reading.

**Gradient (normals).** `ComputeSurfaceNormals` recomputes the field gradient;
it must match the carved surface or divots shade wrong. Differentiating the
smooth-max:

```
∇f_carved = [ e^(f_add/k_c)·∇f_add + Σ_j e^(−s_j/k_c)·(−û_j) ]
            / [ e^(f_add/k_c) + Σ_j e^(−s_j/k_c) ]
```

where `û_j = (p − c_j)/|p − c_j|`. This is the existing softmax-weighted blend of
additive directions (`∇f_add`), extended to also blend in the **inward**
directions `−û_j` of nearby carve particles, weighted by `e^(−s_j/k_c)`. Same
weighting structure already used for the additive gradient and the clip override.

### 2. Carve particle generation (divots + crevices)

`generate_carve_particles(...)` in `src/particle_culling.cpp` seeds from the
**surface sub-particles** emitted for depth-0 slots (interior seeds would be
invisible no-ops). Carve centre = the sub-particle position (no offset in v1;
depth is governed by carve radius — YAGNI on offset).

Per surface sub-particle at world position `pos`:

```
blob  = fbm3(pos * freq)
ridge = 1 - fabs(2*fbm3(pos*freq + OFFS) - 1)   // ridged: thin connected lines
n     = lerp(blob, ridge, ridginess)            // 0 = round divots, 1 = crevices

threshold = 1 - amt                             // amt = 0  =>  threshold 1 => OFF
if (n > threshold) {
    float over = (n - threshold) / (1 - threshold);   // (0,1]
    float r    = clamp(base_r * (0.5 + over), 0, r_max);
    append Particle{ pos, r, materialId=0 };           // materialId unused by carve
}
```

- `amt` = `MSL_CARVE_AMT` (density/strength; higher → lower threshold → more,
  deeper carves; 0 turns carve fully off).
- `freq` = `MSL_CARVE_FREQ` (feature spacing; tuned for a handful per cell at
  cell ≈ 2.4).
- `base_r` = `MSL_CARVE_RADIUS` (base divot size).
- `ridginess` = `MSL_CARVE_RIDGE` (0 → round divots, 1 → linear crevices).
- `k_c` = `MSL_CARVE_BLEND` (fillet softness).
- `r_max` = watertight cap (see Watertightness below).

### 3. Lumpiness — additive radius modulation

In `make_sub_particle` (`src/particle_culling.cpp`), scale the additive radius by
a **low-frequency** coherent noise (distinct from the existing high-frequency
white-noise `RADIUS_VAR`):

```
radius *= 1 + MSL_LUMP_AMT * (2*lattice_vnoise(pos * MSL_LUMP_FREQ) - 1)
```

Low `MSL_LUMP_FREQ` (≈ 1 cycle per cell or less) makes the shell swell and pinch
over large regions → irregular lumpy silhouette. No mesher change; the additive
union already consumes per-particle radius. `MSL_LUMP_AMT = 0` disables it.

### 4. Plumbing & data flow

```
setup_lattice_scene (main.cpp)
  └─ generate_carve_particles(...)  -> std::vector<Particle>
       └─ Cluster::set_carve_particles(list)            // stored as carve_particles_

Cluster::update_cell_meshes (per cell)
  ├─ assign additive particles via intersects_sphere(radius)        [existing]
  ├─ assign carve particles overlapping the cell via
  │    intersects_sphere(carve_radius + k_c * K_REACH)              [new — same halo rule]
  └─ cell->rebuild_meshes(..., cellCarvePtr, cellCarveCount)

Cell::rebuild_meshes -> generate_mesh_for_group
  ├─ GenerateMesh(..., clipPtr, clipCount, carvePtr, carveCount)
  └─ ComputeSurfaceNormals(..., clipPtr, clipCount, carvePtr, carveCount)
```

**Signature changes (carve params appended after the clip params):**

- `surface.h` / `surface.c`:
  - `GenerateMesh(..., Particle* clipParticles, int clipCount, Particle* carveParticles, int carveCount)`
  - `GenerateMeshWithConfig(..., clipParticles, clipCount, carveParticles, carveCount)`
  - `ComputeSurfaceNormals(..., clipParticles, clipCount, carveParticles, carveCount)`
  - internal: `GenerateMeshInternal(...)`, `CalculateScalarAndMaterial(..., carveParticles, carveCount)`
  - new helper: `static inline void ApplySubtractField(ScalarMaterialPair* result, Vector3 position, Particle* carveParticles, int carveCount, float k_c)`
- `cell.h` / `cell.cpp`: `rebuild_meshes` and `generate_mesh_for_group` gain a
  per-cell carve array + count; passed through to the two surface calls.
- `cluster.*`: `std::vector<Particle> carve_particles_`, a
  `set_carve_particles(std::vector<Particle>)` setter, and per-cell carve
  gathering in `update_cell_meshes`.
- `particle_culling.h`/`.cpp`: declare/implement `generate_carve_particles`; edit
  `make_sub_particle` for lumpiness.
- Tests: update prototype mirrors in `tests/mesh_continuity_tests.cpp` and
  `tests/cell_bounds_tests.cpp` to the new signatures (pass `NULL, 0` for carve
  where unused, keeping existing assertions byte-identical).

### 5. Knobs (env vars)

| Var | Meaning | Default |
|---|---|---|
| `MSL_CARVE_AMT` | carve density/strength; 0 = off | tasteful nonzero (TBD in plan, ~0.35) |
| `MSL_CARVE_FREQ` | carve noise frequency (feature spacing) | tuned for handful/cell |
| `MSL_CARVE_RADIUS` | base divot radius | ~0.5 × tier-1 particle radius |
| `MSL_CARVE_RIDGE` | 0 = round divots, 1 = linear crevices | ~0.4 |
| `MSL_CARVE_BLEND` | carve fillet softness `k_c` | ~0.5 × blend_width |
| `MSL_LUMP_AMT` | low-freq radius modulation; 0 = off | tasteful nonzero |
| `MSL_LUMP_FREQ` | lumpiness noise frequency (low) | ≈ 1 cycle/cell |

Defaults ship nonzero so the organic character is visible out of the box (the
explicit ask), all zeroable/overridable via env. Exact default values finalized
during implementation by visual tuning.

## Watertightness & cross-cell continuity

- **No through-holes:** `r_max` caps the carve radius below the local additive
  shell thickness, so subtraction only dishes the surface, never punches through.
  Shell thickness is approximated from the additive particle radius / blend at
  the seeding resolution; the cap is conservative.
- **No seam cracks:** carve particles are assigned to **every cell whose region
  their influence (`carve_radius + k_c·K_REACH`) overlaps**, mirroring the
  additive `intersects_sphere` halo. Any cell that meshes a region touched by a
  carve includes that carve, so shared-face field values match. `K_REACH` (a few
  multiples of `k_c`) covers the exponential carve falloff, analogous to how
  additive continuity tolerates exponentially-negligible distant contributions.
- **Normals match the scalar surface:** the gradient override fires on exactly
  the locus the scalar carve moves (mirroring the clip normals approach), so no
  shading seam between carved and uncarved regions.

## Testing plan

Headless suites (run via `WSL_LINUX=1` build + `build-all.sh test`):

1. **Carve math unit** — a single additive particle (sphere) plus one carve
   particle straddling its surface; assert the field at the carve centre is
   raised (surface retreated) and far from it is unchanged; assert
   `carveCount == 0` reproduces the additive-only field byte-for-byte.
2. **Watertightness** — extend `mesh_continuity_tests`: with carve enabled at
   default strength, the meshed surface remains closed (no boundary edges /
   manifold check) and no through-holes appear (cap respected).
3. **Cross-cell continuity** — two adjacent cells sharing carve particles produce
   matching field values on the shared face (no crack), analogous to existing
   continuity checks.
4. **Normals consistency** — recomputed normals on a carved divot point inward
   on the divot wall (gradient sign), not flat with the surrounding surface.
5. **Regression** — existing `cell_bounds_tests` (clip) and additive surface
   tests still pass unchanged with carve disabled.

Visual acceptance: render the lattice scene (solid-shaded `MSL_RENDER_MODE=4` and
ray-traced) and confirm cinderblock character — round divots, linear crevices via
`MSL_CARVE_RIDGE`, coarse lumps via `MSL_LUMP_AMT`, and concave surfaces.

## Risks / future

- **Carve count vs per-vertex cost:** carve is applied by linear iteration per
  grid vertex (like clip). Per-cell filtering keeps `carveCount` small; if dense
  scenes get slow, fall back to a per-cell mini spatial-hash for carves.
- **Tuning surface area:** seven knobs. Defaults must be tuned once, visually, so
  the out-of-box look is good without env overrides.
- **Future Approach B:** a light high-frequency input-side radius-erosion /
  dropout layer for fine crack detail on top of the carves. Deferred; the carve
  array + knobs leave room to add it without re-plumbing.
