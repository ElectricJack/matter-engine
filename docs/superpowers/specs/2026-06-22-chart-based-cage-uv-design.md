# Chart-Based Cage UV System for Fitted Imposters — Design

**Date:** 2026-06-22
**Status:** Approved (design); pending implementation plan
**Branch:** `feature/imposter-generation`
**Supersedes the packing approach in:** `2026-06-21-fitted-cage-imposter-design.md`

## Problem

Fitted (arbitrary-geometry, simplified-cage) imposters render with per-facet
fragmentation. The current atlas packs each cage triangle into its own grid cell
as a right-triangle, in triangle-index order, so atlas-space neighbors are
*index* neighbors, not *surface* neighbors. The relief march is bounded to a
single triangle's UV cell; a ray that enters one facet and exits through a
surface-adjacent facet leaves the cell, samples garbage, and passes through —
producing fragmentation. This was proven (atlas coverage ~full, flat-sample
solid); it is not a coverage/cage/interpenetration problem.

The fix is a proper chart-based UV atlas: weld cage triangle adjacency, group
triangles into low-distortion charts, UV-unwrap and pack multi-triangle charts,
and let the relief march cross triangle boundaries *within* a chart over a
continuous heightfield.

## Key Constraint That Shapes Everything

`reliefMarch` (shaders/bvh_tlas_common.glsl) historically built **one tangent
frame from the hit triangle** and reused it for the whole march, stepping
linearly in UV. That is an affine = flat assumption. Extrapolating one flat
frame across a curved chart produces two compounding errors that grow with the
chart's normal spread:

1. **Trajectory error** — the computed UV drifts from where the ray actually is
   on the curved surface.
2. **Depth error** — `pen = inward * s` is measured against the flat frame, but
   the heightfield was baked relative to the local (curved-away) surface, so the
   surface-crossing test fires at the wrong depth.

Target geometry is organic (mostly curved). Therefore the march is reformulated
to be **piecewise-affine**: it re-anchors the tangent frame per cage triangle as
it crosses triangle boundaries, using a baked per-texel triangle-id. Each cage
triangle is exactly flat, so its own frame is exact and the geometric march
error goes to zero regardless of overall chart curvature. The only residual is
*resolution* distortion (steeply-tilted triangles project to fewer texels →
local blur, not misalignment), which is acceptable under the "B with some
artifacts" success bar and improvable later with area-aware packing.

## Architecture & Data Flow

Same pipeline shape as today; the per-triangle grid packing in `build_cage` is
replaced by a chart pipeline. All CPU/geometry stages are GL-free and
unit-testable.

```
part tris
  → simplify_mesh (indexed cage; shared verts)
  → per-face oriented normals
  → build_adjacency        (per-triangle neighbor across each edge)
  → segment_charts         (normal-cone region growing)
  → project_chart          (orthographic projection per chart)
  → pack_charts            (shelf/skyline bin packer → atlas UVs + per-chart rect)
  → emit verts/tris        (3 verts per tri, triangle order, inflated; + per-tri chart rect)
  → bake_displacement_cpu  (rasterize tris into atlas: disp + coverage + triangle-id)
  → dilate_atlas           (unchanged)
  → save
```

