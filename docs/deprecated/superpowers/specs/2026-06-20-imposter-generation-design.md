# Imposter Generation (cage + displacement proxy) — Design

**Date:** 2026-06-20
**Status:** Approved, ready for implementation planning
**Project:** MatterSurfaceLib

## Problem

Parts are individually heavy: a single procedurally generated part can carry
enough triangles and BVH nodes that primary, shadow, and GI traversal over it
dominates per-part cost and memory. We want to **replace a part's full triangle
BVH with a lighter stand-in that still looks convincing from any angle, including
up close** — purely to cut per-part scene complexity (not classic distance LOD
billboarding, which falls apart up close).

The bottleneck we are relieving is **per-part geometry weight** (triangle/BVH
traversal + memory per part), not instance count or build time.

The stand-in is an **imposter**: a coarse cage mesh wrapped in a baked-lit color
texture and a scalar displacement texture, intersected by the existing ray tracer
in place of the part's triangle BVH, as a **full participant** in the frame
(camera, sun-shadow, and GI rays all hit it). It is a lighter *alternate*
representation of a part; the real `.part` BLAS remains the up-close fidelity
fallback.

## Scope

This spec covers **two deliverables**:

1. **Bake (#1):** load a `.part`, generate the cage + UVs, ray-cast the real
   geometry on the GPU to fill the displacement and baked-lit color textures, and
   serialize the result as an imposter asset (`.imp`, sibling to `.part`).
2. **Render (#2):** extend the traversal shader to relief-march imposter instances
   and shade them from baked color, so a baked imposter can be placed next to a
   real ray-traced part in the same frame and compared.

Render (#2) is in scope because it is the only honest test of the bake (#1) — you
cannot trust a bake you cannot see.

## Non-Goals (deferred follow-up specs)

- **LOD selection / real-BLAS swap (#3).** Per-instance choosing imposter vs real
  `.part` BLAS by distance/screen-coverage, and loading the real geometry on
  demand when parallax/displacement no longer holds up. This spec designs the
  asset format to carry what #3 needs (bounds, max displacement, recommended
  parallax-valid radius, source `.part` hash) but implements no selection logic.
- **Receiving dynamic lighting on imposters.** Baked radiance is frozen; an
  imposter casts shadows/GI onto neighbors but cannot receive dynamic shadow/GI
  from them. Accepted for mostly-static scenes; mitigated by the #3 swap up close.
- **Overhang/void/disconnected-topology fidelity.** Scalar displacement is a
  one-sided height field; geometry it cannot represent is handled by the #3
  fallback, not by this format.
- **Vector displacement, baked normal maps.** Explicitly dropped for v1 (scalar
  displacement, baked radiance only, no normals — "good enough for now").

## Settled Decisions (from brainstorming)

- **Purpose:** reduce per-part geometry weight while maintaining fidelity at all
  ranges (not distance-only LOD).
- **Representation:** bounding **cage mesh** + baked-lit **color** texture +
  scalar **displacement** texture (relief-mapped proxy).
- **Cage encloses the part:** a slightly inflated decimated hull, so the true
  surface is always inside it and displacement is a one-sided inward depth
  `∈ [0, maxDisp]`. Runtime is then plain relief mapping (enter at cage, march
  inward).
- **Cage = decimated part mesh** (reuse the existing mesh-simplifier); the
  displacement texture restores the fine detail decimation removed.
- **UVs:** per-triangle atlas (lightmapper-style charts with padding), shared by
  color and displacement.
- **Displacement:** scalar inward depth along the cage normal.
- **Color:** baked **radiance** (fully lit: sun + self-shadow + one GI bounce +
  baked AO), not albedo — so an imposter hit returns final color with no live
  PBR/shadow/GI for its own surface.
- **No normal map.**
- **Bake color on the GPU**, reusing the existing shading GLSL (cannot disagree
  with how parts actually render).
- **Full participant (A):** all three ray types intersect imposters.
- **Code lives in `MatterSurfaceLib`** alongside `part_asset` (the runtime half is
  shader + traversal changes that must live here; the bake half lives here too for
  one coherent module). A future imposter-research sibling can `-I` it.

## Terminology

- **Imposter** — a per-part cage + displacement + baked-color proxy that the ray
  tracer intersects in place of the part's triangle BVH.
- **Cage** — the coarse, inflated, decimated enclosing mesh that carries the UV
  atlas and provides the coarse silhouette and the relief-march entry surface.
- **Displacement (atlas)** — scalar inward depth from the cage surface to the true
  part surface, `∈ [0, maxDisp]`, in the cage's UV space.
- **Color (atlas)** — baked-lit radiance at the true surface, in the same UV space.
- **`.imp` file** — the on-disk imposter asset.
- **`ImpGenParams`** — bake parameters (cage decimation target, atlas resolution,
  cage inflation / `maxDisp`) forming the imposter's cache key.

## 1. Code Layout

All in `MatterSurfaceLib/`, splitting GL-free (testable) from GPU parts:

- `include/imposter_asset.h` / `src/imposter_asset.cpp` — `ImposterAsset` struct,
  `ImpGenParams`, format constants, `save`/`load`, and the **GL-free CPU geometry**:
  cage generation (via the mesh-simplifier) and UV-atlas packing. Same robustness
  pattern as `part_asset` (magic, format version, `sizeof` guards, content hash).
  The headless-testable core.
- `src/imposter_bake.cpp` — **GPU bake orchestration**: set up MRT targets, run the
  UV-space pass, read back the displacement + color atlases. Isolated because it
  needs a GL context.
- `shaders/imposter_bake.fs` (+ `.vs`) — the UV-space MRT bake shader, reusing
  `bvh_tlas_common.glsl` + `materials.glsl`.
- `shaders/bvh_tlas_common.glsl` + `shaders/raytrace_tlas_blas.fs` — add the
  `is_imposter` branch + relief march for all three ray types, plus color/
  displacement atlas samplers.
- `blas_manager` / `tlas_manager` / `main.cpp` — register imposter instances (the
  `is_imposter` tag + atlas handles), upload atlas textures, a **bake app-mode**
  (like `MSL_CAPTURE`), and a render path that drops an imposter into the scene.

Dependencies: `part_asset` (loads the source part), the mesh-simplifier, `bvh`, the
two managers, `material_registry`, and the shading includes.

## 2. Representation & On-Disk Format

Single binary file: `imposters/<imp_hash>.imp`. Raw POD array dumps, little-endian
(same portability assumptions as `.part`: WSL/Windows x86-64 only; layout changes
are caught and trigger regeneration, not conversion).

```
Header
  magic            u32  'IMPO'
  format_version   u32
  imp_hash         u64                        // cache key, see §3
  source_part_hash u64                        // the .part this was baked from
  sizeof_CageVert  u32                        // layout guards
  sizeof_CageTri   u32
  content_hash     u64                        // FNV-1a over all bytes after header

Metadata
  bounds_min       f32[3]                      // part-space AABB (for #3 + culling)
  bounds_max       f32[3]
  max_disp         f32                         // shell thickness (displacement range)
  parallax_radius  f32                         // recommended switch distance (#3 hint)
  atlas_w          u32
  atlas_h          u32

Cage mesh
  vert_count       u32
  CageVert[vert_count]                         // position f32[3], normal f32[3], uv f32[2]
  tri_count        u32
  CageTri[tri_count]                           // u32[3] vertex indices

Displacement atlas
  R8 or R16 texels, atlas_w * atlas_h          // scalar inward depth, normalized to [0,maxDisp]

Color atlas
  RGBA8 texels, atlas_w * atlas_h              // baked-lit radiance (A = coverage/mask)
```

The color atlas alpha channel stores a **coverage mask** (1 where a texel maps to
real surface, 0 in chart padding/gutters) so the runtime can reject misses and the
bake can dilate/pad chart edges.

Displacement precision (R8 vs R16) is an `ImpGenParams`-driven choice; R16 is the
default for smoother relief at the cost of memory.

### Validation on load (any failure → ignore file, regenerate, overwrite)

- `magic` mismatch
- `format_version` != current
- any `sizeof_*` != current `sizeof(...)`
- recomputed `content_hash` != stored (corruption)
- `imp_hash` in header != requested hash
- `source_part_hash` != the live source part's hash (stale imposter for changed
  geometry)

## 3. Cache Key

`ImpGenParams` (bake params) drives both baking and the cache key:

- cage decimation target (triangle budget or ratio)
- atlas resolution (`atlas_w`, `atlas_h`)
- cage inflation amount / `maxDisp`
- displacement precision (R8/R16)

`imp_hash = FNV1a(ImpGenParams bytes) ^ format_version`, and the file additionally
binds the **source `.part` hash** so changing the part's geometry (a new `.part`
hash) invalidates the imposter even if `ImpGenParams` is unchanged.

**Startup flow (per part that wants an imposter):**

1. Resolve the source part's `param_hash` (from `part_asset`).
2. Build `ImpGenParams`; compute `imp_hash`; path = `imposters/<imp_hash>.imp`.
3. If the file exists and passes all §2 validation (including the source-hash
   check) → load it.
4. Otherwise → bake via §4, then write the file (temp + rename for atomicity).

## 4. Baking Pipeline (GPU)

Turns a loaded part into the `.imp` asset.

1. **Assemble the part.** `part_asset::load` the `.part` into real BLAS/TLAS, then
   flatten all instances' triangles into one part-space mesh (the geometry source
   of truth).
