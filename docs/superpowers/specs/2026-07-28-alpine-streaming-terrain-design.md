# Alpine Streaming Terrain Design

**Date:** 2026-07-28  
**Status:** Approved for implementation planning  
**World:** `StreamMeadow`

## Goal

Replace StreamMeadow's mellow rolling terrain with colossal alpine ranges: sharp
upper ridges, narrow flyable valleys, calmer erosion-shaped lower slopes, and
roughly 450–650 m of summit-to-valley relief. Preserve deterministic infinite
streaming and keep terrain baking affordable.

The first version continues to use the existing ForestFloor meadow material
across the complete terrain surface. Rock, snow, and other elevation-specific
materials are deferred.

## Current State

- `StreamMeadow.field()` combines two low-amplitude smooth noise fields and
  produces approximately `-2…24 m` terrain inside a `-32…96 m` world range.
- The native terrain field already provides `noise2`, `ridge2`, `warp2`,
  arithmetic composition, `smoothstep`, and `blend`.
- The field is heightfield-shaped: `density(x,y,z) = height(x,z) - y`.
- The streamer currently has three rungs. `WorldSector` treats them only as
  scatter-detail tiers and always asks `terrainVolume` for the same 2 m voxel
  mesh.
- The voxel mesher samples the complete authored Y range for every sector.
  Raising the ceiling to accommodate colossal mountains would therefore
  multiply memory traffic, density evaluations, and extraction work.
- `terrainVolume` can classify grass, dirt, rock, and snow, but the packed
  ForestFloor atlas used by StreamMeadow is bound to `MAT.dirt`.

## Scope

This cycle includes:

1. A layered alpine height field built from the existing native field
   primitives.
2. A heightfield-aware fast path for near voxel-sector extraction.
3. A six-level terrain LOD ladder, from one quad to the 2 m voxel surface.
4. Strict 2:1 adjacency balancing and one-step transition stitching.
5. Separation of terrain LOD from scatter detail.
6. Mapping every StreamMeadow terrain classification to `MAT.dirt`.
7. Automated geometry, streaming, determinism, and terrain-character tests.

This cycle does not include:

- hydraulic or thermal erosion simulation;
- cached erosion macro-tiles;
- caves, overhangs, or non-heightfield density;
- additional terrain textures, rock strata, or snow;
- runtime terrain editing;
- continuous LOD geomorphing.

## Terrain Field

### Layer 1: massif layout

A very-low-frequency noise field defines connected mountain masses and major
valley corridors. It is domain-warped so ranges do not follow the noise lattice.
The intended wavelengths are approximately `1.2–2.2 km`, with `180–320 m` of
domain displacement.

This layer is a mask and broad base shape, not fine surface detail. Its purpose
is to make a mountain occupy many 64 m sectors and read as a range rather than
an isolated noise bump.

### Layer 2: primary alpine profile

Warped ridged multifractal noise at approximately `700–1,200 m` wavelengths
forms the main crests. A nonlinear remap compresses low values into narrow
valleys while preserving broad connected mountain bodies and strong summit
relief.

The combined massif and primary-ridge profile targets:

- ordinary valley floors between approximately `-30 m` and `50 m`;
- typical summit-to-valley relief of `450–650 m`;
- mountain masses spanning `800–1,500 m`;
- major flyable valley corridors approximately `100–250 m` wide.

The authored world range becomes `yMin = -96` and `yMax = 704`. Terrain is not
silently clamped to those values; a sampled sector that escapes the range is a
field-authoring error.

`seaLevel` moves to `-80 m`, below the designed valley floor. This keeps the
complete alpine surface in the Foothills scatter path and avoids accidentally
turning low valleys into ocean sectors.

### Layer 3: elevation-shaped erosion character

The primary profile produces a normalized elevation mask that divides the
mountain character into overlapping bands:

- **Lower third:** suppress high-frequency displacement and retain broad,
  calmer slopes.
- **Middle third:** subtract winding, domain-warped drainage channels to break
  up uniform faces and suggest erosion.
- **Upper third:** fade in secondary ridges and crag-scale displacement so
  summits remain sharp.

The bands blend continuously. There are no elevation discontinuities or hard
terraces. A low-amplitude valley-floor layer prevents the lowest corridors from
becoming razor-thin or noisy.

This is an analytical erosion approximation. It adds a small fixed number of
point-evaluable noise operations and does not require neighborhood simulation,
macro-tile state, or cross-sector synchronization.

