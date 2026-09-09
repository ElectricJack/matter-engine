# Baked Vertex Ambient Occlusion — Design

**Date:** 2026-06-20
**Status:** Approved design, ready for implementation plan

## Goal

Replace the per-pixel ray-traced ambient occlusion in the raytracing fragment
shader with a per-vertex AO value precomputed on the CPU after meshing and read
back via barycentric interpolation. Removes the AO secondary rays entirely (a
GPU-traversal cost) while *improving* AO quality over today's sparse 2-rays /
25%-of-pixels approximation. Quality gain must come at near-zero compute cost.

## Motivation

The renderer is GPU-traversal-bound: a glFinish-bracketed profile shows the
raytrace pass is ~99.8% of a ~354ms frame. AO today
(`shaders/raytrace_tlas_blas.fs:261-289`) casts 2 short hemisphere rays on ~25%
of pixels — deliberately sparse to bound cost, and blocky as a result. Each ray
does a full TLAS→BLAS traversal. Baking AO into the geometry retires those rays
and upgrades the result to a smooth per-vertex term.

Two facts make this cheap and safe:

1. **Meshing is dirty-cell based, not per-frame** (`Cluster::rebuild_dirty_cells()`
   / `force_rebuild_all_cells()`). A static scene does zero remeshing per frame,
   so anything computed at mesh-build time is amortized across all intervening
   frames.
2. **The occupancy grid persists in world space.** `Occupancy scene_occ_`
   (`main.cpp:1977`) is built once at scene setup and kept alive. It is a sparse
   `SlotCoord → SlotData` grid with `occupied()` / `for_each()`, in the same
   local space the mesh vertices are emitted in. A post-meshing stage can read it
   directly with no rebuild.

## Architecture

A single isolated stage decoupled from the meshing algorithm:

```
... mesh all dirty cells (oriented cubes OR marching cubes) ...
        |
        v
  bake_vertex_ao(triangles, normals, occupancy, params)   <-- NEW
        |
        v
  blas_manager packs/uploads the soup (AO now in each triangle record)
```

- **New unit:** `include/vertex_ao.h` + `src/vertex_ao.cpp`.
- **Signature (conceptual):** a pure function taking the assembled per-BLAS
  triangle vertex positions and per-vertex normals plus a reference to the
  `Occupancy` grid and an `AoParams { float radius; float strength; }`, producing
  one AO value in `[0,1]` per triangle vertex.
- **Mesh-agnostic:** it consumes only vertex positions + normals + the occupancy
  field. It never inspects cube-corner topology, so it works identically for
  oriented cubes and marching cubes.

The stage is invoked after all cells in a rebuild have meshed and before the
`blas_manager` packs the triangle soup into GPU textures.

## AO Computation (no rays)

For each triangle vertex with position `p` and normal `n`, both expressed in the
occupancy grid's space (the cluster-local space the vertices are emitted in;
TLAS instance transforms place the BLAS into the world, so the bake operates
pre-transform):

1. Map `p` into slot space using the same lattice mapping the occupancy grid uses
   (so vertex positions and `SlotCoord`s align).
2. Scan the box of slots around `p` out to `ceil(R / spacing)` slots per axis,
   considering only those that are occupied and within Euclidean distance `R`.
3. For each occupied slot at offset `d = slot_center − p`, accumulate an
   occlusion weight that is the product of:
   - **Hemisphere alignment:** `max(0, dot(normalize(d), n))` — only slots in
     front of the surface occlude.
   - **Distance falloff:** a smooth decreasing function of `length(d)` over `R`
     (e.g. `1 − length(d)/R`, clamped to `[0,1]`).
4. `AO = clamp(1 − strength · Σweights, 0, 1)`.

Two tunable knobs: `radius` (R, how far occlusion reaches) and `strength` (how
dark it gets). The computation is deterministic and uses only sparse grid
lookups — a handful per vertex. A vertex buried in solid trends to AO ≈ 0; an
exposed corner stays ≈ 1; a wall-face vertex sits mid-range. Because adjacent
vertices see smoothly-varying neighbor sets, the result is smooth across faces
once interpolated.

## Storage — zero growth, uniform across mesh types

