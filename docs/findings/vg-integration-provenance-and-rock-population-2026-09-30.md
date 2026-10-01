# VG integration provenance and StreamMountain rock population

Task `quick-meadow-68.1`, September 30, 2026. This is the source/baseline audit
for the authorized VG integration and rock population restoration. Implementation
and publication to `main` follow separately. The audit does not claim delivery
to `main` or a new performance/visual acceptance pass.

Use the fetched `origin/vg-vt-improvements` head **`6c70efe2e`**, containing the
complete clear-ridge work. Restore the earlier **three-rock inspection site**
by removing the 1,280 grid additions while retaining the current per-rock
resolution and the surrounding natural rock/forest/terrain work. That target
is inferred from the explicit pre-stress evidence and the current three-record
prefix; it is not an earlier committed scene tree. The historical Git baseline
predates the entire new environment and would undo much more than population.

## Exact source snapshot and ancestry

`git fetch origin main vg-vt-improvements` exited 0 in the owned slot. No
primary-checkout files or untracked capture evidence were modified.

| Reference | Full SHA |
|---|---|
| Fetched `origin/main` | `8aab2e54bbbffbb0f6044ec3bda36974e30b864b` |
| Fetched `origin/vg-vt-improvements` | `6c70efe2e64dee7ff893ac892c38c2c994f03837` |
| Merge base | `d753087d88417ab5b77cf77369c1e38e22cee6b9` |
| Stale local `vg-vt-improvements` | `d006f20e91e4193d9b6e5a3dd2a165e2c3171143` |
| Final implementation measured by clear-ridge.10 | `46cb226692e96e0edf66beee440a6dd4ab51fb9c` |
| Final GI documentation measured by clear-ridge.10 | `7e65df70ee9d23de71cd2fb02783f03b60d40fcd` |
| Final clear-ridge.10 report/source head | `6c70efe2e64dee7ff893ac892c38c2c994f03837` |

There are **11 main-only and 107 VG-only commits**. VG has 92 commits after the
stale local head. The VG delta from the merge base is 2,567 files, 283,503
insertions and 4,308 deletions, including substantial text evidence and moved
content. The endpoint comparison with current main is 2,607 files, 283,499
insertions and 14,282 deletions. Endpoint deletions include files added only on
main; they are not instructions to remove those features.

Both `46cb22669` and `7e65df70e` are ancestors of the current VG head, and
neither is an ancestor of current main. Every path changed between the final
implementation and `6c70efe2e` is under `docs/`. The September 30 measurement
recommendation is therefore documentation over the same engine implementation.

The clear-ridge child source/delivery branch heads are all included in current
VG and absent from main. These are source ancestry facts, not proof of main
publication:

| Child | Full head SHA | Principal result |
|---|---|---|
| .1 | `c9f0a4566428b6f9a3069b5caee4f5395441d2b1` | POM-off baseline |
| .2 | `1b54a566fa60ca7c9e1bd48b2581c7351e7ea0e7` | G-buffer diagnostic split |
| .3 | `fc124643fd0772d22efcc5f5afc4852454a11800` | Bounded terrain-root paging audit |
| .4 | `8a7e72f2b830bd36c022d4d0217adcfa6fa6bfd2` | VT/geometry memory caps and bounded paging |
| .5 | `df8b673b9e61a422b973270a93423ab7a20778f2` | Static growth and upload pacing |
| .6 | `2f328bd646eb58d104658940ca1826a97890892c` | Resumable VT fill/AO slices |
| .7 | `794c029a2e5790cdfddd90f54e2cd08ab227154d` | Vertex cache ordering |
| .8 | `a21ffaa8b2f3b491f99dfee915cec565ab86a06d` | Persistent per-instance cuts and scene reuse |
| .9 | `7e65df70ee9d23de71cd2fb02783f03b60d40fcd` | GI lane specialization and late acceptance |
| .10 | `6c70efe2e64dee7ff893ac892c38c2c994f03837` | Final static/VG comparison and default recommendation |

## Population baseline, mesh detail and terrain detail

