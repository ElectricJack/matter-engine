# ConiferForest

Select **ConiferForest** in the editor. The initial stand contains 32 placements
(36 stems): Scots pine, silver fir, 62 m coast redwoods, and three-stem redwood
clumps. A raised sun makes the branches and bark visible. This scene is the
repeatable forest workload for improving foliage quality and instance cost.

The trees use the same detailed sources as [ConiferLab](../ConiferLab/README.md)
and [RedwoodGrove](../RedwoodGrove/README.md): six shared bough shapes per species,
individual needles as bake sources, near cones, baked foliage views, isosurface near wood,
and authored longitudinal strips farther away. No trees are replaced with
simple low-detail props for the benchmark.

The active target, clarified by the user, is **250,000 trees in 5 ms of
tree-rendering GPU time; GI may be off**. Complete frame time is reported
separately. The [active architecture plan](../../../../../docs/superpowers/plans/2026-09-13-sparse-voxel-forest.md)
defines sparse-voxel milestones and timing attribution, using these detailed
procedural sources and allowing improved textured source approximations. The current 32-placement scene does not
meet that target. Its timings are incremental engineering measurements, not
evidence of the requested population or performance. The nearest shipping
foliage representation should be textured geometry; individual needles are
source data, not a desired runtime detail level.

The sparse-voxel path now compiles the four cached tree types through 47 shared
prototypes and stages 250,000 GPU roots in an opt-in native diagnostic. GPU
selection plus primary visibility measured 1.99 / 4.70 / 5.13 ms for overhead,
elevated and ground views at 1600 x 1000 on RTX 4090. This uses a fixed coarse
level, with noisy close appearance and no voxel shadows. It does not establish
the final quality or 5 ms budget. This editor scene still uses the existing
foliage path; production publication, automatic level selection, textured close
foliage and shadows remain pending. See the plan for raw evidence and reproduction.

## Population and reuse

Edit `FOREST_DEFAULTS` in
[`shared-lib/conifer_forest.js`](../../../shared-lib/conifer_forest.js).

| Control | Default | Effect |
|---|---:|---|
| `count` | 32 | Tree placements; each clump contains three stems |
| `seed` | 20260913 | Reproducible rotation and position jitter |
| `spacing` | 8.5 m | Separation between spiral grid cells |

Increase count to 64, then 128 to extend the same stand outward. With an
unchanged seed and spacing, existing trees retain their positions, rotations
and species. Keeping that prefix stable makes comparisons useful. The authored
limit is 4,096 placements; this is an input bound, not a performance claim.

The four tree recipes form a shared kit. Whole-tree rotation and position vary;
root scale stays at one to retain physical needle, cone and trunk dimensions.
Change the kit recipes to vary tree height, branch counts, crown proportions
and fullness. Giving every tree a unique branch seed increases bake and asset
cost, so population measurements deliberately keep that variable fixed.

## Repeatable captures and measurements

Build from WSL with:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

From native Windows Python, run one case at a time:

```powershell
py -3 tools/forest_perf.py --out-dir C:/tmp/conifer-forest/run-01 --camera overview
py -3 tools/forest_perf.py --out-dir C:/tmp/conifer-forest/run-02 --camera ground
py -3 tools/forest_perf.py --out-dir C:/tmp/conifer-forest/run-03 --camera distant
```

The runner uses 1600 × 1000, uncapped presentation, the authored LOD ladder,
and the scene's saved lighting. It waits for uploads to settle, warms up for
eight seconds, then samples for twelve seconds. A cold bake can take several
minutes. Each fresh output directory retains `perf.json`, a CPU profile trace,
the editor log, a screenshot, and the executable/settings/source fingerprint.
Record the actual graphics device and reconstruction mode reported by the
editor; compare runs on the same device, camera and presentation settings.
Ray tracing is explicitly on by default (`--ray-tracing off` selects a
separate workload). The LOD pixel budget is fixed at 1 by default
(`--pixel-budget` overrides it). Both choices are recorded, so saved editor
preferences cannot silently change these benchmark settings.
Use `--validation` separately for functional checks because validation adds
overhead. `--hidden` is available for unattended measurements.

