# StreamMountain rock density restoration

Task `quick-meadow-68.3`, September 30, 2026. This is the targeted authored-scene
rollback chosen by the [provenance audit](vg-integration-provenance-and-rock-population-2026-09-30.md).
It applies on the VG-disabled-by-default head **`563945f01`**
(`quick-meadow-68.2`, which is also `origin/vg-vt-improvements`). It changes
the StreamMountain detailed-rock population only. No branch or terrain revert
was made, and no GPU capture or performance result is claimed here. Matched
captures belong to the integration candidate (`quick-meadow-68.4`).

## What changed

The shipped scene now places the **three focal rocks** of the 2026-09-18
mountain pilot instead of the 2026-09-19 1,283-rock stress site. The rocks keep
their current resolution of 128. The 1,280 stress-grid additions stay
available, unchanged, but only as an explicit benchmark profile.

| Setting | Before (`563945f01`) | After |
|---|---|---|
| `mountainGeometrySamples(material)` (`projects/world_demo/shared-lib/mountain_geometry_site.js`) | 3 focal + 32×32 grid (1,024 small, 256 large) = 1,283 | 3 focal records; the grid is built only for `{stress:true}` |
| `mountainGeometryCatalog(material)` | 19 `MountainDetailRock` assets | 3 assets (19 with `{stress:true}`) |
| `StreamMountain.params` (`StreamMountain.js:64-67`) | `{worldSeed:20260722, terrainOnly:false}` | adds `geometryRockStress:false` |
| `StreamMountain.biomes()` (`StreamMountain.js:385`) | `__geometryRocks:{material}` | unchanged, plus `__geometryRockStress:<param>` |
| `WorldSector.js:48-50,60,63-64,164-165` | site calls take only the material | `geometrySite(table)` passes `{stress: table.__geometryRockStress === true}` to both `requires()` and `build()` |

The retained focal records are exact. Production material is the existing
`Mountain.GeometryRock` handle.

| X | Z | Seed | Shape | Size (m) | Resolution |
|---:|---:|---:|---:|---:|---:|
| 416 | 1456 | 0 | 0 | 6 | 128 |
| 425 | 1458 | 1 | 2 | 4 | 128 |
| 422 | 1448 | 2 | 1 | 7 | 128 |

Placement code is unchanged: half-open XZ and Y tile ownership, ground
`heightAt(x,z)`, burial `min(0.35, size*0.12)`, and instanced children with
`inlineBelowPx:0`.

### Unchanged on purpose

- Terrain: `voxelRung = max(-5, min(3, terrainLod-2))` (0.25 m finest
  sampling), `terrainTexelsPerMeter:128`, the direct `mountainSurface` source,
  relief terms, `volumetricSectors:true`, world seed and camera.
- Natural `MountainRock` landmarks, boulders and scree, the forest planner and
  its rock clearance, and both catalogs. The comparison below proves these
  placements and catalogs are identical.
- `props.json` is byte-identical: POM off, volumetrics, the saved scatter and
  terrain bands, and the old `Rock/hide` override.
- VG stays opt-in (`MATTER_GEOMETRY_PAGES=1`). The VT, static-buffer,
  vertex-cache, persistent-cut and GI work on the branch is untouched.

## Deterministic population comparison

`population.mjs` extracts the before tree from Git and evaluates both trees
with their own shared libraries. It runs the real `StreamMountain.biomes()`,
the real `WorldSector.requires()`/`build()` and the real natural planners. The
script and its output are in
[`docs/agent/evidence/2026-09-30-rock-density-restore/`](../agent/evidence/2026-09-30-rock-density-restore/population.json).

Authored inspection-site quantities:

| Quantity | Before | After |
|---|---:|---:|
| Site placements | 1,283 | **3** |
| Unique detailed assets | 19 | **3** |
| Cube resolution | 128 | 128 |
| Placed source triangles (12·r² per placement) | 252,248,064 | 589,824 |
| Unique source triangles | 3,735,552 | 589,824 |
| `WorldSector.requires()` entries | 71 | 55 |
| Non-site `requires()` entries | 52 (`cb73926c…`) | 52 (identical hash) |

Placed source triangles are authored complexity, not rendered or resident
triangles.

Sector census, streamed as cube tiles (rung 2, terrain LOD 5, every `ty` in
the −96…704 m slab) at each nested level. Every tile overlapping
x 256–576, z 1280–1600 is included. Terrain is a synthetic analytic field
(`70 + 0.05x − 0.025z + 3 sin(x/37) cos(z/53)`), so the natural counts compare
the two trees and do **not** census the real mountain:

| Tile size | Area covered (x; z) | Site before | Site after | Natural rocks | Forest | Natural placements hash |
|---:|---|---:|---:|---:|---:|---|
| 64 m | 256–576; 1280–1600 | 1,283 | 3 | 183 | 283 | identical |
| 128 m | 256–640; 1280–1664 | 1,283 | 3 | 241 | 410 | identical |
| 256 m | 256–768; 1280–1792 | 1,283 | 3 | 382 | 662 | identical |
| 512 m | 0–1024; 1024–2048 | 1,283 | 3 | 1,489 | 2,693 | identical |

Each rock has exactly one owner at every level. The sorted site placement set
is the same at every level (before `857f68f8…`, after `eb4b6d29…`). The
`{stress:true}` profile reproduces the old 1,283 records byte for byte (SHA-256
`8cd752fb…` in the census material registry; `cb4e9f79…` with the test's
sentinel material 7).

## Benchmark profile

To reproduce the September 19 stress population, set
`StreamMountain.params.geometryRockStress` to `true`. Do this in a generated
scene copy, as `tools/dense_terrain_diagnostic.py` does, or in a local edit
that is never committed. The flag flows through `biomes()` to every
`WorldSector` bake. Compare the profile only against captures that use the same
population, and do not count the density reduction as an algorithmic speedup.

## Cache consequence

Shared-lib sources fold into part hashes (`MatterEngine3/src/module_resolver.h`),
and the biomes string gains `__geometryRockStress`. Every StreamMountain
`WorldSector` therefore re-keys and bakes cold once. The 16 stress-only
`MountainDetailRock` assets are no longer requested. Their cache entries are
dead weight and are not reused.

## Verification

```sh
node docs/agent/evidence/2026-09-30-rock-density-restore/population.mjs
node projects/world_demo/tests/mountain_geometry_site_tests.mjs
node projects/world_demo/tests/mountain_detail_rocks_tests.mjs
node projects/world_demo/tests/mountain_terrain_only_tests.mjs
node projects/world_demo/tests/mountain_rocks_tests.mjs
node projects/world_demo/tests/mountain_forest_tests.mjs
node projects/world_demo/tests/mountain_routes_tests.mjs
node docs/agent/evidence/2026-09-30-vg-integration-audit/audit.mjs
```

Native: `eval_world_tests` evaluates the shipped `StreamMountain.js` through
QuickJS. It now also asserts that the shipped biomes carry
`__geometryRockStress:false`. With the canonical MSVC RelWithDebInfo build
(`tools/build-windows-from-wsl.sh RelWithDebInfo eval_world_tests`) and
`ctest -R ^eval_world_tests$`, it reports ALL PASS. A temporary local flip to
`geometryRockStress: true` failed exactly that check. The flip was reverted and
never committed.