Runtime: UVs (plus each triangle's chart rect and its cage-triangle-id) are
packed in BVH-triangle order into `imposterTriUvTex`; the triangle-id atlas and a
cage-triangle-data texture are uploaded; the shader marches piecewise-affine,
bounded to the hit triangle's chart rect, terminating at the chart edge.

## Scope Boundary

**In scope (this spec):**
- Adjacency, chart segmentation, planar projection, packing.
- Triangle-id atlas + cage-triangle-data texture.
- Piecewise-affine re-anchoring march, bounded to a chart rect.
- The march crosses triangles **within** a chart and **terminates** (pass-through)
  at the chart's UV-rect edge.

**Explicitly out of scope (clean future follow-on):**
- Cross-chart adjacency handoff (continuing the march into a neighbor chart
  across a UV seam). `build_adjacency`'s output and the triangle-id machinery
  leave a natural place to add this later.
- Area-aware packing to reduce resolution distortion on grazing facets.

## Components

All new CPU components live in `MatterSurfaceLib/src/imposter_asset.cpp` with
declarations in `MatterSurfaceLib/include/imposter_asset.h`, alongside existing
imposter geometry functions. Each is a small, independently testable unit.

### `build_adjacency`
From the indexed cage (`cage.indices` with shared vertices, `cage.triangleCount`)
build per-triangle neighbor info: for each triangle, `int nbr[3]` giving the
neighbor triangle across edge (i0,i1), (i1,i2), (i2,i0), or -1 for a boundary
edge. Implemented via an edge → triangle map keyed by the sorted vertex-index
pair. Pure function over indices; no normals, no GL.

`simplify_mesh` already returns an indexed mesh with shared vertices
(imposter_asset.cpp ~line 224), so adjacency is recoverable directly from the
indices — no position-welding needed. Adjacency must be built **before** the
per-triangle vertex duplication that emits `out.verts`.

### `segment_charts`
Region-growing chart assignment:
- Compute a per-face geometric normal (normalized cross product), oriented
  outward using the existing centroid-orientation logic (imposter_asset.cpp
  ~lines 233-256).
- Greedy flood fill: pick an unassigned seed face; start a chart with running
  average normal = seed normal. BFS over `build_adjacency` neighbors; a candidate
  face joins iff `angle(faceNormal, chartAvgNormal) ≤ chartConeDeg`. Update the
  chart's running average normal (accumulate then renormalize) as faces join.
- Output: per-face `int chartId` and, per chart, the average normal and member
  face list.

`chartConeDeg` is a new `ImpGenParams` field, default 75°, and **must** stay
strictly below 90° so the planar projection cannot fold/self-overlap. Its role
is now only to keep the projection non-overlapping; march accuracy no longer
depends on it (re-anchoring handles curvature).

### `project_chart`
For each chart, build an orthonormal tangent basis `(T, B)` spanning the chart's
average-normal plane (from a robust up-vector choice to avoid degeneracy when the
normal is near the chosen up axis). Project each chart corner position:
`uv2d = (dot(p − c, T), dot(p − c, B))`, where `c` is the chart centroid. Zero
distortion for a flat chart, bounded for near-flat. Coordinates are
per-(chart, corner): a cage vertex shared by N charts gets N independent 2D
copies (this matches the later per-triangle vertex duplication, so no extra
bookkeeping is needed).

### `pack_charts`
Shelf/skyline rectangle bin packer:
- Each chart has a 2D bbox (in projected world units) → an axis-aligned rect.
- Choose a uniform world→texel `scale` from total chart area vs atlas area times
  a fill factor: `scale = sqrt(fillFactor * atlasW * atlasH / sum(chartArea))`.
- Each chart's texel rect is `(chartW*scale + 2*pad, chartH*scale + 2*pad)`.
- Pack rects with a skyline packer. If packing overflows the atlas, multiply
  `scale` by a shrink factor (e.g. 0.9) and retry, up to a small retry cap.
- Output: per-chart atlas offset `(ox, oy)` and the final `scale`, yielding final
  atlas UVs per (chart, corner):
  `u = (ox + pad + (proj_u − chartMinU) * scale) / atlasW` (v analogous), and the
  chart's final UV rect `[lo, hi]`.

`pad` is a texel gutter so dilation + bilinear sampling do not bleed across
charts.

### `build_cage` (restructured)
Calls the above in order, then emits exactly as today so the BVH/runtime side is
untouched except for added per-triangle data:
1. `simplify_mesh` → indexed cage.
2. per-face oriented normals; existing smoothed per-vertex normals for inflation.
3. `build_adjacency`.
4. `segment_charts`.
5. `project_chart` per chart.
6. `pack_charts` → atlas UVs + per-chart rect.
7. Emit `out.verts` (3 per triangle, triangle order, positions inflated along the
   existing smoothed per-vertex normal by `p.inflation`) and `out.tris`
   (`{t*3, t*3+1, t*3+2}`) — unchanged structure.
8. Record, per triangle, its chart's UV rect (for the march bound). Carried to the
   shader via spare channels of `imposterTriUvTex` (see Plumbing).

### `pack_cage_tri_data` (new)
Keyed by cage-triangle-id (native `out.tris` order — BVH-independent). Per
triangle, packs the 3 corner positions (cage space) and 3 UVs into an RGBA32F
buffer for `imposterCageTriTex`. Used by the shader to recompute the tangent
frame when the march crosses into a new triangle. GL-free, unit-testable.

### `pack_cage_uvs_bvh_order` (extended)
Already packs per-vertex UVs in BVH-triangle order into the RGBA32F
`imposterTriUvTex` (width = nTris, height = 3). Extended channel map:
- row 0: `.xy` = uv0, `.zw` = chartLo (chart UV-rect min)
- row 1: `.xy` = uv1, `.zw` = chartHi (chart UV-rect max)
- row 2: `.xy` = uv2, `.z` = cageTriId (`= triIdx[slot]`, the seed triangle id),
  `.w` = unused

Because this runs in BVH order with `triIdx` in hand, `triIdx[slot]` is exactly
the cage-triangle-id for that BVH slot, giving the shader the initial triangle id
to seed the march. Remains reorder-invariant.

## Baked Data Additions

`ImposterAsset` gains a triangle-id atlas:
- `std::vector<uint8_t> triid;` sized `atlas_w * atlas_h * 2` (one uint16 per
  texel; `0xFFFF` = uncovered / no triangle).

`bake_displacement_cpu` is restructured from iterating grid cells to **iterating
cage triangles**: for each triangle, rasterize its packed atlas-space triangle
(barycentric per covered texel), interpolate the inflated cage position + normal,
cast the inward `−normal` ray against the part BVH, and write displacement +
coverage (255) + the cage-triangle-id (uint16) for that texel. Because a chart's
triangles pack contiguously and share UV edges, the heightfield is continuous
across in-chart triangle seams. A half-open rasterization rule (or last-write-
wins) resolves shared-edge texels; either of the two adjacent ids is a valid
anchor near the seam. `dilate_atlas` is unchanged and still operates on the
coverage mask.

## Runtime / Shader Plumbing

`main.cpp`:
- Continue building `imposterTriUvTex` via the extended `pack_cage_uvs_bvh_order`.
- Build `imposterCageTriTex` (RGBA32F) via `pack_cage_tri_data`, point-filtered.
- Upload `imposterTriIdTex` from `ImposterAsset.triid` as an R16 (point-filtered)
  texture.
- Add matching uniforms for the two new textures.

`shaders/bvh_tlas_common.glsl` — `reliefMarch` reformulated to world-space
piecewise-affine stepping:
- March parameter `s` = world arc length; `P = entryPos + rayDir * s`.
- Maintain a current triangle T (seed = hit triangle; `cageTriId` from
  `imposterTriUvTex` row2.z). T provides `v0, e1, e2, uv0..2, faceN`.
- Per step: solve `P − v0` in `(e1, e2)` → barycentric → `uvc`;
  `pen = −dot(P − v0, faceN)` (depth below T's plane, positive = inside shell).
- Sample `imposterTriIdTex` at `uvc`. If it names a different valid triangle,
  load it from `imposterCageTriTex` (positions transformed by the instance
  transform into the same space as the seed `w0..w2`), and re-project the same
  `P` → fresh `uvc`, `pen`. One re-eval per crossing.
- `d = texture(imposterDispTex, uvc).r * imposterMaxDisp`; if covered and
  `pen ≥ d`, binary-refine between the previous and current `s` → hit.
- Terminate when `uvc` leaves the chart rect (`chartLo`/`chartHi` from
  `imposterTriUvTex`) or the sampled triangle-id is invalid (`0xFFFF`).

The chart rect replaces the old single-triangle `cellLo`/`cellHi` bound; the
single-frame extrapolation is gone.

## Params / Cache

- Add `float chartConeDeg` to `ImpGenParams` (28 → 32 bytes). Update the
  `static_assert(sizeof(ImpGenParams) == ...)` and `compute_imp_hash`.
- Bump `kFormatVersion` so stale cached `.imp` files (old layout, no triangle-id
  atlas) fail `load` and regenerate.
- `save`/`load` serialize the new `triid` atlas.

## Testing (TDD, GL-free units)

- **build_adjacency:** two-triangle quad → the two triangles are neighbors across
  the shared edge, boundary edges = -1; cube → each triangle has 3 neighbors.
- **segment_charts:** flat quad → 1 chart; axis-aligned cube @ cone < 90° → 6
  charts (one per face); cone > 90° → fewer charts.
- **project_chart:** flat chart → projected edge lengths preserved up to the
  uniform scale (zero distortion); basis is orthonormal and non-degenerate for a
  normal aligned with the chosen up-axis.
- **pack_charts:** N rects placed with no overlap, all within `[0, atlas]`,
  padding respected; an over-budget set triggers a scale shrink and still fits.
- **pack_cage_uvs_bvh_order:** chart-rect and cageTriId channels correct, and the
  whole buffer is invariant under a permuted `triIdx`.
- **pack_cage_tri_data:** layout + round-trip of a known triangle's positions and
  UVs; native (non-BVH) ordering.
- **bake_displacement_cpu:** known chart → expected coverage; adjacent texels
  across a shared in-chart edge are both covered (continuity); triangle-id is
  written for every covered texel and is `0xFFFF` for misses.
- **End-to-end (manual/visual):**
  - cube path still renders perfectly (regression of the unified path).
  - the fitted part no longer shows per-facet fragmentation.
  - a deliberately curved/organic test chart marches without smearing — the exact
    artifact the flat-frame version would exhibit, confirming re-anchoring works.

## Risks & Mitigations

- **Planar projection self-overlap** if a chart wraps past 90°: capped by
  `chartConeDeg < 90°` (default 75°).
- **Packing overflow** on dense cages: iterative `scale` shrink with a retry cap;
  `maxCageTris` already bounds cage size upstream.
- **Per-step shader cost** of re-anchoring: re-fetch only on triangle-id change
  (cached between steps), not every step, so most steps are a single id sample
  plus the existing math.
- **Space mismatch** between baked cage-space positions and the world-space seed
  frame: the shader applies the instance transform to `imposterCageTriTex`
  positions so re-anchored frames live in the same space as the seed `w0..w2`.
- **Resolution distortion** on grazing facets: accepted for this iteration
  (blur, not misalignment); area-aware packing is a noted future improvement.