`qa.timeline` visits the wider editor overview, ground, redwood needles, bark,
shared root crown and distant cameras without exiting. Its final view matches
the scene's default overview and fits the redwood tops. The benchmark runner
retains its original fixed overview camera for comparison with earlier runs.

This is the existing Vulkan mesh/impostor renderer. It still expands trees into
many small leaf instances; a Nanite-style voxel hierarchy has not been added.
Use CPU frame phases and GPU pass timings together when deciding what to change.

## First iteration: publishing the forest

The default stand plans 2,085,163 instances. The initial native run did not reach
a settled frame measurement: twelve samples of the busy render thread all
landed in `WorldState::apply`. Each added instance scanned the growing manifest,
making bulk publication quadratic. The implementation now indexes bulk adds
and compacts removals in one pass, retaining first-match replacement, repeated
removal, re-addition, survivor order and version-counter behavior.

A native MSVC comparison on this workstation measured the following **CPU
publication costs**, not render frame times:

| Added instances | Sequential reference | Indexed update |
|---:|---:|---:|
| 20,000 | 166.2 ms | 3.4 ms |
| 40,000 | 707.4 ms | 6.0 ms |
| 2,085,163 | Not run | 690.1 ms |

Reproduce the behavior checks and optional timing with:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_state_delta_tests
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/world_state_delta_tests.exe --bench
```

The test checks mixed updates against the sequential contract, including
duplicate IDs, and verifies a 200,000-instance remove/re-add. Timing is reported
without a machine-dependent pass threshold. The full-forest reference was
aborted during loading, so there is no valid original steady-frame FPS baseline.

After fixing publication, two 12-second measurements at the fixed overview
camera on an RTX 4090 (1600 × 1000, Native, RT enabled, validation disabled)
gave the following initial comparison. The second run chooses LOD once per
sector/part, reserves the active output list, and aggregates GPU statistics per
workgroup. Both runs recorded zero static geometry or stable-instance uploads
during sampling; draw/triangle/cull counts matched.

| Measurement | First settled run | Second iteration |
|---|---:|---:|
| Sampled frames | 21 | 27 |
| Median end-to-end frame | 588.8 ms | 447.1 ms |
| p95 end-to-end frame | 614.0 ms | 472.3 ms |
| Median GPU cull | 189.1 ms | 175.4 ms |
| Median GPU total | 275.3 ms | 258.0 ms |

This is a 24% reduction in frame time on the same authored stand, and remains
far from an interactive large-forest budget. The CPU still visits a flat list
of millions of instances for selection, temporal history and texture demand;
GPU draw-bucket allocation and culling also remain expensive. The captures
retain these costs instead of reducing the tree count to hide them.

## Foliage and bark iteration

The forest exposed disappearing needles in reduced atlas levels. Foliage now
uses a separate, coverage-preserving mip chain for each baked view, including
at least one covered texel for a nonempty view. View-local filtering keeps
neighboring atlas views from bleeding together. This increases the allocated
128-slot impostor atlas from 1,024 MiB to about 1,365 MiB.

Fir and redwood foliage now have narrow blades with nearly parallel edges and
a lighter underside. Pine needles retain their ridged cross section. Thinner,
darker shoot axes reduce the brown contribution that previously overwhelmed
the redwood crowns. Species colors and dryness variation are baked into the
views. Trunk, branch and redwood bark have separate relief scales; per-plate
color is recorded before shape capture so it reaches the baked texture.

After those leaf-shape and color changes, the same fixed benchmark camera
measured 443.7 ms median / 463.9 ms p95 over 27 frames, with a 175.0 ms median
GPU cull pass and 256.6 ms median total GPU time. This run again recorded zero
static geometry or stable-instance uploads during sampling. The small change
from the second iteration is not evidence of an additional speedup.

The current stand remains a demanding visual/performance test. Native-resolution
foliage still aliases, pine/fir crowns look too pale at distance, and close bark
views reveal excessive pattern repetition. These are visible limitations, so
the current appearance should not be treated as finished realism. The next
rendering milestone is to reject and select detail for whole tree/branch groups
before expanding leaf instances; processing every leaf each frame will not
scale to a large forest.


## GPU residency and retained instance resolution

The next pass moves per-frame instance data, indirect commands and cull-generated
transforms into device-local memory. These arrays previously used host-cached
system memory on the test GPU. Uploads use persistent staging owned by each
frame slot, with transfer barriers and retained lifetimes. Animated skin and
water transform tails use that same asynchronous frame path. The small CPU
statistics readback remains mapped; opt-in full LOD traces use staged readback.

The flat instance cache now compares owned identity fields directly. It avoids
the serial FNV multiply for every byte in a multi-million-instance forest and
checks exact values, including every transform component and source order.
This adds an approximately 175 MiB identity snapshot for this stand.

The resolver retains the distinct part hashes per sector and reuses its output
array while world content, active sectors and selected rungs match. Camera
motion still recomputes distances and choices every frame. World edits, quality
changes, activation changes and rung transitions update the result. Parts with
inline child cutovers retain the full expansion path because their child
placements and rungs depend on additional data.

Native validation covers both in-flight buffer slots, unchanged and changed
uploads, culling parity and overflow, resource recovery, skin animation and
animated water. The resolver checks retained output against a fresh resolver
through camera movement, world replacement, edits, changed ladders, floor and
budget changes, and inline child edits.

The broader `viewer_logic_tests` run reported four failures in baked-root/tree
child-table expectations (`flat root carries LOD geometry`, `flat root has an
empty child table`, `LOD0 raster verts present`, and `compositional tree keeps
its child table`). Its resolver checks passed. These failures remain recorded
in `C:/tmp/conifer-forest/optimization-05-viewer-tests.log`; they are not counted
as a passing full-suite result.


At the original fixed benchmark camera on the same RTX 4090, 1600 × 1000,
Native resolution, RT enabled and validation disabled:

| Measurement | Before this pass (iteration 03) | Final (iteration 05) |
|---|---:|---:|
| Sampled frames | 27 | 60 |
| Median end-to-end frame | 443.7 ms | 200.6 ms |
| Median FPS | 2.25 | 4.98 |
| p95 end-to-end frame | 463.9 ms | 211.7 ms |
| Median GPU cull | 175.0 ms | 3.17 ms |
| Median total GPU | 256.6 ms | 60.5 ms |

The final frame time is 55% lower (2.21× throughput). Both runs processed the same
2,085,163 active instances and reported 55 batches and 27,556,580 raster
triangles at the capture point,
with the same frustum/occlusion counts. Both recorded zero static geometry or
stable-instance uploads and zero immediate submissions during sampling.
These comparisons use identical shaders, tree populations, needle sources and
authored LOD thresholds.

The final CPU trace still measures about 51 ms in temporal-frame construction,
30 ms copying temporal data, 36 ms preparing texture demand and 22 ms preparing
RT geometry per frame. These are the next large costs. The result is still
about five FPS, not an interactive large-forest renderer; hierarchical tree and
branch representation remains future work. Raw runs and captures are retained
under `C:/tmp/conifer-forest/iteration-03`, `iteration-04` and `iteration-05`.

The final visible review visited overview, ground, redwood needles, bark and
shared-base cameras. All five matched the earlier active-instance, batch,
triangle, frustum-cull and occlusion-cull counts, with zero Vulkan validation
errors. The editor was left at the overview. In that view, tracked device-local
allocations rose from 4,665 to 9,785 MiB, while host staging fell from 6,301 to
2,205 MiB. The faster GPU path therefore uses more VRAM. Review captures and
logs are under `C:/tmp/conifer-forest/review-05`.

## Bake-only needle sources

`ConiferNeedles` now authors `static lods = [{ at: 0, impostor: true }]`.
Its detailed `build()` mesh supplies the atlas but is absent from the runtime
flat, including at the nearest distance. Each spray uses two triangles and a
shared 48-view atlas with normal, depth, color and coverage; the existing
coverage-preserving mip chain handles minification. Wood and cone ladders are
unchanged.

The flattener accepts this form for a source leaf. It retains the source's
bounds, writes only the billboard BLAS into the flat, and publishes the atlas
before the flat. PartStore verifies the atlas against the canonical source in
temporary CPU storage at load time. The source is never registered in the
shared runtime BLAS or its renderer mesh streams on a successful flat load.
Disabled sole representations and sources with children produce named bake
failures; they do not publish successful empty flats. The existing loader's
compositional fallback still applies if an artifact is damaged or unavailable.
The representation version changes from 3 to 4 to invalidate previously
ignored single-impostor declarations.

This is a step toward the requested textured highest detail, not the final
forest renderer. A single-depth view atlas has limited disocclusion and view
coverage, especially when looking upward; close foliage still needs visual
refinement. The current RT builder excludes billboard rungs, so these sprays
also lack a traced shadow/GI occluder at close range; a suitable foliage shadow
proxy is still required. This is a material quality limitation, even when the
rest of the scene has ray tracing enabled. Runtime expansion still emits the
same millions of shoot instances.
The next structural work must retain tree/branch assemblies on the GPU and
select aggregate canopy representations before expanding their children.

Epic's [Nanite Assemblies documentation](https://dev.epicgames.com/documentation/unreal-engine/nanite-assemblies?lang=en-US)
describes reducing distant assemblies to a single cluster instead of paying
one runtime instance per branch. Its [Nanite Foliage documentation](https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage)
describes voxel clusters with 4×4×4 bricks and normal distributions for aggregate
foliage, rasterized in depth buckets. Those are relevant architectural references
for the remaining scaling work; this engine does not implement those systems yet.

A controlled A/B pair on the RTX 4090 uses the **same executable**, camera,
1600×1000 Native output, RT enabled (two trace dispatches), and pixel budget 1.
Only `ConiferNeedles`' LOD declaration changes: the reference uses
`[{at:0},{at:4.5,impostor:true}]`, while the new version uses the sole textured
rung. Both warm up for eight seconds and sample for twelve seconds, uncapped
and with validation disabled.

| Measurement | Reference, iteration 07 | Bake-only needles, iteration 08 |
|---|---:|---:|
| Sampled frames | 60 | 66 |
| Complete frame median | 199.00 ms | 178.10 ms |
| Complete frame p95 | 210.85 ms | 197.33 ms |
| Raw GPU total median | 63.52 ms | 54.18 ms |
| Raw GPU culling median | 3.20 ms | 3.19 ms |
| Raw GPU G-buffer median | 27.42 ms | 23.05 ms |
| CPU VT demand median, final 11 seconds | 35.42 ms | 10.55 ms |

The complete frame median improves about 10.5%, to 5.61 FPS, for **32 placements
and 36 stems**, not 250,000 trees. Both settled captures report exactly
2,085,163 active instances, 56 raster batches, 28,436,722 raster triangles,
6,182 frustum rejects and 5,714 occlusion rejects. Both runs have zero static
vertex/cluster/stable-instance upload deltas and zero immediate submissions
during sampling. The nearest foliage representation and its RT occlusion do
change as described above; matching this distant camera is not a claim of
identical image quality at every distance. Remaining temporal preparation and
copying costs about 83 ms on the CPU, and resolving the expanded manifest costs
about 31 ms.

Evidence is in `C:/tmp/conifer-forest/iteration-07-reference/` and
`iteration-08-baked/`: exact executable/source fingerprints, settings, raw
per-pass statistics, profile traces and screenshots. The adjacent
`bake-only-controlled-comparison.json` records the comparison and proves that
only the needle source changed. The original baked-only authoring was restored
before the second run and remains the current source.

The runner now pins both ray tracing and the LOD pixel budget. Earlier runs
05/06 did not pin the latter, so their provisional 11% comparison is superseded
by this controlled pair. The first five-view functional review inherited ray
tracing off; a second five-view review in `review-06/` enabled it. Both captured
all views with no Vulkan validation errors or rejected atlases. `review-08/`
leaves the final editor at the overview with RT enabled and pixel budget 1.

Native checks passed: `conifer_lod_provider_tests` (including the production
PartStore and no-source-runtime-mesh assertion), `script_host_tests`, and
`part_flatten_tests`. The latter now has an MSVC CMake target and uses the CRT
allocator for its mesh fixture, so it does not require raylib memory symbols.
When invoking it directly through WSL, run from the executable directory so its
self-spawn determinism check resolves the correct binary. Conifer authoring
also passes with `node --experimental-vm-modules projects/world_demo/tests/conifer_tests.mjs`.