Per-vertex AO is **three** values per triangle (one per corner). Rather than
widen the triangle record, pack the three values at 8 bits each (24 bits total)
into the **single currently-unused triangle-record slot** (`row5.w` in the
existing 6-row record; see `blas_manager.cpp` triangle packing ~`434-485`).

Packing: `packed_bits = a0 | (a1 << 8) | (a2 << 16)` stored via the float's raw
bit pattern; unpacked in GLSL with `floatBitsToUint` + byte extraction, each
byte divided by 255.0.

Consequences:

- **Marching cubes** (smooth, needs all three normals N0/N1/N2) gains AO at *zero*
  size cost — it claims the otherwise-wasted slot.
- **Oriented cubes** also fit. The separately-planned flat-normal compression
  (collapsing N0=N1=N2 to one face normal for flat-shaded cube meshes) remains an
  independent cube-only size optimization, fully decoupled from AO.

This keeps AO uniform across both mesh paths and avoids growing any record.

## Shader Read Path

In `bvh_tlas_common.glsl`:

- `decodeTriangle` unpacks the three AO bytes from the slot into three `[0,1]`
  values alongside the existing triangle data.
- At shading, interpolate by the hit's barycentric weights:
  `ao = a0·w0 + a1·w1 + a2·w2` (the same interpolation already done for normals).
- `calculateAmbientOcclusion` in `shaders/raytrace_tlas_blas.fs` loses its ray
  loop entirely and becomes this lookup.
- The `aoEnabled` uniform is retained as the on/off toggle — it now gates whether
  the baked term is applied, keeping A/B comparison trivial.

The perf win lands here: the 2-rays-on-25%-of-pixels traversal cost goes to zero,
replaced by an interpolation already being performed for normals.

## Dirty-Rebuild Correctness

AO is recomputed inside the **same dirty-cell rebuild** that regenerates a cell's
triangles — it rides the existing `rebuild_dirty_cells()` path, no new trigger.

Subtlety: a vertex's AO depends on occupancy within radius `R`, so a dirtied cell
changes the AO of vertices in neighboring cells within `R`. Decision:

- **Start with: re-bake only the dirty cell's own vertices.** AO at cell seams
  lags by a frame during particle motion (faint seam shimmer in the motion-test
  mode; invisible in a static scene).
- **Follow-up only if needed:** dilate the dirty set to also re-bake cells within
  `R`. Treated as YAGNI until the shimmer is observed to matter.

This is a conscious simplification, not an oversight.

## Testing

`bake_vertex_ao` is a pure, GL-free function and unit-tests like
`tests/particle_culling_tests.cpp`:

- Vertex deep inside a solid block → AO ≈ 0 (dark).
- Isolated / fully-exposed vertex → AO ≈ 1 (bright).
- Vertex on a flat wall face → mid-range, and AO decreases monotonically as an
  occluding neighbor slot is added.
- Byte pack/unpack round-trips to within 1/255.

Plus a manual visual check in the running app (cannot be automated): confirm
baked AO looks at least as good as the previous ray-traced AO, and confirm the
frame-time drop from retiring the AO rays.

## Scope / Non-Goals

- Only ambient occlusion is baked. Shadows and indirect/GI lighting stay as-is
  (shadows move with the light; not a baking candidate here).
- The flat-normal compression and BLAS-node padding removal are *separate*
  lossless optimizations from the same investigation; this spec only assumes the
  AO slot-packing, not those changes.
- Dirty-set dilation for motion-correct seams is explicitly deferred.

## Affected Files

- **Create:** `include/vertex_ao.h`, `src/vertex_ao.cpp`,
  `tests/vertex_ao_tests/` (mirroring existing headless test dirs).
- **Modify:** the rebuild path that assembles the soup and calls `blas_manager`
  (invoke `bake_vertex_ao`); `blas_manager.cpp` triangle packing (write AO bytes
  into `row5.w`); `bvh_tlas_common.glsl` (`decodeTriangle` unpack + barycentric
  AO); `shaders/raytrace_tlas_blas.fs` (`calculateAmbientOcclusion` → lookup);
  the Makefile/test wiring for the new test target.