2. **Build the cage (CPU).** Run the mesh-simplifier on that merged mesh,
   decimated to the `ImpGenParams` target, then inflate slightly so the cage
   encloses the part. Produces cage verts (position + normal) and tris.
3. **Atlas the cage (CPU).** Pack each cage triangle into a UV chart with padding
   at `atlas_w × atlas_h`. Deterministic; this and step 2 are the headless-testable
   core.
4. **Fill the textures (GPU, one MRT pass).** Rasterize the cage in **UV space**
   (`gl_Position = uv` mapped to clip space), carrying the cage world position +
   interpolated normal as varyings. For each atlas texel the fragment shader:
   - casts a ray from the cage point **inward along the normal** against the
     already-uploaded part BVH textures to find the nearest true surface hit;
   - writes **displacement** = inward distance cage→hit (normalized to
     `[0, maxDisp]`) to MRT target 0;
   - **shades** the hit with the existing traversal + `materials.glsl` includes
     (GGX + sun + shadow + one GI bounce + baked AO) and writes **baked radiance**
     to MRT target 1; writes coverage = 1 (else 0 on a miss).
   This reuses all existing shading GLSL — the bake cannot disagree with how parts
   render. Runs as an app mode like `MSL_CAPTURE`.
5. **Post (CPU).** Dilate chart edges into the gutter using the coverage mask
   (avoids black seams under bilinear sampling), read back both atlases, assemble
   the `ImposterAsset`, compute `content_hash`, write the file.

