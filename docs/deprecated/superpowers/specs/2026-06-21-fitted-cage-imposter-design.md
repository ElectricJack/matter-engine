# Fitted-Cage Imposter Design

**Date:** 2026-06-21
**Status:** Approved (approach A — unify; success bar B — production quality, minor artifacts acceptable)

## Goal

Make the *fitted* (simplified, arbitrary-geometry) imposter cage render correctly and
convincingly from all viewing angles, and **unify** the cube debug path onto the same
mechanism so there is a single reorder-safe UV code path.

## Background / Root Cause

An imposter replaces a part's triangle BVH with a coarse cage mesh + a 16-bit displacement
atlas + a baked RGBA color atlas (A = coverage), relief-marched in the fragment shader.

The shader currently *recomputes* each cage triangle's atlas UVs from its triangle index
(`imposterTriUVs(localTri)`). But the BLAS reorders cage triangles into BVH order, so
`localTri` (BVH-order slot) no longer matches the order `build_cage` used when it baked the
atlas. The wrong atlas chart gets sampled → shuffled / melted blobs.

The cube path was fixed with a *geometry-derived* workaround: infer the face from the
world-space normal and the per-vertex UV from position within the cage AABB
(`imposterCubeFace` / `imposterCubeUV`). That only works because a cube has 6 axis-aligned
faces; it does **not** generalize to arbitrary fitted-cage triangles.

Key existing infrastructure:
- `ImposterAsset.verts` (`CageVert`) already stores correct per-vertex atlas UVs (`u`, `v`)
  for whatever chart layout the bake chose (square charts for the cube, right-triangle
  charts for the fitted cage). The bake is already correct.
- `BLASManager::get_entry(handle)->bvh->triIdx[i]` is public and gives the original cage
  triangle index for BVH-order slot `i` (the exact permutation that breaks index math).

## Approach (chosen)

Stop deriving UVs in the shader. **Plumb the baked per-vertex cage UVs to the shader**,
attached to the triangle's BVH slot so reordering cannot break them.

Considered and rejected:
- **Extra rows on the global triangle texture** to carry UVs: bloats *every* triangle in
  the scene by ~33% texture memory for an imposter-only need. Rejected (cost / YAGNI).
- **Re-baking the atlas in BVH order**: couples the GL-free bake to BLAS internals and
  re-packs the atlas. Rejected (complexity).

Chosen: a small **dedicated UV texture for the imposter cage**, built once at setup in
BVH order using the cage BLAS's `triIdx` permutation. The shader reads the three corner
UVs for the hit triangle's slot and feeds `reliefMarch` exactly as today. Because the UVs
are explicit per vertex, the same path serves the cube and the fitted cage — the cube's
normal/AABB special case is deleted.

## Components

### 1. `ImposterRenderer` UV texture (main.cpp)
After registering the cage BLAS and getting its triangle offset:
- Read the permutation: `const BLASEntry* e = blas_manager_->get_entry(imposter_cage_blas_);`
  then `e->bvh->triIdx[i]` for `i in [0, nCageTris)`.
