# Voxel Box Imposter — Design

**Status:** Approved for planning (2026-06-22)
**Supersedes:** the chart-based fitted-cage imposter approach
(`2026-06-21-fitted-cage-imposter-design.md`, `2026-06-22-chart-based-cage-uv-design.md`).

## Goal

Replace heavy part geometry with a cheap, instanceable distance-LOD proxy that is a
**box cage enclosing a dense voxel volume**. A ray that hits the box marches the volume
(3D-DDA) and returns the first covered voxel's albedo, normal, and depth, which the
existing lighting path shades. This handles **non-convex and porous parts (trees,
shrubs)** correctly — concavities, overhangs, see-through canopy gaps, and accurate
silhouette from every angle — which the chart-based relief cage could not.

## Architecture (one paragraph)

Each part *type* is baked once into a dense voxel grid (per-axis resolution `Nx×Ny×Nz`
sized to the part's local-space AABB so voxels stay ~isotropic in world space). The grid
stores, per voxel: 8-bit coverage, RGB albedo, and an octahedral-packed normal. At
runtime the proxy is a **unit-cube BLAS** (12 triangles); each instance is a TLAS entry
whose transform stretches/orients/positions that unit cube to the part's world AABB. A ray
hits the box, transforms into normalized `[0,1]³` box space, runs a 3D-DDA voxel walk from
entry to exit, and on the first covered voxel returns a standard `HitResult` (world
position, world normal, albedo) so the engine's normal lighting/shadow/GI path shades it
like any other surface. The voxel volume is shared by all instances of a type; only the
transform (and an optional per-instance tint/seed) varies.

## Tech Stack

C++17, raylib/rlgl + OpenGL 3.3, GLSL fragment-shader ray tracer (TLAS/BLAS in
`bvh_tlas_common.glsl`). GL-free CPU bake (unit-tested with the existing test harness under
`MatterSurfaceLib/tests/`).

---

## Goals / Non-goals

**v1 goals**
- Dense voxel volume per part type; arbitrary (non-cube) AABB via per-axis grid dims.
- Per-voxel coverage + albedo + octahedral normal.
- Unit-cube BLAS proxy; per-instance transform carries box scale/orientation/position.
- 3D-DDA volume march in the traversal shader; standard `HitResult` out → existing lighting.
- Correct rendering of a non-convex test part (porosity + all-angle silhouette).
- Serialize/deserialize the baked volume; content-hash cache like the current `.imp`.
- **Salvage**: extract the reusable mesh-charting / UV-atlas code into a standalone library
  before deleting the chart-cage imposter path.

**Explicit non-goals for v1 (deferred to later phases, see Phasing)**
- Sparsity (brick grid / empty-space skipping).
- Block compression (BCn) of the volume.
- Oriented bounding box (PCA-fit OBB); v1 uses the local-frame AABB.
- Pre-baked radiance/AO in the volume; v1 stores albedo and lights at runtime.
- Shared single box-BLAS across *all* types with a per-instance type id selecting a volume
  atlas slice (natural follow-on; v1 may use one BLAS + per-type texture binding).

---

## Components

Each component is GL-free and unit-testable unless it explicitly needs a GL context.

### 1. MeshChartingLib (salvage / extraction)

The chart-based cage produced genuinely reusable mesh-processing code that is independent of
imposters. Extract it into its own sibling library (per the monorepo convention in
`CLAUDE.md`: a top-level project dir with `include/` + `src/`, consumed via
`-I../MeshChartingLib/include`). This must happen **before** the chart-cage imposter code is
deleted, so the functions and their tests move intact.

Functions to extract (currently in `MatterSurfaceLib/include/imposter_asset.h` +
`src/imposter_asset.cpp`):
- `build_adjacency` — position-welded triangle edge adjacency.
- `segment_charts` — normal-cone region-grow chart segmentation.
- `plane_basis` — robust orthonormal basis for a plane normal.
- `pack_charts` — shelf bin-packer for chart rects into an atlas.

Their tests (in `tests/imposter_asset_tests.cpp`) move with them into a new
`tests` target for the library. `mesh_simplifier` already lives separately and is left where
it is (the library may depend on it via include path).

Imposter-specific packers (`pack_cage_uvs_bvh_order`, `pack_cage_tri_data`) are **not**
extracted — they are deleted with the chart-cage path (Component 7).

### 2. Voxel imposter asset + bake (CPU, GL-free)

New asset type (new header/source, e.g. `voxel_imposter.h` / `voxel_imposter.cpp`):

```cpp
struct VoxGenParams {
    int   maxDim;      // resolution budget for the longest axis (e.g. 128)
    int   seed;        // reserved
    float coverThresh; // surface-voxelization fill threshold in [0,1] (default 0.5)
};

struct VoxelImposter {
    float    bounds_min[3], bounds_max[3]; // part local-space AABB
    int      nx, ny, nz;                   // per-axis grid dims
    uint64_t source_part_hash;
    std::vector<uint8_t> coverage;         // nx*ny*nz, 0=empty 255=full
    std::vector<uint8_t> albedo;           // nx*ny*nz*3, RGB
    std::vector<uint8_t> normal;           // nx*ny*nz*2, octahedral RG8
};
```

Bake pipeline (all CPU, testable):
1. Flatten part triangles (reuse existing `flatten_part_triangles(blas, tlas)`).
2. Compute the part local-space AABB.
3. **Choose grid dims**: world voxel size `v = maxExtent / maxDim`;
   `nx = clamp(ceil(extentX / v), 1, maxDim)`, similarly `ny, nz`. Keeps voxels ~isotropic.
4. **Surface-voxelize**: for each triangle, mark voxels it overlaps (triangle/voxel-box
   overlap test). `coverage = 255` for marked voxels (fractional coverage is a future
   refinement).
5. Per covered voxel, accumulate area-weighted **albedo** (from the overlapping triangles'
   material albedo) and area-weighted **normal**; normalize; octahedral-pack the normal.

### 3. Serialization

New file format + extension (e.g. `.vxi`), mirroring the existing atomic temp+rename, magic,
version, content-hash, and `source_part_hash` guards from `imposter_asset::save/load`.
Stores header (bounds, dims, hashes) + the three voxel byte arrays. GL-free.
`compute_vox_hash(params) `→ cache path, identical caching strategy to today.

### 4. Runtime upload + cage/instance (main.cpp)

- Register a **unit-cube BLAS** (12 triangles, corners at `[0,1]³`) once.
- For each imposter instance, add a TLAS entry pointing at that BLAS with the transform
  `worldPlacement ∘ (AABB→box)` (translate+scale, plus any instance rotation), flagged
  `isImposter`.
- Upload three GL **3D textures** per part type:
  `imposterColorVolume` (RGBA8: RGB albedo, A coverage),
  `imposterNormalVolume` (RG8 octahedral).
  (Coverage is packed into color alpha to save a sampler; it stays uncompressed and is the
  hit authority.)
- Uniforms: grid dims come from `textureSize()`; pass the box AABB / normalization so the
  shader maps local hit → `[0,1]³`.

### 5. Shader voxel march (bvh_tlas_common.glsl)

Replace the relief-march block in `intersectScene`'s imposter branch with a **3D-DDA volume
march** (Amanatides-Woo):
1. From the box hit, compute entry/exit parameters of the ray within the box in normalized
   `[0,1]³` space (transform ray with `inst.invTransform`).
2. Initialize DDA at the entry voxel; step voxel-by-voxel toward exit.
3. At each voxel sample `imposterColorVolume.a` (coverage); if `> 0.5`, it's a hit.
4. On hit: read albedo (`.rgb`), decode the octahedral normal, compute the world-space hit
   position (t along the world ray), transform the normal to world via the inverse-transpose
   of the box→world linear part. Populate a standard `HitResult`.
5. No covered voxel before exit → coverage miss (`result.hit = false`): the ray passes
   through (this is what gives trees their porosity for both primary and shadow rays).

### 6. Lighting integration

The voxel hit populates the **standard** `HitResult` (position, world normal, albedo). It
flows through the engine's existing lighting/shadow/GI path exactly like a triangle hit — no
`bakedColor` bypass. This means imposters light dynamically and consistently with real
geometry. (Pre-baked radiance/AO is a future memory/perf optimization, not a v1 need.)

### 7. Cleanup / removal

After Components 1–6 land and the non-convex visual check passes, delete the chart-cage
imposter path that is now dead and not salvaged:
- `build_cage`, `bake_displacement_cpu`, `dilate_atlas`, `pack_cage_uvs_bvh_order`,
  `pack_cage_tri_data`, `cage_to_tris`, the chart fields on `ImposterAsset`, and
  `src/imposter_bake.cpp` (GPU radiance bake for the relief atlas).
- The relief-march shader code (`reliefMarch`, `projectInTri`, `neighborForExit`,
  `fetchCageTri`, the 2D imposter atlas samplers/uniforms).
- Their tests, after the salvaged subset has moved to MeshChartingLib.

---

## Coordinate spaces & math

- **Local box space** = part AABB normalized to `[0,1]³`. The volume is indexed here, so one
  bake serves every transformed instance.
- **Unit-cube BLAS** corners at `[0,1]³`; the instance transform `T` maps it to the world
  OBB. Arbitrary box dimensions = non-uniform scale in `T`. Instancing is preserved: N
  instances = N tiny boxes in the BVH + 1 shared volume.
- **Ray → local**: `transformRay(ray, inst.invTransform)`; DDA runs in local space where the
  grid is a uniform `nx×ny×nz` lattice (anisotropic world voxels handled by the DDA's
  per-axis tDelta).
- **Normal → world**: octahedral-decode in local space, transform by the inverse-transpose
  of `T`'s linear part (existing `transformNormal`).

---

## Memory & performance

**Memory (dense v1, color RGBA8 + normal RG8 = 6 bytes/voxel):**

| Grid (longest axis 128) | Voxels | Bytes | Notes |
|---|---|---|---|
| 128³ (cube part) | 2.1 M | ~12 MB | worst case, cube-shaped part |
| 128×128×32 (flat brick) | 524 K | ~3 MB | per-axis dims pay only for real extent |
| 32×32×128 (tall tree) | 131 K | ~0.8 MB | per-axis dims pay only for real extent |

Per-axis resolution already removes the cube's wasted-air tax along short axes. The deeper
savings (sparsity + BC compression, ~5–10× and ~4× respectively) are deferred; see Phasing.

**Performance:** ray-box is a 12-tri leaf. The DDA visits up to ~`maxDim·√3` voxels worst
case (dense), but neighboring rays hit the same shared volume → strong texture-cache reuse,
and the per-tree deep BLAS (many alpha-tested leaf tris) is gone entirely. Empty-space
skipping (Phase 2 sparsity) will cut the average step count for porous parts.

**Instancing:** one shared volume per type + one unit-cube BLAS; per-instance cost is just a
transform. Endpoint (Phase 4): a single box BLAS shared across *all* types, with a
per-instance type id selecting a volume-atlas slice — collapsing the TLAS to many instances
of one minimal BLAS.

---

## Error handling / edge cases

- Empty/degenerate part (no triangles, zero-extent axis) → bake returns false; caller skips
  the imposter (renders real geometry).
- Coverage miss along the whole ray → `result.hit = false` (pass-through), not a black fill.
- Cache/format/version/`source_part_hash` mismatch on load → regenerate (same policy as the
  current `.imp` loader).
- Anisotropic/rotated instance transforms → handled by the local-space DDA + inverse-
  transpose normal; no special path.

---

## Testing strategy

**GL-free unit tests (new `voxel_imposter_tests`, plus migrated charting tests):**
- Grid-dim selection: known AABB + `maxDim` → expected `nx,ny,nz` (isotropy + caps).
- Voxelization: a single known triangle → exact set of covered voxels; a thin quad →
  surface (not solid) fill.
- Albedo/normal accumulation: two triangles in one voxel → area-weighted result.
- Octahedral encode/decode round-trip within tolerance over a normal sphere sample.
- Serialization round-trip; corruption/version/hash-mismatch → load returns false.
- 3D-DDA traversal (host port or extracted helper): ray → expected voxel visitation order
  and first-hit voxel for a planted coverage pattern.
- MeshChartingLib: existing `build_adjacency` / `segment_charts` / `plane_basis` /
  `pack_charts` tests pass unchanged after extraction.

**Visual validation (headless captures, `MSL_CAPTURE`):**
- Convex part (existing brick/sphere cluster): imposter vs real mesh side-by-side — silhouette
  and shading match at imposter range.
- **Non-convex test part** (a forked/porous shape standing in for a shrub): verify canopy
  gaps show background (porosity), silhouette correct from multiple `MSL_CAM` angles, no
  fragmentation.

---

## Phasing

1. **v1 — dense voxel box imposter** (this spec): salvage library → voxel asset+bake →
   serialization → runtime upload + unit-cube BLAS → 3D-DDA shader → lighting integration →
   non-convex visual check → remove chart-cage path.
2. **Sparsity**: two-level brick grid (coarse index + occupied-brick atlas) + empty-space
   skipping in the DDA. Biggest vegetation win; also speeds the march.
3. **Compression**: BCn on the color/normal brick atlas (capability-checked; occupancy/
   coverage stays uncompressed to avoid holes).
4. **Instancing endpoint**: single shared box BLAS across all types + per-instance type id →
   volume-atlas slice.
5. **Refinements as needed**: PCA-fit OBB for diagonal parts; pre-baked radiance/AO;
   fractional coverage for anti-aliased silhouettes.

---

## Open questions (non-blocking for v1)

- `maxDim` default (start 128; revisit after the non-convex visual check).
- Whether v1 needs fractional coverage for silhouette AA or binary coverage suffices at
  imposter range (lean: binary for v1).