## 5. Runtime Relief-March Rendering

The imposter renders as a full participant in the existing single-pass frame.

- **Registration.** Cage triangles (with atlas UVs) go into a BLAS in the existing
  BVH; imposter instances go into the TLAS tagged `is_imposter` with handles to
  their color + displacement atlases. Existing traversal finds the cage-entry hit
  for free.
- **On hitting an imposter cage triangle**, the shader branches to a **relief
  march**: project the ray into the triangle's UV/tangent frame, linear-search the
  displacement atlas for the depth crossing, binary-refine to the true surface
  point.
- **Camera / GI closest-hit:** the march returns the hit; shading is a **baked-
  color texture read** — `calculatePBR` is bypassed (lighting is already in the
  texel).
- **Sun-shadow / GI-occlusion (any-hit):** the same march, stop at the first
  crossing → occluded. This is how an imposter casts shadows and blocks bounces on
  neighbors.
- **GI bounce into an imposter:** the bounce ray's march hit reads baked color as
  incoming radiance.
- **Coverage miss:** if the march reaches `maxDisp` without a covered crossing, the
  ray passes through (no hit) — the cage is a conservative bound, not opaque.

A frame mixes real triangle-BLAS parts (live PBR + shadow + GI as today) and
imposter parts (march + baked-color read) in one traversal. The only thing
imposters cannot do (accepted limitation) is *receive* dynamic shadow/GI.

## 6. Testing

Headless `tests/imposter_asset_tests` mirroring the existing `tests/*` suites,
plus a documented GPU/visual check (the GPU bake and relief-march shader cannot be
pure-CPU unit-tested; the CPU displacement test covers their geometry math, the
capture covers color/visual parity).

- **Cage/atlas (CPU):** decimate a small synthetic part; assert the inflated cage
  **encloses** every original vertex within tolerance; assert UV charts are within
  `[0,1]`, non-overlapping, and padded.
- **Displacement reconstruction (CPU):** for a synthetic part with a known surface,
  CPU-cast the displacement (the geometry half of the bake is CPU even though color
  is GPU) and assert reconstructed surface positions match the real surface within
  an error bound.
- **Format round-trip + guards (CPU):** save → load → byte-identical cage/atlas/
  metadata + matching `content_hash`; corrupt `format_version` / a `sizeof_*` / a
  data byte → load rejects.
- **Source-hash link (CPU):** assert the imposter stores the source `.part` hash
  and that load rejects a mismatch.
- **GPU/visual (manual via `MSL_CAPTURE`-style mode):** bake an imposter, render it
  and the real part from the same camera, compare images within a tolerance.

## Versioning & Forward Compatibility

`format_version` is the single growth switch, same contract as `.part`:

- Every reader branches on `format_version`; anything it cannot parse → fail
  validation → regenerate.
- Additive changes bump the version and append a new length-prefixed section, so
  appending never disturbs existing sections (e.g. a future baked-normal atlas, or
  vector-displacement, or embedded `ImpGenParams` for self-describing rebake).
- Layout-breaking changes bump the version and are independently caught by the
  `sizeof_*` guards.

**Planned future sections (not now):** baked normal atlas (relight/specular/
denoise), vector displacement (moderate overhangs before the #3 fallback),
embedded `ImpGenParams` for self-describing rebake. Deferred; the format is
designed so each is a version bump + appended section.

## Open Items

None blocking the current scope; all decisions settled in brainstorming.

**Deferred (future specs / versions):**

- #3 LOD selection + real-BLAS swap (separate spec; this format carries its hooks).
- Baked normals, vector displacement, embedded rebake params (behind a
  `format_version` bump).