The [September 18 mountain pilot](../agent/evidence/2026-09-18-stream-mountain-geometry/README.md)
explicitly records **three independently generated rocks**, 110,592 triangles
each / 331,776 total, placed alongside ordinary mountain scatter. Its
[artifact hashes](../agent/evidence/2026-09-18-stream-mountain-geometry/artifact-hashes.json)
pin `mountain_geometry_site.js` to SHA-256
`e9dc9ca4abdc3150bac9b0e50d865537919138f39e4d51ff294201f6b65baebb`.
That count corresponds to cube resolution 96 (`12 * resolution²`). No separate
Git commit or exact earlier source text is available for that intermediate
authoring state; the old poses cannot be independently proved from the hash.

The [September 19 stress record](../agent/evidence/2026-09-19-geometry-stress/README.md)
records 1,283 placements / 19 assets at resolution 128. Its authoring was
committed in two mixed commits:

| Commit | Relevant changes |
|---|---|
| `912ad0fb350efb3c8df3ff129c3b5ee2b457826b` | Moves **and rewrites** the world/sector/props; hooks up `__geometryRocks`, replaces natural scatter/forest, adds continuous terrain material and relief, raises terrain source sampling. The title understates the content changes. |
| `4228d4c9d76e23ed6437160a3553e17101b4df1f` | Introduces `mountain_geometry_site.js`, dense rock generation, natural rock/forest modules and their tests with other world content. |

Current world, sector and props are byte-identical to their versions in
`912ad0fb3`. The stress-site module is byte-identical to `4228d4c9d`. Later
clear-ridge work did not increase this authored population.

| Inspection-site quantity | Pre-stress evidence | Current stress site | Population restoration target |
|---|---:|---:|---:|
| Placements | 3 | 1,283 | 3 |
| Unique assets | 3 | 19 | 3 |
| Cube resolution | 96 | 128 | **128 retained** |
| Triangles per asset | 110,592 | 196,608 | **196,608 retained** |
| Placed source triangles | 331,776 | 252,248,064 | 589,824 |
| Unique source triangles | 331,776 | 3,735,552 | 589,824 |

Placed source triangles are an authored complexity calculation, not a rendered
triangle count. The reduction target removes 1,280 instances (99.77% of the
stress-site population); it does not lower the detail of the retained rocks.
Resolution 96→128 is separately documented and should not be reverted for an
instance-density request.

The three current focal records give an exact, reproducible restoration target:

| X | Z | Rock seed | Shape | Size (m) | Resolution |
|---:|---:|---:|---:|---:|---:|
| 416 | 1456 | 0 | 0 | 6 | 128 |
| 425 | 1458 | 1 | 2 | 4 | 128 |
| 422 | 1448 | 2 | 1 | 7 | 128 |

Production material is the existing `Mountain.GeometryRock` handle. Preserve
`groundY = heightAt(x,z)`, burial `min(0.35, size*0.12)`, and instanced child
ownership with `inlineBelowPx:0`. Half-open XZ bounds and the per-rock half-open
Y owner retain exactly one instance across nested tile changes.

The added grid spans 32×32 cells at 8 m spacing from `(300,1320)`:

- 1,024 small rocks use up to ±2.5 m jitter, hash salt `0x71a3`, seeds 108–115,
  sizes 0.25–2 m in quarter-metre increments, and shapes `variantIndex % 3`.
- 256 large rocks occupy even/even grid coordinates with a `(3,3)` m offset,
  hash salt `0x4b21`, seeds 100–107, and sizes 3/4.5/6/7.5 m.
- The focal seeds are 0/1/2. All stress-site records are independent of
  `worldSeed`; reseeding the world changes height/terrain, not their XZ plan.

World seed remains **20260722**. The current camera remains
`(380,90,1600) → (420,55,1420)` and `terrainOnly:false`. Retain 128 terrain
texels/metre, direct `mountainSurface`, the 9/4/1.5/0.75 m ground-relief terms,
and `voxelRung = max(-5,min(3,terrainLod-2))` (0.25 m finest source sampling).
Those terrain changes are separate from rock instance density.