### Field evaluation order

The height graph is emitted before the moisture and relief controls. Since
`height_at` evaluates only through the height register, height sampling does not
pay for unused biome controls.

StreamMeadow retains its thresholds above the control-noise range so its
above-sea-level terrain continues to classify as Foothills for scatter
selection. The field program must remain below the existing 64-operation limit.

## Heightfield-Aware Voxel Fast Path

The near terrain remains the existing native voxel/surface-nets output, but its
heightfield case is optimized:

1. Evaluate `height_at` once for every X/Z lattice point, including the existing
   border halo.
2. Compute the exact local minimum and maximum from that complete lattice. This
   is not a coarse probe and cannot miss a surface sample used by extraction.
3. Expand the range by two vertical voxel cells on both sides.
4. Convert the expanded range to integer indices relative to the authored
   global `yMin`. This preserves one global Y lattice even when neighboring
   sectors choose different local slabs.
5. Fill density as `cachedHeight(x,z) - y` within only that snapped local slab.
6. Run surface nets over the local slab and retain world-aligned vertex
   positions.

The optimization changes neither the sampled surface nor its deterministic
result. It removes repeated height evaluation for every Y sample and makes cost
depend on local surface variation rather than the complete `-96…704 m` range.

The optimization is explicitly a heightfield path. A future non-heightfield
density program must use a general density-volume fallback rather than this
shortcut.

## Terrain LOD Ladder

Terrain LOD is separate from scatter detail. For a 64 m sector, the stable
terrain ladder is:

| Terrain LOD | Representation | Horizontal cells | Cell size |
|---:|---|---:|---:|
| 0 | Native heightfield mesh | 1×1 | 64 m |
| 1 | Native heightfield mesh | 2×2 | 32 m |
| 2 | Native heightfield mesh | 4×4 | 16 m |
| 3 | Native heightfield mesh | 8×8 | 8 m |
| 4 | Native heightfield mesh | 16×16 | 4 m |
| 5 | Native voxel/surface-nets mesh | 32×32 nominal | 2 m |

LOD 0 is a literal two-triangle surface quad before its transition or safety
edge geometry. LODs 1–4 sample the same `height_at` function and emit native
heightfield triangles with gradient-derived smooth normals. They do not allocate
or extract a voxel volume.

The default radial profile, expressed in sector sizes `S`, is:

| Radius | Terrain LOD |
|---:|---:|
| `0…3S` | 5 |
| `3S…5S` | 4 |
| `5S…8S` | 3 |
| `8S…14S` | 2 |
| `14S…24S` | 1 |
| `24S…40S` | 0 |

These bands preserve the existing 3-sector full-detail radius and 40-sector
streaming extent.

## Balanced Neighbors and Transition Edges

The stable desired and resident terrain maps obey:

`abs(lod(sector) - lod(cardinalNeighbor)) <= 1`

The default radial thresholds are separated by at least two sector widths, so a
single cardinal or diagonal sector step cannot cross two bands. The streamer
also enforces the rule explicitly, including custom ring profiles, rather than
depending only on the default geometry.

Each terrain request carries a four-bit edge mask identifying cardinal
neighbors that are exactly one terrain LOD coarser. Because larger differences
are forbidden, no exact neighbor level is needed.

The finer sector owns a 2:1 transition:

1. Every other fine edge vertex aligns with a coarse edge vertex.
2. Intermediate fine edge vertices are collapsed to the coarse edge
   interpolation.
3. A small transition strip triangulates the fine interior row to that shared
   coarse boundary.
4. Equal-LOD edges use the ordinary grid boundary.

The coarser sector needs no special geometry for a finer neighbor. Shallow
safety skirts remain beneath sector borders only to hide a transient frame
during an asynchronous replacement; they are not the stable crack solution and
do not extend to the world minimum.

The publication path does not replace a resident mesh with a result that would
create a neighbor difference greater than one. If a balanced group is not ready,
the existing resident mesh remains visible. Edge-mask changes are treated as
terrain-variant changes so the stable published pair has matching topology.

## Scatter Detail

The six terrain levels do not expand expensive scatter. A separate
`scatterTier` retains the existing distance policy:

| Terrain LOD | Scatter tier | Content |
|---:|---:|---|
| 0–2 | 0 | trees and landmark boulders |
| 3–4 | 1 | plus rocks and pebbles |
| 5 | 2 | plus grass |