- Build a CPU float buffer, **BVH order**, format RGBA32F, size `width = nCageTris`,
  `height = 3` (row `r` = corner `r`'s UV). For slot `i`:
  `orig = triIdx[i]; t = imp.tris[orig];` then row 0 = `verts[t.i0].uv`, row 1 =
  `verts[t.i1].uv`, row 2 = `verts[t.i2].uv`, stored as `(u, v, 0, 0)`.
- Upload as `imposter_triuv_tex_` with `TEXTURE_FILTER_POINT` (must not interpolate).
- Store `imposter_tri_count_ = nCageTris`.

### 2. Uniforms (main.cpp upload + shader declarations)
- Add: `uniform sampler2D imposterTriUvTex;` and `uniform int imposterTriCount;`.
- **Remove** (no longer used for UV selection): `imposterQuadCharts`, `imposterAabbMin`,
  `imposterAabbMax`. `imposterGrid` / `imposterPad` are no longer needed by the shader for
  UVs or the relief cell bound (see #4); remove their shader uses. (The bake keeps its own
  internal grid/pad — these are build-time constants in `imposter_asset.cpp`, untouched.)

### 3. Shader hit block (bvh_tlas_common.glsl)
Replace the cube/fitted UV branch with a single fetch:
```glsl
int localTri = int(triIdx) - imposterTriBase;
vec2 uv0 = texelFetch(imposterTriUvTex, ivec2(localTri, 0), 0).xy;
vec2 uv1 = texelFetch(imposterTriUvTex, ivec2(localTri, 1), 0).xy;
vec2 uv2 = texelFetch(imposterTriUvTex, ivec2(localTri, 2), 0).xy;
```
Delete `imposterTriUVs`, `imposterCubeFace`, `imposterCubeUV`. `reliefMarch` is unchanged
except for the cell bound (#4).

### 4. Relief cell bound from triangle UVs (bvh_tlas_common.glsl)
`reliefMarch` currently clamps the march to the uniform-grid cell
(`cellSz = 1/imposterGrid`). With explicit per-vertex UVs, clamp instead to the hit
triangle's UV bounding box (a small epsilon margin):
```glsl
vec2 cellLo = min(uv0, min(uv1, uv2)) - vec2(0.002);
vec2 cellHi = max(uv0, max(uv1, uv2)) + vec2(0.002);
```
This is both more correct (matches the actual chart) and removes the `imposterGrid`
dependency, so it works for square and right-triangle charts alike.

### 5. Bake (imposter_asset.cpp / imposter_bake.cpp)
No functional change required — it already emits correct per-vertex UVs. The cube
(`MSL_IMPOSTER_CUBE`) still selects box geometry + square charts in the bake; the shader
is now agnostic to which it was.

## Data Flow

bake → `ImposterAsset.verts[*].u/v` (per-vertex UVs, original cage order)
→ register cage BLAS (BVH reorders triangles)
→ setup reads `triIdx` permutation, packs UVs into `imposter_triuv_tex_` (BVH order)
→ shader `texelFetch(imposterTriUvTex, localTri)` → `reliefMarch` → color/disp atlas sample.

## Error Handling / Edge Cases
- If `imposter_cage_blas_` has no entry/bvh, skip the imposter (already guarded today).
- `localTri` is always in `[0, nCageTris)` because the hit came from the cage BLAS; no
  bounds clamp needed, but `texelFetch` of an out-of-range x is UB, so the build asserts
  `nCageTris == imp.tris.size()`.
- POINT filtering is mandatory; bilinear would smear UVs across corners.

## Testing
- **Unit (host, GL-free):** a test that, given a known `triIdx` permutation and an
  `ImposterAsset`, the CPU UV-packing produces row/column values matching
  `verts[tris[triIdx[i]].ik].uv`. Lives in `tests/imposter_asset_tests.cpp`.
- **Visual regression (WSLg headless):** render the fitted cage beside the real part and
  compare; then render the cube via the *unified* path and confirm it still matches the
  known-good cube reference (no regression). Capture with
  `MSL_SHOW_IMPOSTER=1 [MSL_IMPOSTER_CUBE=1] MSL_CAPTURE=... MSL_FRAMES=3 MSL_RENDER_MODE=0 MSL_CAM=...`.

## Quality (success bar B) — addressed after it renders
Once the fitted cage renders unshuffled, tune for "convincing from all angles":
- Atlas resolution / `MSL_IMP_MAXTRIS` / `MSL_IMP_RATIO` to trade cell size vs cage detail.
- Optionally flat per-face cage normals for crisper relief direction (defer; smooth
  normals' softening is an acceptable artifact for v1).
- Right-triangle chart seams between adjacent cage triangles are an accepted minor artifact.

## Out of Scope
- New UV unwrapping / quad-charting of the fitted cage.
- Changing the relief-march algorithm itself.
- LOD selection / when to swap real geometry for the imposter.