## Natural scatter and saved overrides

The original committed baseline `d753087d8` has **no inspection-site rocks**.
Its natural `Rock` planner uses 180 m landmark candidate spacing, 2.5/4 m
source sizes at ×10 instance scale, 8 small-rock seeds, and a per-cell RNG
seeded by world seed and cell coordinates. At near scatter rungs it attempts
`3 * biome.rocks` candidates: 48 per foothill/meadow cell, 12 per mountain cell.
Its world-space scree mask uses `worldSeed ^ 0xB62`, frequency 1/70 m,
threshold 0.2, with 18% acceptance outside the patches; surviving small rocks
scale 0.6–1.8. Old vegetation/rock gating stops below terrain LOD 3.

The new natural `MountainRock` population uses world-coordinate candidates and
stable cluster anchors: salts `0xB071/2/3/4`, spacing 180/96/9/3.8 m,
landmark sizes 16–36 m, boulders 2–7 m, scree 0.35–1.4 m, and four seeds per
shape across 0.5/2/8/32 m reference sizes (48 prototypes). It rejects oceans,
altitudes outside 0–520 m and slopes above 0.48 (landmarks above 0.32), fits
support-plane pitch/roll, and keeps forest footprints clear. New near scatter
gating starts at terrain LOD 2. `biomes().rocks` still reads 16/16/4 but the new
planner does not consume those counts, so lowering them would not remove the
stress field or thin this natural population.

The audit JSON includes deterministic old/current natural-scatter counts on
the same controlled 25-cell flat-habitat fixture, with forest and inspection
site suppressed. These distinguish the planners, not real terrain populations.
At terrain LOD 5 with world seed 20260722, the old/new counts are 1/22 at
scatter rung 0 and 470/206 at rungs 1 and 2. The natural planner rewrite is
therefore not a uniform population increase; the separate 1,280-rock stress
grid is the clear additive expansion.
The [earlier natural-rock evidence](../agent/evidence/2026-09-17-mountain-rocks/README.md)
and current existing tests preserve 1,689 natural placements on their separate
1 km² fixture with seed 872. Keep that planner/catalog/clearance behavior for
the targeted inspection-site reduction.

Saved props override authored rings/bands: scatter `159:2,624:1,20000:0`, terrain
`275:5,601:4,1009:3,1959:2,6014:1,20000:0`, hysteresis 512, max inflight 128.
`Rock/hide:true` is an old module-specific draw override; it does not hide
`MountainRock` or `MountainDetailRock`, and hiding instances is not population
restoration. Counts in this audit refer to authored placements, not visible,
resident or RT-scanned populations. Preserve the new saved POM-off/volumetric
settings rather than restoring the entire older props file.

## Branch improvements and main work to preserve