`WorldSector` receives both values. Terrain generation uses `terrainLod` and
the four-edge mask; vegetation gates use `scatterTier`.

## Material Policy

For this first alpine pass, all four terrain material classifications supplied
to the native terrain generators map to `MAT.dirt`. Steep slopes and high
summits therefore use the same ForestFloor packed material as valley floors.

Biome and slope queries remain available for scatter filtering and future
material work. This change does not remove native rock or snow classification;
it only maps StreamMeadow's generated terrain buckets to one material.

## Data Flow

1. The streaming anchor produces the radial desired terrain-LOD map.
2. The streamer balances that map so cardinal neighbors differ by at most one.
3. Terrain LOD maps to the independent scatter tier.
4. Each sector request includes coordinates, terrain LOD, scatter tier, field
   hash, biome table, and four-edge coarser-neighbor mask.
5. `WorldSector` chooses the native heightfield generator for LODs 0–4 or the
   optimized voxel generator for LOD 5.
6. The generated terrain uses the meadow material mapping; scatter is added
   according to its independent tier.
7. Publication waits until replacing the resident preserves balanced
   adjacency and compatible edge masks.

## Error Handling

- Invalid terrain LODs or edge masks fail the sector bake with coordinates in
  the error.
- A sampled height outside the authored world range fails that sector rather
  than flattening or clipping the mountain.
- A failed heightfield or voxel build leaves the current resident sector in
  place and follows the existing retry/backoff path.
- A transition variant that is not yet compatible with resident neighbors is
  held, not published.
- Field-graph parse errors and the 64-operation limit continue to fail at world
  load.

## Verification

### Terrain field

- Repeated evaluation of the same seed is byte-stable.
- Changing `worldSeed` changes the field program and sampled terrain.
- A deterministic multi-kilometer sample window contains at least `450 m` of
  summit-to-valley relief and remains inside `-96…704 m`.
- Heights are finite and continuous across positive and negative coordinates.
- A lower-elevation roughness metric is measurably calmer than the corresponding
  upper-ridge metric.
- The emitted field graph remains below 64 operations.

### Meshing

- Heightfield LOD top-surface counts are exactly 2, 8, 32, 128, and 512
  triangles before transition/safety geometry.
- Heightfield meshes are deterministic and use gradient-derived normals.
- The local voxel slab contains every sampled surface and uses globally aligned
  Y indices.
- The optimized near mesh matches the prior full-slab surface within the
  existing voxel tolerance.
- Equal-LOD borders match.
- Every legal `N ↔ N+1` pairing has a closed 2:1 transition on all four edges.
- Invalid differences greater than one are rejected by balancing/publication
  tests.

### Streaming

- Default and custom desired maps are balanced.
- Promotion, demotion, delayed bake, and out-of-order completion never publish
  resident cardinal neighbors more than one LOD apart.
- Edge-mask changes request the correct transition variant.
- Terrain LOD changes do not expand grass, rock, or landmark scatter radii.

### Materials

- Every StreamMeadow terrain bucket resolves to `MAT.dirt`.
- Existing ForestFloor tileset binding remains active.

### Performance

CI uses structural cost assertions rather than wall-clock thresholds:

- LODs 0–4 do not allocate a voxel density volume.
- Near height evaluation occurs once per X/Z lattice point, not once per X/Y/Z
  sample.
- Local voxel slab depth is derived from sampled local height range.

During implementation, record representative valley, slope, and summit sector
bake timings before and after the fast path. The acceptance target is that a
full-detail alpine sector is not slower than the current StreamMeadow
full-height-slab sector despite the richer field, while coarse sectors show the
expected orders-of-magnitude triangle reduction.

### Rendered acceptance

A StreamMeadow fly-through must show:

- mountain ranges rather than isolated smooth hills;
- a clear sense of 450–650 m vertical scale;
- sharp upper ridges and calmer lower slopes;
- narrow but navigable valley corridors;
- no persistent cracks at stable LOD boundaries;
- no objectionable terrain holes during streaming replacements;
- acceptable LOD changes at the default ring distances;
- the existing meadow material across the terrain.

## Compatibility and Rollout

The world field and sector parameters are content-hashed, so the change
naturally invalidates affected transient terrain variants. Scatter child assets
remain reusable. No cache deletion or artifact migration is required.

The new heightfield generator and balanced terrain LOD inputs are reusable by
future streaming worlds, but this cycle changes only StreamMeadow's authored
terrain and material mapping.