| Area | Implementation anchors and retained behavior |
|---|---|
| Geometry/terrain foundations | `adc37c2d9`, `4072dc776`: hierarchy/pages, indexed CPU/GPU cuts, binary caches, BLAS restoration and source-VT terrain proxies; `18d8a4b51` bounded static fallback, `5d597bcce` streamed roots, `419f98b01` bounded audit budgets. Retain ordinary source fallback and complete-parent coverage. |
| Memory admission | `0cb883ee2`, `d53508829`: account geometry consumers and cap VT/geometry reservations; bound paging under pressure. |
| Static buffers | `c3cdbcf6b`, `71bf972ff`, `5c4f2d46f`: incremental growth, smaller allocations/staging, bounded triangle upload across frames. |
| VT fill and AO | `06d8923a8`, `53e4d04ad`, `f10ae7019`: resumable row slices, combined producer/AO pacing, horizontal warp tiles; retain partial-page publication/lifetimes and coverage. |
| Vertex reuse | `4a1697e2d`: reorder within GPU draw ranges; preserve winding/attributes and mesh ownership. |
| Persistent geometry | `e51271afa`, `a621fe4a0`: per-instance cuts, unchanged RT scene reuse and unrelated-root publication filtering. |
| GI | `15753fbde`, `46cb22669`: separate diffuse/reflection timers, GI lane specialization and skip disabled secondary parallax work. Retain RT/GI settings and material evaluation. |
| Baseline/defaults | `3bafe0495`: POM defaults off; `1b54a566f`: optional G-buffer diagnostics. Production mode remains `pom_off`/full shading, with VG opt-in. |
| Reliability/build | Smart-torrent fixes: recover failed/re-requested pages and lost worker handoffs, restore terrain LOD fallback, VT queue/sink/tail retirement, cancellation budgets, lazy prepared-sector bank, descriptor reuse. Retain shader dependency and CTest GPU registration/timeout/readiness repairs. |
| Materials/content | `8cc8661fc`, `44fd458bf`, `c738c460b`, `4228d4c9d`: layered/finite/periodic materials, VT feedback/encoded cache/export, shared surface DSL, mountain surface/forest/rock content and tests. |
| Other capabilities | `4795606c4`: page/blob/fixed-bank libraries; `e9421a93f`: shared sparse voxel foliage; `c1582748b`: static asset export; `f4fdbc710`: object evaluation/capture. Keep these integrations and their registrations. |

Current main separately contains GI lightmap baking (`89282f8e9`, `f77f2e18c`)
and OBJ/MTL/PBR export/CLI (`9b4170785`, `554be8eef`, `bc035d6af`) plus the
Kreuzenstein project-tier fix (`135ac7cc7`) and repository hook configuration.
The prospective merge preserves their added source files, but reports eight
conflicting paths:

- `MatterEngine3/Makefile`
- `MatterEngine3/tests/world_definition_tests.cpp`
- `cmake/MatterEngine.cmake`
- `cmake/MatterViewer.cmake`
- `cmake/tests/viewer_graph_tests.cmake`
- `projects/world_demo/objects/KreuzensteinBrick.js`
- `projects/world_demo/scenes/castles/layouts/Kreuzenstein/README.md`
- `projects/world_demo/tests/kreuzenstein_scene_tests.mjs`

Resolve build lists/tests as the union of supported capabilities and reconcile
the new content layout with the main project-tier fix. A whole-file branch
overwrite would lose one side. This read-only merge-tree probe changed no
branch/worktree and is not a delivered merge.

VG is already opt-in through an exact `MATTER_GEOMETRY_PAGES=1` check in
`MatterEngine3/src/matter_engine.cpp`; absent/0 uses the source path. Keep that
behavior and explicit launchers. POM is off in `PomSettings` and saved mountain
props. The [final performance decision](hdgeo-performance-pass-2026-09-30.md)
supports the static default and leaves VG coverage/parity/performance gates
open; historical timings are not evidence for the restored population.

## Reproduction and verification

Run the [audit script](../agent/evidence/2026-09-30-vg-integration-audit/audit.mjs)
from any directory with Node 24 and the pinned objects available. It loads
source through `git show`, checks ancestry/byte identity, counts the site and
catalog, checks ownership, records control-fixture counts/hashes, inventories
all VG/main-only commits and computes the prospective merge conflicts. It
starts no C++ suite, editor or GPU workload. Material ID 7 in its focal records
is an explicit test sentinel, not a fixed production registry ID.

```sh
node docs/agent/evidence/2026-09-30-vg-integration-audit/audit.mjs
node projects/world_demo/tests/mountain_geometry_site_tests.mjs
node projects/world_demo/tests/mountain_rocks_tests.mjs
node projects/world_demo/tests/mountain_detail_rocks_tests.mjs
node projects/world_demo/tests/mountain_terrain_only_tests.mjs
git diff --check
```

The [machine-readable snapshot](../agent/evidence/2026-09-30-vg-integration-audit/audit.json)
contains the full provenance, source hashes, exact focal records, distribution,
counts and prospective conflicts. The existing four Node suites and audit pass
on this source snapshot. Native builds/captures are reserved for the dependent
implementation/delivery gates; none were run or claimed by this audit.
