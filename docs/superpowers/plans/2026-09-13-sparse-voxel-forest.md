# Sparse voxel geometry and the 250,000-tree forest

Status: active implementation; goal unachieved. Updated 2026-09-13.

Current priority: the user rejected the lit forest as a "fuzzy mess" and
clarified: "Its supposed to look the same as the model if it were instanced
as triangles." This is the acceptance criterion. A fast image that loses the
source tree's silhouette, branch structure, gaps, coverage or solid wood is a
failed result. Make matched whole-tree triangle-reference comparisons before
further forest performance tuning. GPU hierarchy traversal is connected to
the compiled tree graph, but its current voxel representations are rejected.
Compact textured near geometry, forest-scale shadow integration and editor
integration remain open. The latest checkpoint adds native sparse sun shadows
and matched single-tree comparisons against opaque triangle shadows.

## Objective and accepted direction

Render a forest of 250,000 procedural evergreen tree placements within **5 ms
of tree-rendering GPU time**, with GI permitted to be disabled. Preserve
convincing close foliage and automatic detail selection. Individual modeled needles are optional bake references, never a
required runtime representation. This is an architectural change to how dense
geometry is compiled, selected, streamed, and rendered.

The user approved improving the source approximation: textured quads or small
three-dimensional shoot clusters may replace excessively fine needle meshes.
Preserve visual fullness, plausible branch structure, and useful shadow coverage;
literal subpixel needle geometry is not a fidelity requirement. Keep detailed
wood, cones, pine/fir/redwood variation, and shared-base trees. Existing biological
counts and source appearance are not yet validated.

Sparse voxels are the preferred aggregate distance representation. Do not make
whole-tree impostors an intermediate dependency or continue optimizing the
millions-of-shoots execution model as the main route to the goal. Existing
impostors may remain for comparison while the new path is built.

The latest user clarification supersedes the original complete-frame wording:
**5 ms covers the portion that renders trees, not the complete frame; GI need
not be enabled.** Record dedicated tree pass timings and matched shared-pass
costs, with frame totals as context. This is a user-authorized metric change.

## Evidence and present limits

The last controlled pair used an RTX 4090, 1600 x 1000 Native output, RT enabled,
pixel budget 1, uncapped presentation, and validation disabled. The 32-placement
forest (36 stems) measured 199.00 -> 178.10 ms median complete frame time after
making needles bake-only. Both captures reported 2,085,163 active instances.
Raw GPU time was about 54 ms; GI alone about 17 ms. This does not establish any
part of the 250,000-placement / 5 ms tree-rendering endpoint. These historical
whole-frame measurements are not the newly clarified acceptance metric.

Evidence: `C:/tmp/conifer-forest/iteration-07-reference/`,
`iteration-08-baked/`, and `bake-only-controlled-comparison.json`.
The [scene notes](../../../projects/world_demo/scenes/vegetation/ConiferForest/README.md)
record previous changes, measurements, and outstanding visual defects.

The old path would extrapolate to roughly 16 billion small instances. It must
be replaced, not merely made cheaper per instance. Foliage currently lacks
traced shadows because its billboard rungs are excluded from RT geometry.
The broader viewer logic suite also has four previously recorded failures;
passing focused tests must not be reported as a clean full-suite result.

## Runtime architecture

1. **Source compiler.** Retain procedural part references and transforms. Load
   each unique source once. Bake source geometry or textured surfaces through
   the hierarchy without merging all placed triangles. Alpha coverage must
   participate in voxelization; a transparent card must not become a solid slab.
2. **Generic representation hierarchy.** A node has bounds, geometric/coverage
   error, residency information, and either shared mesh clusters, sparse voxel
   bricks, or child references. Dense disconnected surfaces are voxel candidates;
   smooth walls and other economical surfaces can remain triangles. This is a
   part representation, not a tree-specific renderer.
3. **Sparse bricks.** Start with 4 x 4 x 4 cells per brick, occupied-cell masks,
   compact attributes, and empty-space omission. Preserve surface coverage,
   material color, orientation statistics, and transmission. Do not treat every
   touched cell as an opaque cube or average opposing normals into zero. Brick
   size and encoding remain measured implementation choices, not promises.
4. **GPU-resident placement and selection.** Upload the 250,000 root placements
   and shared prototypes. Traverse coarse spatial bounds and representation
   nodes on the GPU. Apply frustum/occlusion tests before descending. Instantiate
   branches only when selected detail needs them, and emit indirect draw work
   without CPU readback. Preserve stable object identities for editing.
5. **Hybrid rendering.** Rasterize close meshes and textured foliage clusters;
   use a dedicated voxel-brick GPU path for aggregate geometry. Both write the
   same depth/material buffers. Order work for useful early depth rejection.
   Use the engine's canonical LOD distance/error convention; do not introduce
   a conflicting projected-size selector in another subsystem.
6. **Lighting.** Sparse coverage/transmission must contribute to shadows and GI.
   Avoid per-needle BLAS/TLAS entries and opaque-rectangle card shadows. Share
   generic surface evaluation between primary visibility, shadow queries, and
   the source baker. GI may be off for the performance metric. Tree-attributable
   shadow work must be measured and reported; unrelated scene lighting is not
   charged wholesale to the tree-rendering budget.
7. **Streaming and motion.** Keep coarse parents resident until selected children
   are available. Bound queues and memory, with visible parent fallback on
   overflow. Keep static buffers resident and process scene deltas. Add branch
   motion with conservative bounds and distance-dependent animation only after
   the static architecture is measured.

## Execution milestones

### A. Coherent source and first sparse-voxel slice

- [x] Retire the interrupted, unused whole-tree traced-impostor extension.
      Its unused tracer additions were also removed; patches are retained under
      `C:/tmp/conifer-forest/retired-traced-atlas*.patch`.
- [ ] Establish a simpler textured shoot source/reference with stable coverage.
      Keep authoring controls for density, width, color, and silhouette.
- [x] Implement the CPU sparse brick builder, fractional surface sampling,
      orientation moments, and area-preserving parent reduction.
- [x] Pack GPU bricks/cells and shared root instances, retaining coverage and
      normal moments with bounded, validated uploads.
- [x] Compile the shared source hierarchy bottom-up from unique prototypes,
      resampling child cells without expanding placed source triangles.
- [ ] Connect real texture decoding/filtering and complete material attributes
      to that builder.
- [x] Display the actual brick representation through a dedicated GPU path.
      Native GBuffer fixtures cover real source shoots and generic ornament;
      world-provider publication and editor scene authoring remain pending.
- [ ] Orbit a branch, vary distance and illumination, and compare coverage,
      silhouette, shading, and shadowing against the source reference.
- [x] Exercise the same compiler/renderer on a non-foliage dense ornament.
      No foliage-name checks in the generic renderer; quality acceptance remains
      pending alongside foliage quality.

### B. Population and GPU hierarchy proof

- [x] Stage 250,000 GPU roots in the native population probe, using sparse
      aggregates compiled from the four cached tree types.
- [ ] Publish the population through production world authoring/provider paths.
      The editor ConiferForest scene still contains its original 32 placements.
- [x] Frustum-cull whole roots on GPU before placed-brick expansion.
- [x] Select a resident root aggregate level on GPU using the canonical
      normalized switch-distance rule, with stable identity and no CPU readback.
- [ ] Traverse spatial/representation hierarchies and select detail before
      branch or shoot expansion.
- [x] Keep prototype/root buffers on GPU and emit bounded indirect draws.
      Selection and command buffers are isolated per in-flight frame slot.
- [ ] Measure overview and moving-camera paths at the full population, including
      CPU frame intervals, every GPU pass, memory, queue occupancy, and uploads.
      Initial coarse-only appearance is a temporary scaling proof, not success.

### C. Close detail, transitions, and lighting

- [ ] Connect branch-level descent, textured near foliage, and detailed wood.
- [ ] Add coverage-preserving transitions and residency fallback without holes.
- [ ] Integrate foliage shadows/transmission and aggregate GI representation.
- [ ] Resolve close-up aliasing, pale/sparse crowns, bark repetition, underside
      views, and multi-stem appearance through native editor captures.
- [ ] Add bounded wind after static quality and performance evidence exists.

### D. Tree-rendering budget and final acceptance

- [ ] Instrument tree selection/culling, hierarchy traversal, mesh/voxel primary
      visibility, and tree-specific shading/shadow work. Sum only non-overlapping
      GPU intervals; do not mistake one cull pass for all tree rendering.
- [ ] Use matched trees-on/trees-off captures and GPU measurements to attribute
      costs in shared passes. Report those deltas separately from dedicated pass
      timings to avoid double counting. State exactly what the 5 ms total includes.
- [ ] GI may be disabled for acceptance. Retain useful direct lighting and foliage
      shadows for the visual proof. Report full-frame CPU/GPU time separately;
      the user explicitly clarified that full frame time need not be <= 5 ms.
- [ ] Verify all 250,000 placements exist and participate in the spatial and
      representation hierarchy. Ordinary visibility culling is expected; reducing
      the authored population or hiding the forest with a short range is not.
- [ ] Record hardware, resolution/reconstruction, lighting, animation, camera,
      warm-up, sampling duration, executable/source fingerprints, and raw traces.
      Historical baseline is RTX 4090, 1600 x 1000 Native, pixel budget 1, RT on.
      Establish the new tree-only baseline with GI off and explicit shadow settings.
      Quality-setting changes require explicit reporting and matched comparisons.
- [ ] Demonstrate <= 5 ms tree-rendering performance on representative overview,
      ground, flythrough, transition, and occlusion-heavy routes. Report median,
      p95, stalls, and streaming separately; a lucky single frame is insufficient.
- [ ] Verify visual quality with close/middle/far captures, camera motion,
      changing light, foliage shadows, all species, and shared-base trees.
- [ ] Run native MSVC builds, appropriate representation/renderer tests, and
      Vulkan validation. Inspect artifacts and captures, not just pass counters.

Completion requires the population, tree-rendering time, automatic LOD, and visual
requirements together. None is currently established. If the tree-rendering target
is contradicted by measurements, document the limiting pass and pursue the next
architectural change; do not silently lower the objective.

## Design references and confidence

Epic documents choosing voxel clusters when they preserve dense geometry more
accurately than triangle simplification, with a separate raster path and normal
distributions: [Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage).
Shared assembly hierarchies avoid paying for every distant branch instance:
[Nanite Assemblies](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-assemblies).

These establish the approach's plausibility, not performance evidence for this
engine. Exact demo parity and the requested tree-rendering budget remain
unproven. Generic applicability and avoiding flat-instance expansion are the
stronger premises; appearance, transition stability, lighting, and total cost
must be demonstrated by the implementation.


## First compiler evidence (2026-09-13)

`MatterEngine3/src/sparse_voxel_bake.{h,cpp}` now builds 4 x 4 x 4 sparse
bricks from streamed triangles. It clips surface patches to cells, interpolates
source UVs, and accepts a footprint-filtered coverage/color callback. Occupied
cells hold double-precision surface-area integrals, color, first normal moments,
and symmetric second normal moments. Parent reduction combines those integrals
without replacing fractional coverage with solid occupancy. This is an offline
intermediate format, not the final packed GPU representation.

Native MSVC `sparse_voxel_bake_tests` passed with zero failures. Cases cover
transparent and fractional-alpha quads, grid/triangle seams, negative brick
coordinates, exact area of a non-foliage octahedron, opposing normal distributions,
parent reduction, determinism, and bounded failure without partial publication.
The native editor also builds successfully with the component in the canonical
core manifest. The source-count guard in `MatterViewer.cmake` accounts for it.

Three real canonical source leaves from the current forest were probed at 4 mm:

| Source hash | Source triangles | Occupied cells at 4 / 8 / 16 / 32 mm | Initial CPU bake |
|---|---:|---|---:|
| `38ab71c47df3e572` | 10,600 | 2,924 / 744 / 198 / 59 | 41.4 ms |
| `9f9af205b5d50909` | 10,600 | 2,915 / 776 / 198 / 57 | 43.9 ms |
| `599a1608c46736e3` | 10,600 | 2,903 / 745 / 196 / 56 | 41.2 ms |

Integrated surface area remains unchanged across seven levels in each probe.
These are **offline bake measurements**, not rendering timings or visual proofs.
The probe reads REP0 source geometry, rejects hierarchy roots, and does not
resolve runtime material textures. At that checkpoint, source-detail textures, useful direct foliage shadows, the
dedicated GPU voxel path, the forest hierarchy and 250,000 placements were still
pending. The GPU visibility slice below supersedes that renderer status. There
is no new forest performance result from the compiler measurements.

Reproduce from the repository root after the native build:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target sparse_voxel_bake_tests
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/sparse_voxel_bake_tests.exe
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/sparse_voxel_bake_tests.exe --part C:/tmp/conifer-cache/projects/c2bf6fbff660b3fd/ConiferForest 38ab71c47df3e572 0.004
```

Logs: `C:/tmp/conifer-forest/sparse-voxel-bake-tests.log`,
`sparse-voxel-probe-build.log`, `sparse-voxel-editor-build.log`, and
`sparse-source-<hash>.log`. The GPU visibility implementation follows below.


## First GPU visibility slice (2026-09-13)

`render/vk_sparse_voxel.{h,cpp}` and `shaders_vk/sparse_voxel*` now implement a
dedicated graphics pipeline inside the existing GBuffer pass. Each occupied
4-cubed brick emits a conservative screen rectangle. Fragments reconstruct a
camera ray, traverse occupied cells, sample coverage-weighted optical depth,
and write the sampled hit depth, linear color, surface normal, roughness and
material/instance identity. Empty cells are skipped. This is actual volumetric
visibility, not baked view selection or visible cube faces.

`VkSceneRenderer::set_sparse_voxels` accepts explicit prototype batches and
root transforms. A batch uploads its brick/cell payload once, then GPU indexing
maps each root/brick pair without uploading a placed-brick record. Snapshots are
immutable and retained by submitted frames, including device-loss lifetime
handling. Publication can upload and create a pipeline; per-frame recording
only binds and draws. This is a first rendering slice, **not** GPU hierarchy
selection or a streaming-ready asset pipeline. No world/provider call currently
publishes these batches into ConiferForest.

The initial GPU ABI uses 32-byte bricks, 64-byte occupied cells and 160-byte root
instances. It preserves first/second normal moments, rejects nonphysical moment
covariance, invalid transforms and buffer limits, and leaves the previous
snapshot intact after a failed publication. Cell payload compression remains
pending. Stable per-frame movement/reprojection remains pending as well.

Native MSVC `vulkan_smoke_tests` mode `sparse-voxel` passed on RTX 4090 with
**zero Vulkan validation errors**. It exercised:

- Fractional source coverage and an alpha-empty half-card inside brick bounds.
- Reversed-Z occlusion in both directions between mesh and sampled voxel hits.
- Mirrored, nonuniform instance transforms; camera inside an occupied brick.
- Empty/failed snapshot publication, valid material identity and finite normals.
- A generic ring of faceted beads from two views (4,489 and 3,918 visible pixels).
- Canonical source `38ab71c47df3e572`: 10,600 source triangles, rendered at
  4/8/16 mm spacing from four rotations. All three levels produced visible hits.

The native editor builds with this component. The existing `raster` Vulkan
regression mode also passes with zero validation errors; log:
`C:/tmp/conifer-forest/sparse-voxel-raster-regression.log`. The CPU bake suite
passes with zero failures. The focused GPU mode is registered as the CTest
`sparse_voxel_gpu_tests` test. Captures under
`C:/tmp/conifer-forest/sparse-gpu/` were inspected: `sparse-source-orbit-0.png`,
`-2.png`, `sparse-mesh-occlusion.png` and `sparse-ornament-1.png` show the expected
geometry/occlusion, but also substantial stochastic noise and coarser apparent
coverage. They are **sRGB-encoded albedo-buffer diagnostics**, not final shaded
forest-quality comparisons. The source probe supplies source tint, not resolved
runtime material textures. No performance target is established by these tests.

The visibility model currently uses Poisson optical depth and RMS projection
from second normal moments. RMS projection is an approximation to the mean
absolute projected area. It is not an exact general foliage visibility model;
the noisy and enlarged coarse appearance requires matched coverage fitting.
Normals use a moment-based facing lobe. Velocity is zero and reactivity is one
until temporal reprojection is connected. Voxel shadows, GI participation,
material transmission, automatic LOD and wind are not implemented.

Reproduce the actual-source diagnostic (native PowerShell, repository root):

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target sparse_voxel_bake_tests
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/sparse_voxel_bake_tests.exe --part C:/tmp/conifer-cache/projects/c2bf6fbff660b3fd/ConiferForest 38ab71c47df3e572 0.004 C:/tmp/conifer-forest/sparse-needle.fixture
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
$env:MATTER_VK_SMOKE_MODE = 'sparse-voxel'
$env:MATTER_SPARSE_VOXEL_FIXTURE = 'C:/tmp/conifer-forest/sparse-needle.fixture'
$env:MATTER_SPARSE_VOXEL_CAPTURE_DIR = 'C:/tmp/conifer-forest/sparse-gpu'
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

The fixture path is optional; without it, the analytical and ornament tests
still run. Its text format is test interchange only, not a runtime cache ABI.
Logs: `sparse-voxel-gpu-final-smoke.log`, `sparse-voxel-gpu-final-build3.log`,
`sparse-voxel-gpu-editor-build.log`, `sparse-needle-export.log`, and
`sparse-voxel-bake-final-tests.log`, all under `C:/tmp/conifer-forest/`.

Next: compile shared source prototypes and tree aggregate rungs; publish the
actual 250,000 roots; select/cull the shared representation hierarchy on the GPU
before expansion. In parallel with that architectural work, replace the finest
shoot source with a stable textured approximation and fit aggregate coverage
against matching views. Keep the 5 ms **tree-rendering** budget, GI permitted off,
and direct-shadow/quality acceptance explicit throughout.


## Shared source compilation and 250,000-root probe (2026-09-13)

The generic `sparse_voxel_hierarchy` compiler loads each unique source node once,
keeps child references, computes prototype bounds, and builds seven aggregate
levels. Parent construction resamples child cell integrals with bounded spatial
quadrature instead of replaying each placed triangle. Area, color and orientation
moments are conserved under translation, rotation/reflection and uniform scaling.
Nonuniform stretch is explicitly unsupported in this source aggregation stage;
it requires richer directional statistics. The GPU instance path supports affine
transforms and uses conservative transformed bounds for root culling.

Cycles, unavailable sources, invalid transforms, overflow and work/storage limits
fail without replacing the published hierarchy. The native hierarchy tests check
shared loading, independent analytical transformed-area/normal results, mirrored
winding, all parent levels, budgets and atomic failure. `Builder::finish` also
validates accumulated data before publication to reject transformed overflow.

The supplied ConiferForest resolve snapshot contained 2,085,163 old manifest
instances. Its four matching source roots compiled in **16.7 seconds**:

| Root | Type | Logical source triangles if fully expanded |
|---|---|---:|
| `5db299eef3be6842` | Pine | 290,770,800 |
| `c5aa59911908fa20` | Fir | 933,204,684 |
| `f751207e09c319e8` | Redwood | 612,180,488 |
| `651e6ea54ea7518c` | Shared-base redwood clump | 1,657,883,836 |

Only **4,192,972 unique source triangles** were processed across **47 prototypes**
and **354,370 shared child links**, yielding **664,803 stored cells** across the
prototype levels after 33,607,288 cell operations. The 3.494-billion-triangle
expanded forest kit was never materialized. Source tint is retained, but runtime
material texture resolution remains pending. These are measurements of the
explicitly supplied cached snapshot, not a check of today's authored cache key.

The GPU path now frustum-tests each root's conservative world bounds in compute,
compacts visible root indices, and writes indirect command counts. Each batch
still has an explicitly selected level. There is no CPU visible-instance readback
or per-frame placement upload in production recording. Three isolated frame
slots retain their own visible-index and indirect buffers. The scene retains
all snapshot resources until its submitted frames retire. Focused GPU tests
verify offscreen and behind-camera root rejection before brick expansion.

A native diagnostic stages **250,000 root placements** on a deterministic
500 x 500 grid with positional jitter and random yaw. It reuses the four real
compiled tree types at **fixed level 4** and draws through the existing GBuffer.
The overhead view's GPU indirect counts verify that all 250,000 roots participate.
The component's buffers occupy **51,015,936 bytes (48.65 MiB)**; this excludes
shared renderer reservations and render targets. The final measured one-time snapshot
upload/pipeline setup was 106.9 ms.

Measured on RTX 4090, driver 610.74, native MSVC RelWithDebInfo, **1600 x 1000**,
Vulkan validation enabled. Each stationary camera uses two warm-up frames and
12 recorded samples. GI is absent in this legacy diagnostic render path;
**voxel shadows are not implemented**. Four dedicated GPU timestamps bracket
root selection and voxel primary visibility. The table sums only those two
non-overlapping intervals; visibility is also nested inside the existing GBuffer
zone and must not be added to that zone again.

| Diagnostic view | GPU-selected roots | Selection median | Voxel visibility median | Combined median | Combined p95 |
|---|---:|---:|---:|---:|---:|
| Overhead | 250,000 | 0.131 ms | 1.858 ms | 1.989 ms | 1.994 ms |
| Elevated | 74,418 | 0.044 ms | 4.657 ms | 4.703 ms | 4.718 ms |
| Ground | 56,354 | 0.039 ms | 5.097 ms | 5.134 ms | 5.230 ms |

The native run passed with **zero Vulkan validation errors**. Captures in
`C:/tmp/conifer-forest/sparse-forest-250k/` were inspected, especially
`sparse-forest-0.png` and `sparse-forest-2.png`. They are albedo diagnostics.
Overhead shows the full regular test population; ground shows coarse, enlarged,
noisy voxel forms. White bark/source tints are unresolved. This proves the root
population and visibility path can run at this scale, **not** the final appearance
or the full 5 ms tree-rendering acceptance metric. Shared lighting/shadow costs,
close foliage, automatic LOD, motion, production world integration and natural
forest placement remain unverified or incomplete. No full-frame result is claimed.

Reproduce the offline source compilation and native population test:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target sparse_voxel_hierarchy_tests
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/sparse_voxel_hierarchy_tests.exe
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/sparse_voxel_hierarchy_tests.exe --cached-world C:/tmp/conifer-cache/projects/c2bf6fbff660b3fd/ConiferForest ConiferForest 'ConiferTree,ConiferClump' C:/tmp/conifer-forest/tree-voxels
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
$env:MATTER_VK_SMOKE_MODE = 'sparse-voxel'
$env:MATTER_SPARSE_VOXEL_FOREST_DIR = 'C:/tmp/conifer-forest/tree-voxels'
$env:MATTER_SPARSE_VOXEL_CAPTURE_DIR = 'C:/tmp/conifer-forest/sparse-forest-250k'
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

The population probe is opt-in; ordinary CTest sparse-voxel runs do not allocate
250,000 roots or depend on an external tree cache. Exported `.fixture` files
remain test interchange, not runtime bundle sections. Raw logs are under
`C:/tmp/conifer-forest/`: `sparse-tree-compile.log`, `sparse-hierarchy-tests.log`,
`sparse-pine-gpu.log`, `sparse-root-cull-smoke.log`,
`sparse-forest-250k-validation.log`, `sparse-root-raster-regression.log`, and
`sparse-hierarchy-editor-build.log`.

Next implementation priority: GPU level selection using the canonical normalized
switch-distance contract, finer source representation near the camera, and
production publication of shared prototypes/root placements. Preserve the
coarse parent during unavailable detail; then add spatial/child descent without
returning to per-frame expansion of hundreds of thousands of source shoots.


Final verification uses `sparse-forest-250k-final.log` and captures in
`C:/tmp/conifer-forest/sparse-forest-250k-final/`. The table above records that
run; the first run measured 1.987 / 4.695 / 5.207 ms. This variation is not
claimed as an optimization. `sparse-forest-evidence.json` preserves individual
samples and SHA-256 fingerprints of the tested executable, editor, implementation
sources and four input aggregates. The native editor final build and CPU
hierarchy/bake tests pass. The goal remains active and unachieved.

## Screenshot checkpoint (2026-09-13)

At the user's request, captured fresh native GPU views under
`C:/tmp/conifer-forest/progress-20260913/`. Optional source previews now render
at 1200 x 900; analytical tests retain their calibrated 320 x 240 extent.
The forest probe remains at 1600 x 1000 with the same 250,000 placements.

- `pine-and-forest/sparse-source-orbit-0.png`: pine aggregate levels 0, 1,
  and 2 from left to right. Four rotations are available.
- `shared-base-redwoods/sparse-source-orbit-2.png`: shared-base redwoods at
  those three aggregate levels, again with four rotations available.
- `pine-and-forest/sparse-forest-0.png`, `-1.png`, and `-2.png`: overhead,
  elevated, and ground views of the population using fixed level 4.

These are direct, sRGB-encoded albedo readbacks from the new renderer. They
show enlarged and overly solid coarse crowns, stochastic noise, unresolved
white wood/material tints, and regular test placement. The ground view is
visibly unsuitable at this fixed coarse level. No final shaded editor quality
or automatic LOD is claimed. These images establish a visual baseline for the
next LOD, coverage, material, and close-source changes; future progress reports
should include matching captures alongside timings.

The native MSVC smoke target rebuilt successfully. Both capture runs passed
with zero Vulkan validation errors. Logs are
`C:/tmp/conifer-forest/progress-20260913-pine-and-forest.log` and
`C:/tmp/conifer-forest/progress-20260913-shared-base-redwoods.log`.
GPU LOD selection remains the next implementation step; the goal is active.

## Automatic root LOD and bounded brick projection (2026-09-13)

The GPU now chooses one of up to 32 resident levels for each visible root.
`SparseVoxelBatch` keeps its original fixed-detail interface and adds a finest
switch distance plus ordered coarser assets/distances. The forest diagnostic
uses all seven compiled levels and normalized limits 16/32/64/128/256/512/infinity.
Selection follows `lod_distance.h`: finest bounds radius times average instance
column scale times the pixel-budget dial. The all-level occupied-bounds union
is separate and used only for conservative frustum culling. Different level
origins and spacings are supported in the same object coordinate system.

Placements remain immutable and GPU-resident. Each frame has isolated indirect
commands and compacted root indices for each level. A root emits exactly one
level; no placed brick records or source triangles are uploaded per frame.
The root ABI is now 48 bytes; the resident level table uses 32 bytes per entry.
The four prototypes, seven levels, 250,000 placements and three selection slots
occupy **92,975,584 bytes (88.67 MiB)**, excluding the renderer's shared targets
and static reservations. Initial upload in the final run took 188.663 ms.

The first automatic-LOD ground run took 191.85 ms. Inspection exposed waste in
the initial proxy implementation: wholly behind-camera bricks and many bricks
straddling the near plane produced full-screen rectangles. The final vertex
path clips occupied-cell bounds against the near plane, projects their corners
and edge intersections, and emits a conservative nearest depth. The fragment
shader's `depth_less` declaration allows early rejection under reversed-Z.
Packed occupied-cell bounds also omit empty corners of coarse brick boxes.

That last change exposed a correctness defect in the original ray marcher:
`t += 1e-5` can round to no change at large ray distances, leaving a ray stuck
on a cell boundary. Traversal now advances integer cell indices and all tied
crossings, without a fixed epsilon on ray distance. A new native fixture puts
an opaque back plate behind a thin transparent front layer and empty cells.
The same projected interior is checked from 2 m and 200 m. Restoring the old
step caused that regression to fail; the final implementation passes. Log:
`C:/tmp/conifer-forest/sparse-dda-red.log`. Earlier captures/timings that used
the broken traversal are superseded and cannot establish equivalent quality.

Final native MSVC measurements on RTX 4090, validation enabled, 1600 x 1000,
16 warmup frames then 12 samples per route, GI off and shadows absent:

| View | Selected roots | Selected brick instances | Selection + visibility median | p95 |
|---|---:|---:|---:|---:|
| Overhead | 250,000 | 450,114 | 2.299 ms | 2.375 ms |
| Elevated | 74,753 | 2,631,741 | 7.614 ms | 7.895 ms |
| Ground | 56,776 | 2,943,045 | 4.031 ms | 4.276 ms |
| Moving ground, 12 positions | 57,027 at final position | 2,932,301 at final position | 3.978 ms | 4.242 ms |

The moving route advances 5 m per sample over 55 m, with captures at samples
0, 5 and 11. It exercises camera-dependent selection; it is not a temporal
quality or frame-pacing acceptance test. The fixed-level-4 comparison on the
same executable/placements measured 0.825/1.840/0.695/1.313 ms for the four
routes, but its close geometry is visibly unsuitable. This is a quality/cost
comparison, not a claim that automatic LOD is cheaper than fixed coarse detail.

Both population modes and the analytical suite passed with zero Vulkan
validation errors. The focused tests include canonical distance/dial selection,
camera movement, scaled/mirrored roots, rebased level grids, atomic rejection
of invalid level tables, unchanged editing identity, and actual hit depth.

Final captures: `C:/tmp/conifer-forest/sparse-forest-auto-final/` and
`C:/tmp/conifer-forest/sparse-forest-fixed-final/`. Logs have the same folder
names plus `.log`. Set `MATTER_SPARSE_VOXEL_LOD=auto` with the existing forest
directory and capture-directory variables to reproduce the automatic mode;
omit it or set `fixed4` for the fixed comparison. Source previews still use
`MATTER_SPARSE_VOXEL_FIXTURE` independently.

**The full goal remains unachieved.** These are albedo diagnostics, not final
shaded trees. Elevated visibility alone exceeds 5 ms; attributable shadow and
lighting costs are not included. Material/texture resolution, textured close
shoot geometry, coverage fitting, temporal stability, branch-level descent,
spatial/occlusion selection, streaming fallback and production world/provider
publication remain pending. The editor scene still has 32 placements.

Next: connect actual source materials and a textured close-shoot approximation
to address the white wood, noise and blocky crowns visible in the captures.
For scale, add camera-dependent work ordering and spatial/occlusion rejection
before brick expansion, with the elevated route as the current expensive case.
Preserve the same views and report quality alongside the full tree-pass budget.


## User visual rejection and matched source gate (2026-09-13)

The user reports that the voxel approximations do not resemble the original
models and look like fuzzy ghosts. This is a representation failure, not a
lighting adjustment. The current Poisson volume model makes even coherent opaque
wood probabilistically transparent, while resampling spreads surface coverage
through cells and loses needle/shoot structure. Temporal filtering alone cannot
recover missing coverage or fix an enlarged silhouette.

Added a native comparison of actual canonical source triangles with their voxel
bake at identical pose, camera, resolution and unlit tint. The CPU source probe
now also exports a `.triangles` diagnostic sidecar, retaining positions, UVs,
source tint and coverage. The native GPU fixture imports it without pulling the
entire cache/BLAS loader into the renderer test target. These sidecars are test
interchange, not runtime assets. Only a single source leaf is expanded.

Measured at 1200 x 900, angle 0, at the finest tested spacing:

| Source | Source triangles | Spacing | Source-covered pixels | Missing source pixels | Pixels outside source coverage |
|---|---:|---:|---:|---:|---:|
| Pine wood `944607a0ee80db99` | 111,464 | 110.784 mm | 8,004 | 1,611 (20.1%) | 2,161 |
| Pine shoot `7e2c36734ef99b15` | 5,800 | 2.254 mm | 65,384 | 11,249 (17.2%) | 32,333 |

These are single-sample image-space diagnostics, not a supersampled coverage
oracle or final acceptance threshold. They demonstrate the user's complaint:
wood has holes and a widened edge; the shoot loses crisp needle structure and
spreads into a fuzzy cloud. Both sources were captured at two rotations and
three voxel levels. Material textures and lighting were deliberately absent
from both sides to isolate representation coverage; this is not a bark-quality
comparison. The native runs passed their rendering/identity checks with zero
Vulkan validation errors, while each quality report explicitly says
`accepted=0`. A green functional test does not imply accepted visual fidelity.

Artifacts under `C:/tmp/conifer-forest/source-quality-wood/` and
`source-quality-needle/`: `source-reference-0.png` is the triangle source;
`source-voxel-l0-0.png` is its matching finest voxel image. The `-1` rotation and
`l1`/`l2` levels are also available. Logs are the same directory names plus
`.log`; fingerprints and complete metrics are in `sparse-source-quality-evidence.json`.

Reproduce after building `sparse_voxel_bake_tests` and `vulkan_smoke_tests`:

```powershell
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/sparse_voxel_bake_tests.exe --part C:/tmp/conifer-cache/projects/c2bf6fbff660b3fd/ConiferForest 7e2c36734ef99b15 0.004 C:/tmp/conifer-forest/reference-needle.fixture
$env:MATTER_VK_SMOKE_MODE = 'sparse-voxel'
$env:MATTER_SPARSE_VOXEL_SOURCE_TRIANGLES = 'C:/tmp/conifer-forest/reference-needle.fixture.triangles'
$env:MATTER_SPARSE_VOXEL_CAPTURE_DIR = 'C:/tmp/conifer-forest/source-quality-needle'
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

Next work takes precedence over the previous performance ordering: build the
hybrid near representation with solid mesh wood and textured shoot clusters,
then fit distant aggregate coverage against matched, properly filtered source
views. Preserve tree/branch shapes and opacity across the transition. The
quarter-million-tree optimization resumes against that validated appearance.
The current volume approximation is not accepted as the final tree renderer.

Verification at this checkpoint: native editor build and existing raster GPU
regression passed (`sparse-lod-editor-build.log`, `sparse-lod-raster-regression.log`).
The CPU bake tests pass after adding source export (`sparse-source-export-tests.log`).
Forest timing evidence is in `sparse-lod-evidence.json`; its executable fingerprint
predates the later optional source-comparison harness, with production renderer
and shader sources unchanged by that harness. No goal completion is claimed.

## Source surface checkpoint: preserve the approved appearance

The user explicitly approved `surface-needle-d8/source-solid-0.png` and rejected
`source-patches-1.png` in that directory as broken. The approved image draws the
**original 5,800 source triangles** through the new solid surface path; it is
not evidence that needle texture reduction has been solved. Keep that image as
the visual target. The source, fitted proxy and voxel previews are unlit albedo
and depth comparisons, not final forest lighting or shadow evidence.

Added a generic fixed-surface representation alongside the sparse bricks:

- `surface_proxy.h/.cpp` builds immutable triangle/texture assets. The current
  baker finds components connected by shared edges (a shared attachment point
  does not merge neighboring needles), fits their principal plane, and limits
  maximum displacement from that plane. Components outside the error budget
  retain their original triangles. Twigs therefore remain solid and in place.
- Each accepted component gets a fixed 3D quad with a rectangular texture sized
  to its aspect ratio. The bake resolves depth per supersample and stores
  linear albedo, normal and coverage. Mips filter premultiplied data and carry
  cutout coverage scale. This is neither a camera-facing billboard nor the
  Poisson volume visibility model.
- `SparseVoxelBatch::surface` adds an optional near surface before the voxel
  levels, or a surface-only batch. Both use the same root selection, indirect
  commands, culling, material/instance identity, in-flight buffers and snapshot
  lifetime. No per-placement triangle records are uploaded.
- `surface_proxy.vert/.frag` writes ordinary opaque depth for solid surfaces
  and deterministic texture cutouts for patches. The initial upload stores
  texture mips in SSBOs and filters them manually. This validates representation
  behavior; it is not the final compressed, hardware-sampled texture pipeline.

A spatial-block bake (600 triangles) was rejected: its axis-aligned planes
lost 61% of reference pixels in one view, and displaced the twigs. Fitting the
average surface orientation within those same blocks still merged unrelated
leaves and did not fix silhouette error. That algorithm was removed. The
connected-component bake with a 1 mm error budget (2,000 triangles) restores
recognizable shape but still loses too much fullness. Its texture footprint is
83,045,200 bytes for this shoot prototype, another reason not to accept it as
production-ready.

Current conservative default: **0.35 mm maximum plane displacement**, texture
long dimension 256, 2x2 supersampling. For the cached pine shoot this bakes 297
of 950 needles into quads, keeps the other needles and all five twig axes as
triangles, and emits **4,612 triangles** with **25,962,552 texture bytes**.
This is only a 20.5% geometry reduction. It is an initial fidelity checkpoint,
not fulfillment of the requested highest-detail textured foliage solution.

Matched native Vulkan results at 1200x900 (reference and approximation use the
same source, pose, camera, material identity and tint):

| Representation | View | Reference pixels | Rendered pixels | Missing source pixels | Spill outside source |
|---|---:|---:|---:|---:|---:|
| Original needle triangles through new solid path | 0 | 65,384 | 65,384 | 0 | 0 |
| Original needle triangles through new solid path | 1 | 66,621 | 66,621 | 0 | 0 |
| Bounded textured patches + retained needle/twig geometry | 0 | 65,384 | 62,863 | 3,991 | 1,470 |
| Bounded textured patches + retained needle/twig geometry | 1 | 66,621 | 64,353 | 4,024 | 1,756 |
| Old finest voxel volume, unchanged | 0 | 65,384 | 86,468 | 11,249 | 32,333 |
| Original wood triangles through new solid path | 0 | 8,004 | 8,004 | 0 | 0 |
| Original wood triangles through new solid path | 1 | 7,856 | 7,856 | 0 | 0 |

The conservative proxy's net occupied area is 96.1%/96.6% of the reference;
source-mask overlap is 93.9%/94.0%. These are different quantities: do not call
96% an exact pixel match. Original wood (111,464 triangles) matches both source
coverage and depth exactly. No improved far-voxel fidelity or new 250,000-tree
performance result is claimed. The existing elevated forest route remains
above the 5 ms budget, and the prior volume appearance remains rejected.

Evidence under `C:/tmp/conifer-forest/`:

- `surface-needle-bounded/source-reference-{0,1}.png`: original geometry.
- `surface-needle-bounded/source-patches-{0,1}.png`: conservative current bake.
- `surface-needle-bounded/source-voxel-l0-{0,1}.png`: unchanged volume comparison.
- `surface-wood/source-solid-{0,1}.png`: solid wood, no volume holes.
- `surface-needle-bounded.log`, `surface-wood.log`: all focused GPU checks pass,
  zero Vulkan validation errors.
- `surface-cpu-tests.log`: fitted oblique charts, separate leaves sharing a tip,
  rectangular complete mips, nonplanar solid fallback, malformed-input rejection
  and atomic failure checks pass.
- `surface-editor-build.log`: native MSVC editor build passes.
- `surface-raster-regression.log`: existing raster regression passes, zero
  Vulkan validation errors.
- `surface-proxy-evidence.json`: source/executable/input/image fingerprints.

Reproduce the current pine comparison after building `vulkan_smoke_tests`:

```powershell
$env:MATTER_VK_SMOKE_MODE = 'sparse-voxel'
$env:MATTER_SPARSE_VOXEL_SOURCE_TRIANGLES = 'C:/tmp/conifer-forest/reference-needle.fixture.triangles'
$env:MATTER_SPARSE_VOXEL_CAPTURE_DIR = 'C:/tmp/conifer-forest/surface-needle-bounded'
$env:MATTER_SURFACE_PATCHES = '1'
$env:MATTER_SURFACE_MAX_ERROR = '0.00035'
$env:MATTER_SURFACE_RESOLUTION = '256'
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

Next implementation steps remain ordered by source fidelity:

1. Replace the expensive retained needle geometry with a better bent blade or
   small surface cluster bake, staying close to the approved silhouette across
   views. Test fir/redwood leaves too. Reduce texture duplication and use real
   sampled/compressed atlases. Material texture evaluation, two-sided foliage
   color and transmission still need integration.
2. Compile these shared surface prototypes into the part hierarchy and publish
   them from the provider. The new path is available through the renderer API
   and native tests; it is **not yet the editor forest's active provider path**.
   Preserve shared branches and avoid expanding millions of shoot instances.
3. Refit distant aggregate visibility against properly filtered reference
   coverage, including wood separately. The current stochastic volume cannot
   be accepted just by choosing a finer cell size or temporal blur.
4. Validate automatic transitions, moving-camera stability, normal lighting
   and useful shadows; then rerun the quarter-million-tree GPU routes with the
   tree-only budget (GI may be off). No goal completion is claimed here.

### Fixed layer experiment: rejected for runtime foliage

Three orthogonal stacks of fixed textured quads were tested as a near-foliage
bake, independently of distant voxel visibility. The final experiment textures
all 950 source needles and retains 100 original twig triangles. At 0.6 mm layer
spacing and resolution 1024 it produces 2,370 triangles and 1,135 textures.
Sparse 8x8 texture pages use occupancy masks, packed nonempty texels and a tile
lookup table. The measured asset still occupies 43,141,008 texel bytes plus
55,578,772 table bytes (98,719,780 bytes total).

This experiment is **rejected for runtime use**. On the RTX 4090, at 1200x900,
the enlarged shoot's median visibility cost is 2.234368 ms versus 0.047104 ms
for its 5,800 original solid triangles in view 0. View 1 measures 1.753088 ms
versus 0.041984 ms. Each measurement uses 12 warmup and 12 sampled frames;
selection has separate timestamps. These are diagnostic shoot measurements,
not quarter-million-tree timings or a complete lighting benchmark. Rasterizing
the layer rectangles processes too much empty space despite the lower triangle
count. Adding finer layers did not repair diagonal coverage either.

Nine identical source/candidate poses were captured. Indices 0..8 correspond
to orbit angles 0, 1.1, 0.76, 0.78, 0.79, 0.81, 2.2, pi and 3*pi/2 radians.
At angle 0 the candidate misses 6,231 source pixels and adds 3,854 outside the
source mask. Near the axis transition (0.78 radians), it misses 17,126 of
65,222 source pixels and adds 5,955 outside the mask. The original solid path
matches source masks and depth exactly at every tested angle. The layer bake
remains an opt-in native diagnostic and is not the editor forest provider.

Keep the generic sparse texture storage and its dense/sparse equivalence tests.
Do not continue increasing projection count or layer density without evidence
that the empty-space rasterization cost can be removed. Return to compact
foliage surfaces, shared hierarchy integration and corrected distant coverage.
The approved source remains the original solid reference; neither these layers
nor the current distant stochastic voxels satisfy appearance acceptance.

Distance policy must use projected error: a fixed voxel gets smaller on screen
as distance increases, while choosing a coarser level makes its world-space
cells larger. A pixel-scale target is a starting bound, not proof of fidelity.
Compare source and candidate at the same intended screen size; preserve branch
coverage, silhouette, color and stability through motion and LOD transitions.
Incorrect aggregate transparency does not become correct just by retreating.

Evidence under `C:/tmp/conifer-forest/`:

- `layer-needle-tight/source-{reference,solid,patches}-[0..8].png` and
  `layer-needle-tight.log`: nine-view coverage and first-two-view GPU timings;
  focused infrastructure checks pass with zero Vulkan validation errors.
- `layer-final-cpu-tests.log`: layer classification, resource budgets, malformed
  assets, sparse pages and existing surface bake tests pass.
- `layer-raster-regression.log`: raster regression passes; validation errors 0.
- `layer-editor-build.log`: native MSVC editor build passes.
- `run-layer-needle.ps1`: exact diagnostic environment and invocation.
- `layer-proxy-evidence.json`: fingerprints and measured results, explicitly
  marked rejected. Historical evidence files are preserved.

The goal remains active. There is no new accepted forest performance result,
no accepted distant-voxel fidelity result and no production provider change.

### Surface support, projected coverage and first lit forest checkpoint

The previous checkpoint made progress by closing the native build/regression
checks and recording the rejected layer experiment. This continuation changed
the voxel representation and added comparisons at actual viewing sizes.

`MATTER_SPARSE_SOURCE_DISTANCE=1` runs eight source poses (two angles, four
distances). A 4x4 supersampled source is compared with both native single-sample
voxel rendering and a 4x4 supersampled voxel estimate at 400x300. The filtered
estimate isolates representation bias; it is **not** an implemented runtime AA
solution. Contact sheets have source/L0/L1/L2 columns and increasing-distance
rows. Reported `cell_px` is the projected cell pitch at the object center, not
a proof that every cell or its full diagonal fits within that pixel bound.

Three changes address the source of fuzzy coverage:

- Each cell retains conservative bounds of the clipped source surfaces inside
  its grid address. Mixed cells distribute their area inside this tighter box.
- Coplanar cells retain a plane and direct projected coverage. One opaque sheet
  no longer needs dozens of coincident copies to hide Poisson holes. The GPU
  regression now uses one sheet; half a sheet measures approximately 49%
  coverage. Plane intersection bypasses a slab that can round to zero thickness
  for distant rays; the negative-direction far traversal test passes.
- The bake accumulates mean absolute normal projections in nine directions.
  A positive-semidefinite ellipsoid fit replaces the previous RMS projection
  estimate. Isotropic mean projection is 0.5, while the old RMS estimate was
  1/sqrt(3). Arbitrary distributions still incur fitting error. CPU orientation
  moments remain available; the current GPU facing-normal approximation uses
  the fitted distribution and is not a completed foliage shading model.

GPU cells grew from 64 to 80 bytes. Support bounds use three previously unused
words as conservative UNORM16 min/max pairs; the extra vec4 stores the plane.
The diagnostic fixture format is version 3; versions 1 and 2 remain readable.
Hierarchy resampling conserves area/color/orientation and carries bounds plus
the projected-area fit, but still resamples mixed volumes. Its current path
does not preserve a child's exact plane through arbitrary rotated quadrature.
That limitation remains visible when whole-tree aggregates are used nearby.

At a projected cell pitch of 0.927443 pixels, filtered occupied-area ratios to
the source were:

| View | LOD | Before | With support and projection fit |
| --- | --- | --- | --- |
| 0 | L0 | 1.327569 | 1.149900 |
| 0 | L1 | 1.371367 | 1.200286 |
| 0 | L2 | 1.429795 | 1.314498 |
| 1 | L0 | 1.334497 | 1.169065 |
| 1 | L1 | 1.407698 | 1.254882 |
| 1 | L2 | 1.441008 | 1.359107 |

These are meaningful improvements, **not appearance acceptance**. Residual
excess coverage is 15–17% at L0 and 31–36% at L2 in these samples. Native output
also remains noisy because stochastic visibility is not temporally resolved.
The initial support-only experiment failed the distant thin-plane traversal
test; `distance-projection.log` records the corrected passing implementation.

The user requested visible lit forests during this work. The native population
probe now optionally captures the renderer's linear HDR output, using the same
ACES reference curve as the display-transform regression, with a high sun and
a ground plane (`MATTER_SPARSE_FOREST_LIT=1`). It is a 250,000-instance preview
through the sparse renderer API, not the editor provider. GI is off and tree
shadows are absent. The first preview exposed two faults: HDR readback assumed
the GBuffer's read-only layout instead of HDR's color-attachment layout, and
the offline geometry probe ignored tint blend weights, turning untinted bark
white. Readback now restores HDR's actual layout. The offline source loader
restores cached material definitions and uses the renderer's base-color/tint
blend. Surface-detail bark atlases, per-material foliage transmission and
shadows still need integration; base material color is not the finished bark.

The refreshed four-tree bake still shares 47 prototypes, 4,192,972 source
triangles and 354,370 child links. Support reduced stored cells across all
prototype levels to 626,852; the measured geometry-only rebake took 16.58 s.
The source still represents 3.49 billion virtual expanded triangles. Near
rendering must traverse shared parts and select textured surfaces before
whole-tree voxel cells become visibly large; that provider integration remains
required. No completion or 5 ms acceptance is claimed.

Evidence under `C:/tmp/conifer-forest/`:

- `distance-baseline/`, `distance-support/`, `distance-projection/` and their
  corresponding logs preserve before/intermediate/after captures and metrics.
- `projection-cpu-tests.log`, `projection-hierarchy-tests.log` and
  `distance-projection.log`: focused CPU/hierarchy/GPU checks pass; the GPU run
  has zero Vulkan validation errors.
- `projection-tree-bake.log`, `tree-voxels-support/`: new support/projection
  geometry fixtures before source material-color correction.
- `lit-material-tree-bake.log`, `tree-voxels-material/`: base-color/tint-correct
  source fixtures, separate from the historical inputs.
- `run-lit-material-forest.ps1`: reproducible current lit population invocation.
- `lit-forest-support.log`: rejected first capture (white bark and readback
  validation errors); do not use its timings for acceptance.

The corrected lit run is `lit-forest-material.log` / `lit-forest-material/`.
All focused checks pass with **zero Vulkan validation errors**. The four
root prototypes occupy 94,856,588 GPU buffer bytes for 250,000 placed trees.
At 1600x1000, GI off, tree shadows absent, 16 warmup + 12 sampled frames:

| Route | Selection + visibility median | p95 |
| --- | --- | --- |
| Overhead, all 250,000 roots selected | 0.848192 ms | 0.862400 ms |
| Elevated, 74,504 roots selected | 7.577760 ms | 8.216704 ms |
| Ground, 56,447 roots selected | 4.605728 ms | 4.776320 ms |
| Moving, 56,700 roots selected at final pose | 4.306304 ms | 4.645312 ms |

These timestamps exclude shared lighting/composite work and are not the full
attributable tree-rendering metric. The elevated route still exceeds 5 ms.
The lit screenshots show grain, visibly coarse nearby whole-tree cells,
missing self-shadowing and the benchmark's regular placement grid. They are
an integration preview, not hyper-realism acceptance. `sparse-forest-lit-2.png`
is the ground view; `sparse-forest-lit-1.png` is the elevated view. No camera
frame or texture detail has been synthesized outside the renderer.

Next priority is the editor's shared hierarchy/provider path, selecting compact
textured surfaces and solid wood nearby, plus voxel visibility/shadows at their
intended distances. Preserve the actual source material detail through that
path. Continue coverage fitting and temporal stability work without hiding
representation bias behind blur. The complete forest goal remains active.

Final checks/evidence for this checkpoint: `lit-material-cpu-tests.log` and
`lit-material-hierarchy-tests.log` pass; `lit-material-raster-regression.log`
and `distance-final.log` pass with zero Vulkan validation errors. The final
distance run reproduces the corrected ratios above using the same executable
as the lit forest capture. The native editor build is recorded in
`projection-editor-build.log`. `support-lit-evidence.json` fingerprints the
final binaries, source, material-correct tree fixtures, logs and images while
explicitly recording that forest appearance and complete performance are not
accepted. Historical failed/intermediate captures are retained separately.

## Spatial grouping of the shared source hierarchy

The source cache exposes 44,860 / 83,048 / 61,093 direct children on the
pine / fir / redwood roots and 165,369 on the shared-base clump. `emitTree`
places shoots directly so branch scaling does not change physical needle or
cone size. Returning to uniformly scaled bough instances would change that
source, so this step preserves the authored placements and builds generic
spatial groups in `sparse_voxel_hierarchy` instead.

Grouping is opt-in through `HierarchyConfig::max_children`; zero preserves
the source graph. A stable spatial sort divides child placements into balanced
blocks, each with at most the configured number of children. Split sizes are
balanced by leaf-block count so partially filled ranges do not multiply the
number of groups. Generated nodes use identity transforms; the original
geometry references, duplicate placements, rotations, reflections and scales
survive descent. Mixed nodes get an explicit own-geometry leaf, so refinement
cannot discard their own surface. Every compiled child precedes its parent.
Groups remain shared per source prototype, independent of forest population.

Original source aggregate levels are baked from the original placements, as
before. Generated groups are also baked directly from their original child
placements, avoiding successive resampling through newly generated groups.
This does not eliminate the existing child-asset resampling approximation.
Each group uses an 8-cell longest-axis grid and one aggregate level. Its
spacing is an independent error measure: a topological child can have coarser
cells than an original source's 128-cell level. Runtime traversal must test
the candidate's error and continue through unsuitable groups; it must not
blindly equate descent with improved detail. The canonical normalized switch
distance rule remains the runtime selection contract.

Two bounded failures informed the final settings. The first 16-cell group
grid exceeded the 4,194,304-cell budget (`spatial-tree-bake.log`). An 8-cell
grid with ordinary midpoint splits fit the cell budget but exhausted the
16,384-node limit at 16,383 published nodes and 2,389,010 cells
(`spatial-tree-compact-bake.log`). Packing leaf blocks fixed the node-count
overhead without raising either budget.

The final native MSVC probe (`spatial-tree-final-bake.log`) passes:

| Property | Result |
| --- | --- |
| Original shared source prototypes | 47 |
| Generated shared spatial groups | 11,068 |
| Total compiled nodes | 11,115 |
| Original / compiled child links | 354,370 / 365,438 |
| Maximum compiled child count / depth | 64 / 14 |
| Unique source triangles | 4,192,972, unchanged |
| Stored aggregate cells, all prototypes and levels | 1,829,676 |
| Offline geometry compilation | 22.625 s |

This is one offline compilation measurement, not GPU timing. The new groups
have not been uploaded or rendered. All 28 exported root level fixtures are
byte-identical to `tree-voxels-material`, recorded in
`spatial-root-comparison.json`. Therefore the current lit preview and prior
GPU measurements remain unchanged; this checkpoint claims no visual or frame
time improvement.

`spatial-hierarchy-final-tests.log` passes focused native checks for exact
placed-source preservation, mixed own surfaces, shared loading, conserved
area, unchanged original aggregates, deterministic topology, coincident
instances, and atomic failures at node/depth/cell/work/edge limits. A fanout
boundary case verifies that partly occupied leaves fit the node budget.
`spatial-hierarchy-final-build.log` records the test build. The optional probe
command is `sparse_voxel_hierarchy_tests.exe --cached-world-grouped CACHE
ConiferForest ConiferTree,ConiferClump OUTPUT_DIRECTORY`; ordinary
`--cached-world` retains the original graph.

Next implementation: upload the shared prototype/child graph with root
placements and traverse it in bounded GPU queues. Compose child transforms
only for selected work, derive switch distances from representation error,
and retain a complete ancestor if a child set cannot fit the work budget.
Then capture a near lit tree with solid wood and textured foliage before
repeating the 250,000-tree route. Compact near textures, coverage accuracy,
temporal stability, material-detail atlases, shadows, editor integration and
complete attributable tree timings remain open. The goal is still active.

## Bounded GPU hierarchy traversal and compact draw streams

`VkSparseVoxelScene::create_hierarchy` and
`VkSceneRenderer::set_sparse_hierarchy` now accept shared prototype levels,
child transforms, root placements and explicit node/primitive capacities.
The initial implementation requires nonempty fallback geometry, similarity
transforms, children before parents, at most 64 children per node and at most
64 levels of graph depth. These match the compiled forest's current topology.
Input errors leave the prior renderer snapshot live. Ordinary batch rendering
remains available for matched comparisons.

Selection runs entirely on the GPU with separate buffers for three in-flight
frame slots. Each root starts with a reserved complete coarsest fallback.
Indirect compute waves cull nodes, apply the canonical normalized switch
distance rule, compose child transforms and refine selected nodes. Bounded
atomic reservations protect both the node pool and the total primitive count.
A parent is removed only after its complete child fallback set fits; otherwise
it remains in the final cover. Failed node reservations cannot overflow the
pool, and unused slots from failed primitive reservations are explicitly
inactive. Culling bounds union transformed descendants independently of the
fallback's support, preventing a coarse proxy from clipping visible children.

A final compaction pass emits only the selected cover into one shared draw-item
range: voxel items grow from the front, surface items from the back. Two
indirect graphics draws consume those streams. This avoids issuing one CPU
draw call for each prototype/level as the shared graph grows. Fragment shading
continues to use the existing sparse voxel and textured surface evaluators.
GPU timestamps include initialization, traversal, compaction and visibility;
they still exclude shared lighting/composite work.

The new path also keeps picking identity separate from stochastic coverage
identity. Root placement and child ordinal hashes produce a stable graph-path
seed independent of queue reservation order. Without this separation, repeated
shoots of the same tree would share their transparency samples and fail to
accumulate coverage. The legacy batch seed remains unchanged.

Native evidence in `C:/tmp/conifer-forest/`:

- `hierarchy-gpu-coverage-build.log`: native MSVC Vulkan test build.
- `hierarchy-native-editor-build.log`: successful native editor build.
- `hierarchy-gpu-final-tests.log`: **ALL PASS**, zero Vulkan validation errors
  on RTX 4090. This includes the existing sparse voxel/surface regressions.
- The transformed shared-child test mixes a textured triangle surface and a
  sparse voxel surface under nested transforms, including reflection. It
  matches 3,452 reference pixels with **zero mask differences**, a maximum
  depth difference of `9.31323e-10`, and the correct root picking identity.
  Exported reference/refined PNGs are byte-identical.
- Tests move the camera between different root placements, retain visible
  descendants outside a fallback's own bounds, exercise both exhausted budgets,
  and verify atomic rejection of invalid replacement graphs.
- 256 separated root objects compete for limited node and primitive budgets
  over four frames per limit. All objects remain visible; budget invariants
  and exhaustion counters pass under contention.
- Two overlapping half-covered shared leaves produce 2,073 pixels versus
  1,382 for a single leaf, a ratio of **1.5**, matching the expected independent
  coverage. Repeated frames reproduce depth and color exactly.
- `run-hierarchy-gpu.ps1` reproduces the focused GPU run. Diagnostic captures
  are under `hierarchy-traversal/`; these small geometry checks are not new
  forest appearance evidence. Intermediate build/test logs are retained.

This checkpoint has not yet connected the compiled 11,115-node tree graph to
the renderer. The existing cached-world probe only exports root voxel levels;
bridge its complete graph into the GPU test/provider path next. The latest
forest image and forest timings remain the previous root-only lit preview.
No new 250,000-tree performance or appearance claim is made here.

Remaining traversal work includes error-derived switch distances for the real
tree representations, prioritizing useful near-camera refinement, and a stable
choice of detail when budgets are saturated. Current global reservations may
choose different cuts under pressure; the coverage seed is stable, but that
does not make the selected LOD cut temporally stable. Coarse group fallbacks
also still require the quality/error handling noted above. These limits must
not be hidden by calling the diagnostic checks forest acceptance. The original
goal, compact near foliage, wood/bark detail, shadows, editor integration and
complete 5 ms tree-rendering metric remain open.

`hierarchy-gpu-evidence.json` fingerprints the final source, binaries, build and
test logs, comparison captures and prior compiler evidence. It explicitly keeps
forest appearance and performance acceptance false.

## Complete tree graph bridge and rejected forest cuts (2026-09-13)

The native probe now exports the complete DAG in the bounded diagnostic-only
`forest.hierarchy` binary and the Vulkan population adapter consumes it. This
is not an editor/provider integration or a production cache ABI. It preserves
all coverage/support attributes and exact prototype/child transforms. The
511,330,268-byte tree fixture contains 11,115 nodes, 365,438 compiled edges and
1,829,676 cells; its exported root levels are byte-identical to the previous
material-correct tree fixtures. Native CPU interchange/compile checks pass.

Three recorded 250,000-tree experiments used the same 1600x1000 cameras and
lighting, GI off, no tree shadows. All three are visually rejected. Dedicated
selection plus visibility medians in milliseconds (overhead/elevated/ground/
moving; shared lighting is excluded):

| Implementation | Overhead | Elevated | Ground | Moving |
| --- | ---: | ---: | ---: | ---: |
| Global reservations | 345.453 | 169.573 | See raw log | See raw log |
| Subgroup reservations | 5.884 | 8.573 | 4.966 | 5.200 |
| Topology first, near-first detail | 1.374 | 22.647 | 15.826 | 16.588 |

Evidence: `hierarchy-forest-lit.log`, `hierarchy-forest-subgroup.log`,
`hierarchy-forest-priority.log` and identically named capture directories under
`C:/tmp/conifer-forest/`. Some routes fit the partial timing budget; none is an
accepted forest rendering. The last forest cut exhausted its million-node pool
before reaching any source shoot leaf. A subsequent near-first topology queue
passes `hierarchy-frontier-tests.log` with zero validation errors, but has not
been measured on a forest. Do not transfer an earlier run's timing to it.

The bridge is roughly 1 GB on the GPU, including default selection/draw pools.
That storage is not a quality argument. Stable opacity seeds do not guarantee
stable representation selection when pools saturate.

## Whole-tree triangle equivalence checkpoint (2026-09-13)

The user's requirement is the appearance of the original model instanced as
triangles, at the same camera distance. Do not substitute a vaguely tree-shaped
volume or treat rejected visual quality as a successful performance result.
Source improvements remain authorized, but the reference must be explicit and
updated when source geometry changes; this checkpoint uses the existing cache.

Added a native diagnostic export/render pair:

- `sparse_voxel_hierarchy_tests --cached-triangle-reference CACHE WORLD HASH OUT`
  exports each finest source mesh once and exact placed transforms. The source
  keeps its original per-vertex shading normals and resolved base-color/tint.
- `MATTER_TRIANGLE_REFERENCE_FILE` and `MATTER_TRIANGLE_REFERENCE_HIERARCHY`
  select the Vulkan comparison mode. It renders original triangle instances,
  the root L0 voxel fallback, and the same adaptive hierarchy adapter used by
  the population probe, from two identical cameras.
- Pine `5db299eef3be6842`: 13 unique meshes, 1,047,172 unique triangles,
  44,860 mesh placements and 290,770,800 placed triangles. The triangle census
  matches the sparse compiler. `pine-triangle-reference.bin` is 91,013,112 bytes.
- Capture resolution is 1000x1000; front camera `(0,7.2,22)` and oblique camera
  `(14,8,18)`, target `(0,7.2,0)`, vertical FOV 1 radian. Shared high sun,
  roughness 0.8, GI off, no shadows or bark/detail atlases on either side.
  These isolate geometry, coverage and normals; they are not finished-material
  or lighting acceptance. Both lit HDR and raw albedo captures are retained.

Evidence under `C:/tmp/conifer-forest/`: `export-triangle-reference.ps1`,
`run-tree-reference.ps1`, `pine-triangle-export.log`, `tree-reference.log`,
`triangle-reference-cpu-build.log`, `triangle-reference-gpu-build.log`, and
`tree-reference/tree-{triangles,root-l0,hierarchy}-{0,1}.png` (plus albedo PNGs).
The native comparison passes functional checks with zero Vulkan validation
errors. It explicitly reports `appearance_accepted=0`.

| View / representation | Covered pixels | Missing source pixels | Extra pixels |
| --- | ---: | ---: | ---: |
| Front triangles | 88,612 | — | — |
| Front root L0 | 94,096 | 845 | 6,329 |
| Front adaptive | 90,906 | 1,190 | 3,484 |
| Oblique triangles | 81,625 | — | — |
| Oblique root L0 | 86,808 | 709 | 5,892 |
| Oblique adaptive | 83,623 | 1,092 | 3,090 |

Each adaptive view reaches all 44,860 source leaves, with zero budget fallbacks
and about 1.93 million selected voxel bricks. This isolates a representation
failure independently of forest budget pressure: even the source-leaf voxel
cut turns the opaque trunk into stippled coverage. Coarse root shading also
forms broad bands; refined foliage remains different from the triangle source.
Aggregate pixel coverage alone hides these errors and is not a sufficient gate.
The triangle source itself aliases at this sampling rate; smoothing it does not
authorize changing its structure or erasing solid surfaces.

Next work must preserve opaque wood/cones as surfaces and establish the near
foliage representation against this reference, including lighting and gaps.
Only allow a voxel substitution where matched views demonstrate acceptable
error; a one-pixel address-cell target is not proof of that error. A source leaf
with coarse finest voxels currently has no surface escape path in the tree
adapter. Do not hide that limitation with looser distance thresholds or accept
coarse fallback appearance merely because a bounded renderer remains fast.
Then test moving views, lit/shadowed small stands, and the full population.

## Hybrid solid-surface and error-bound checkpoint (2026-09-13)

The generic renderer now accepts a solid surface LOD ladder before voxel
levels, including surface-only leaves that stay solid at every distance.
The pine diagnostic replaces its 228 wood/cone placements with these surfaces;
44,632 needle-cluster placements still use voxels. Original triangle normals
and colors are retained. GPU budget selection tries intermediate affordable
rungs instead of immediately dropping a rejected refinement to the coarsest.
Native surface-ladder, hierarchy, budget, coverage and depth regressions pass
with zero Vulkan validation errors (`surface-error-gpu-tests.log`).

The authored-strip hybrid fixes the porous trunk, but remains visually rejected.
At the front camera it covers 90,664 pixels, missing 1,276 source pixels and
adding 3,328, with 1,840,692 voxel bricks and 2,243,320 surface triangles and
no primitive budget fallbacks. Evidence: `pine-surface-reference.bin`,
`surface-tree-reference.log`, and `surface-tree-reference/`. No current forest
performance claim follows from this single-tree diagnostic.

Authored wood strips do not exactly match the source isosurfaces. A new generic
`mesh_error` utility bounds symmetric surface distance, including triangle
interiors via adaptive subdivision and nearest-triangle BVH queries. Its
native tests cover interior holes, both distance directions, known offsets,
degeneracy, transforms, query budgets and conservative depth limits. The
diagnostic STRIREF3 format adds these independently measured bounds to each
solid rung; older v1/v2 fixtures remain readable.

Measured source-to-strip bounds are approximately 0.270 m for the trunk,
0.049 m for large branches, 0.027 m for medium branches, and 0.014 m for small
branches. The full geometric bound also includes buried/nonvisible surfaces.
Using a conservative one-pixel projection of those errors keeps much more
original isosurface geometry near the camera. With the existing 4,194,304-item
primitive pool, the front view selects 3,974,854 surface triangles and only
219,450 voxel bricks, with 40,610 failed detail refinements. This starves the
foliage and is also rejected (`pine-error-reference.bin`,
`error-tree-reference.log`, `error-tree-reference/`). A geometric bound alone
does not establish acceptable appearance or good allocation between materials.

A source-derived solid LOD baker now invokes the existing QEM simplifier,
reprojects original shading normals and colors, and independently verifies
surface error. Its offline dependencies are isolated in `surface_proxy_lod.cpp`
so the Vulkan smoke renderer does not pull in the mesh-editing toolchain.
Native planar reduction, source-normal/color preservation, and invalid-input
atomicity tests pass (`solid-lod-bake-build.log`, `solid-lod-bake-tests.log`).
It has not yet been used to generate tree rungs or solve the allocation issue.

The user's observation of bottom-lit hybrid trees identifies another failed
appearance gate: the voxel shader's diagnostic facing-normal approximation.
When mean source normals cancel, it normalizes `-M * ray_direction`, where M
is the fitted projected-area ellipsoid (legacy fixtures use the source normal
second moment). Roughly isotropic fits yield a normal
facing the camera. Upper-canopy rays then produce downward-facing lighting
normals and lower-canopy rays produce upward-facing normals. The shader feeds
that one normal into the ordinary surface lighting calculation. Both the sun
convention and the native fixture specify an overhead sun; missing self-shadows
in the diagnostic further expose the lower canopy. Preserve the source normal
distribution for aggregate shading and validate it against triangle lighting;
do not fix this by tilting normals upward or moving the sun below the scene.

Native normal captures now verify that diagnosis with the committed renderer
sun and identical covered pixels. Enable `MATTER_TRIANGLE_REFERENCE_NORMALS`
for an additional composite normal view; its PNG stores `normal * .5 + .5`
directly, without ACES, exposure or sRGB conversion. The fixture asserts unit
decoded normals, unchanged coverage, and an overhead committed sun.

| Front-camera foliage | Mean normal Y, top / bottom | Mean sun cosine, top / bottom |
| --- | ---: | ---: |
| Original triangles | -0.0628 / 0.1744 | 0.3638 / 0.4804 |
| Root L0 voxels | -0.1862 / 0.2500 | 0.2579 / 0.5686 |
| Hybrid | -0.1306 / 0.1707 | 0.3297 / 0.4912 |

These are per-covered-foliage-pixel statistics split at the image midline;
the clamped normal/sun cosine is a direct-light orientation term, not final
brightness or a shadow measurement. The original triangles also favor the
lower canopy under this camera and shadowless lighting. Hybrid normals
exaggerate the vertical difference (bottom/top ratio 1.49 versus 1.32), and
root aggregation is worse (2.20). The oblique view shows the same ordering.
The results therefore implicate both the aggregate-normal approximation and
the incomplete shadowless lighting setup; they do not establish normals as
the sole cause. No normal or lighting correction has been applied yet.

Evidence: `run-normal-tree-reference.ps1`, `normal-reference-build-4.log`,
`normal-tree-reference.log`, `normal-tree-reference/` (24 native lit, albedo,
and normal images), and `hybrid-normal-evidence.json`. Native comparison:
ALL PASS, zero validation errors, appearance still rejected. An initial probe
incorrectly set the authored lighting debug field instead of the renderer's
composite override; it captured lit color rather than normals. Those invalid
diagnostics are retained separately under `normal-tree-reference-invalid-debug`
and must not be used as normal evidence. The corrected probe adds the unit
normal and coverage assertions above to reject that mistake explicitly.

## Visible-normal shading checkpoint (2026-09-13)

Previous turn classification: progress. Native normal captures confirmed the
vertical lighting error and established separate triangle/hybrid baselines.
The current change replaces the single facing-normal approximation with a
sample of the visible normal distribution of the existing projected-area fit.
It follows [Heitz et al., The SGGX microflake distribution, section 5](https://research.nvidia.com/labs/rtr/publication/heitz2015sggx/).
The sampler uses reverse Cholesky factorization in the viewer's basis, a cosine
disk sample, and a relative diagonal regularization of 1e-6 for singular fits.
Actual planar cells retain their plane normals. The cell format, extinction,
coverage seeds, geometry, source assets and sun stay unchanged. The uploaded
matrix is the projected-area fit, not the original source second moment;
the earlier checkpoint's description above has been corrected accordingly.

New native GPU tests independently integrate the analytic SGGX density on the
CPU and compare sampled normal means and diffuse illumination. Cases cover
isotropic, anisotropic, rotated, reflected and opposing rank-one sheets. Each
case has 147,456 covered pixels; the maximum measured error among normal and
illumination terms is 0.001148 (tolerance 0.015). Isotropic mean cosine agrees
with 2/3 and perpendicular diffuse response with 2/(3*pi). All normals are unit
and face the viewer, and unchanged input reproduces identical output. The full
sparse GPU suite passes with zero Vulkan validation errors:
`visible-normal-build.log`, `visible-normal-tests.log`.

The matched whole-tree run also passes its native functional checks, with
zero validation errors and exactly unchanged coverage and selected primitive
counts. Albedo is identical except at one oblique hybrid pixel (519,407),
which changes by at most 10/255 in a display channel; its cause has not been
isolated, so do not claim bit-identical albedo across shader builds.
`visible-normal-tree/` holds 24 lit/albedo/normal captures
from the same two views as `normal-tree-reference/`.

| Front foliage shading | Triangle reference | Old hybrid | Sampled hybrid |
| --- | ---: | ---: | ---: |
| Mean clamped sun cosine, upper half | 0.3638 | 0.3297 | 0.3323 |
| Mean clamped sun cosine, lower half | 0.4804 | 0.4912 | 0.4501 |
| Lower / upper ratio | 1.320 | 1.490 | 1.354 |

The root-L0 ratio improves from 2.205 to 1.442; the source is still 1.320.
Oblique hybrid ratios are 1.398 -> 1.302, against 1.280 for triangles. The
vertical bias is substantially reduced, but average hybrid illumination is
still lower than the source. SGGX describes the fitted distribution; it does
not reconstruct every original needle normal or its correlation with color,
visibility and position. This is not full appearance equivalence.

There is a real variance tradeoff. For front-view common covered pixels,
display-RGB MAE rises from 0.1422 to 0.1494, while an 8x8 block-mean comparison
only improves from 0.02450 to 0.02419. Root L0's broad shading mismatch improves
more strongly (block MAE 0.05718 -> 0.02688), but its individual-pixel error also
rises. These measurements use native PNGs and do not modify or smooth the
delivered captures. New images remain noisy and are **not accepted** as the
forest's finished appearance. No new performance result has been claimed.

The next variance work must connect the existing temporal renderer correctly,
not blur the tree image after capture. `VkSparseVoxelScene::record_selection`
currently receives no previous-frame matrices; sparse voxel and surface
fragments write zero velocity and full reactivity. The normal sample seed is
also fixed per pixel. Therefore simply enabling a temporal resolver cannot
converge the new samples reliably. The engine already carries current/previous
jittered and unjittered matrices, presented-frame indices and reset state in `TemporalFrame`,
and its DLSS path can consume motion/reactivity. Provide camera-correct motion
for static immutable sparse snapshots, reject history after snapshot changes,
and vary normal samples only when accumulation is active. Verify motion, retry
stability and disocclusion on native moving views before accepting a temporal
quality result. Direct canopy shadows, compact near needle textures, affordable
source-derived solid LODs, provider/editor integration and the complete
250,000-tree 5 ms GPU acceptance remain unfinished.

## Sparse temporal input and production DLSS checkpoint (2026-09-13)

Previous turn classification: progress. Visible-normal sampling reduced the
excess vertical lighting bias but exposed substantial single-sample variance.
This turn connects sparse foliage and solid surfaces to the renderer's temporal
input and tests actual DLSS accumulation through a production Vulkan device.

Each sparse frame now uploads a 96-byte previous-camera/extent/history/sequence
record in a dedicated storage binding (288 resident bytes across three frame
slots). Both sparse voxel and surface fragments emit camera motion at their
world-space hit, in internal-resolution pixels. The current and previous
matrices include jitter, matching the existing raster shader and Streamline's
`motionVectorsJittered=true` convention. Invalid history emits zero motion and
full reactivity. Offscreen or behind-camera previous projections also reject
history; this projection check does not establish arbitrary occluder rejection.

History belongs to the immutable sparse snapshot and frame that was actually
presented. The normal frame completion callback promotes it only on success
with the matching submission serial. Replacement, reset and extent mismatch
reject history; rejected publication preserves the previous snapshot. Normal
and coverage sample sequences vary with the successfully presented frame index
only when an accumulator is active. Native rendering retains fixed samples.
This handles static snapshots and camera movement; it does not add tree wind
or per-object deformation motion.

Native smoke tests use the production frame submission path for voxel, solid
surface and hierarchy-surface cases. They verify first frame, camera jitter,
lateral movement, rejected-frame retries, wrong-serial callbacks, snapshot
replacement, failed publication, camera cuts and offscreen re-entry. Separate
sequence tests verify varying samples, identical retries, and stable native
output. The final suite reports ALL PASS and zero Vulkan validation errors:
`sparse-motion-final-build.log`, `sparse-motion-final-tests.log`. The smoke
device deliberately disables Streamline; a test-only switch exercises sample
sequencing without claiming a temporal resolver.

These tests also exposed a screenshot-helper layout assumption after normal
frame presentation. `VkRasterAttachment` now reports its tracked layout, and
the helper transitions from and restores that actual layout. The initial
`sparse-motion-tests.log` has ten validation errors and is rejected; both the
corrected intermediate and final runs report zero. The tree-reference mesh
and hierarchy adapters now live in `tests/sparse_reference_scene.h`, shared
by the smoke fixture and the production preview.

The new manual CMake target `sparse_forest_preview` links the production viewer
objects and uses the installed, signed Streamline 2.12 runtime. Build it through
the official Windows wrapper with `-EnableStreamline -StreamlineRoot
D:/SDKs/streamline-sdk-v2.12.0`; the WSL convenience wrapper otherwise disables
Streamline. The current cache is SDK-enabled, PhysX remains disabled, and the
separate output is `MatterEditor/build/windows-msvc-forest/sparse_forest_preview.exe`.
The normal editor executable was not rebuilt or launched in this checkpoint.
The preview accepts `SOURCE HIERARCHY OUTPUT_PREFIX native|quality
triangles|hybrid 0|1|2` and captures the actual composed swapchain output.

Five 64-frame native runs completed on the RTX 4090, each with four screenshots
and zero Vulkan validation errors. Quality runs assert that real DLSS remains
active rather than accepting a fallback. The matrix contains stationary source
and hybrid, moving source and hybrid, and stationary native hybrid. All output
images are 1000x1000; Quality renders internally at 667x667, while Native renders
at 1000x1000. Both references use +2 EV display exposure, the same overhead sun,
GI/RT/volumetrics disabled, and the same original source placements. The moving
view remains stationary for 32 frames, then translates camera and target by
0.06 m per frame, reaching x=1.92 m at frame 63. This is a single pine, not a
250,000-tree performance run. Canopy shadows and bark detail atlases are absent.

Artifacts in `C:/tmp/conifer-forest/`:

- `live-preview/`: 20 native screenshots at frames 0, 31, 47 and 63.
- `live-preview-ev2-*.log` and separate stderr logs: five production runs.
- `live-preview-matrix-2.log`: all five runs completed successfully.
- `run-live-preview-matrix.ps1`, `run-live-preview.ps1`: exact launch scripts.
- `live-preview-build-3.log`: final production preview build.
- `live-preview-measurements.json`: display-image measurements only.
- `sparse-temporal-evidence.json`: SHA-256 fingerprints of inputs, source,
  binaries, scripts, logs and captures, with acceptance explicitly false.

The native loader still reports stale Epic overlay manifest paths; these are
loader/general diagnostics, not renderer validation failures. Optional latency
plugin diagnostics do not prevent the verified DLSS Quality evaluations. The
first matrix wrapper failed because PowerShell treated native stderr as a
terminating error. The corrected launcher redirects native stdout and stderr
separately with `Start-Process` and checks the actual process exit status.

Visual inspection shows substantially less grain after accumulation and a
closer source/hybrid silhouette in both views. It does not establish exact
source equivalence or temporal stability. A deliberately limited green-color
display mask (G > B + .025 and G > R + .008, normalized RGB) has source/hybrid
intersection-over-union .9573 stationary and .9526 moving. This mask excludes
wood and dark samples, so it is not geometric coverage. Display RGB MAE on the
mask union is .06446 stationary and .08912 moving. Stationary frame-31/frame-63
MAE is .04834 hybrid and .04671 triangles: even the settled captures still
vary. Native frames 31 and 63 are bit-identical, but visibly grainy. These
measurements do not modify or filter any delivered screenshot.

Both source and hybrid remain brighter at the bottom in the shadowless setup.
The sampled normals reduce the hybrid's additional bias; temporal accumulation
does not replace canopy self-shadowing. The authored trunk strips also visibly
differ from the source isosurface, particularly near the base. Appearance
acceptance remains false, and no new GPU timing or 5 ms claim is made.

Next work remains compact near needle textures, affordable source-derived
solid LODs, direct canopy shadows, provider/editor integration and full forest
appearance and tree-attributable GPU measurement at 250,000 placements.

## Sparse sun-shadow checkpoint (2026-09-13)

Previous goal turn classification: progress. It connected sparse motion/history
and produced real production-DLSS captures. This turn adds shadow visibility
from sparse geometry and establishes a corrected opaque-triangle reference.
The full forest objective remains active and unachieved.

`VkSparseVoxelScene::create_shadow_casters` builds an immutable light snapshot:
one brick-AABB BLAS per unique fixed voxel asset, shared across its TLAS
placements. It reuses the validated cell packing, allocates no raster pipeline
or selection buffers, and releases build inputs/scratch after setup completion.
The renderer retains old snapshots with submitted frames. Publication is atomic,
empty input removes the casters, and replacement resets whole-frame temporal
lighting. `record_shadows` performs one compute ray query per covered receiver,
multiplies the existing sun visibility, and participates in the production frame
and immediate native diagnostic paths, including with GI and triangle RT off.
It has independent GPU timestamps; no per-frame caster upload or AS rebuild.

Primary and shadow paths share `sparse_voxel_optics.glsl`: support clipping,
fractional planes, projected-area extinction and within-cell hit distances.
Empty cells inside a brick remain empty. Each brick/placement uses a stable
optical-depth draw and an actual generated intersection; shadow samples vary
with successfully presented frames only when accumulation is active. This is
sampled binary visibility with the expected fractional transmittance, not a
deterministic transmittance integral or colored leaf transmission. The stable
draw is necessary because the [Vulkan ray traversal specification](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html)
permits repeated AABB candidates; summing their optical depth directly could
darken the same brick repeatedly. Early termination follows a confirmed hit.

Native tests now verify fractional sheets, overlapping shared placements,
opaque blockers, self-shadow bias, reflected/scaled/rotated casters, empty
interior cells, mixed-cell extinction, finite ray ranges, rays facing away,
repeatability, failed replacement, and removal. Measured transmission is
.501875 for one half-covered sheet, .252083 for two sheets (expected .25), and
.503750 for a mixed cell whose analytic transmission is .5. A capacity test
builds 250,000 placements of one analytic cell in a 500x500 grid, with
75,365,152 resident bytes. At 320x240 it measures .114176 ms shadow dispatch
with a 5000 m ray interval and zero transmission through the stacked column.
This is a capacity/traversal test, not a forest rendering or quality result.

The triangle reference required two corrections. Diagnostic materials had
been zero-initialized without setting `scattering_shape[3]` (shadow opacity).
They therefore transmitted light and were not an opaque triangle reference;
the any-hit layer cap could still produce misleading ground shadows. Both the
preview and analytic fixture now explicitly author shadow opacity 1. Separately,
the existing primary sun pass uses a depth-dependent geometric-normal offset,
up to .5 m, which skips small nearby occluders. `VulkanRayTracingSettings` now
has `max_normal_bias`, default .5 m to retain existing behavior; the exact
source preview uses zero and keeps the existing .001 m light-ray bias. Changing
the cap invalidates temporal lighting. A real triangle test places a blocker
.03 m above its receiver: default offset gives visibility 1, reference cap
gives 0. Both traced frames assert RT is active and retain the raster receiver.

Final validation: `sparse-shadow-complete-build.log` and
`sparse-shadow-complete-tests.log`, ALL PASS, zero Vulkan validation errors.
Earlier failed runs are preserved: the first lacked conventional mesh data
required by the legacy capture path; the first population test accidentally
retained a 10 m ray range; the first bias test had zero shadow opacity. The
passing final suite includes the corrected cases, the new gap/rotation checks,
and all previous sparse/motion/visible-normal tests. It does not claim that
unrelated previously recorded viewer-logic failures have been fixed.

The production preview now includes a common ground receiver and optional
`none|sparse|fine|triangles` shadow mode. The source triangles are explicitly
enrolled in the RT TLAS only for the triangle-shadow reference. `sparse` uses
the whole-tree root's finest cached voxel level: 2,635 bricks, 100,402 cells,
.113263 m cells, 8,219,056 resident shadow bytes. `fine` runs a bounded diagnostic
hierarchy cut targeting .01 m cells or terminal assets. For this pine the cut
reaches 44,860 placements and 13 shared prototypes, 8,964 unique bricks and
50,756,948 virtual placed bricks, using 23,072,256 shadow bytes. Terminal wood
assets may exceed the requested cell spacing. The cut expands only this
comparison tree; it is not the forest's runtime shadow compiler.

`run-shadow-opaque-matrix.ps1` completes eight 64-frame runs, each with four
native swapchain screenshots and zero validation errors. Source and hybrid
are compared stationary and after a 1.92 m lateral camera translation. Every
run asserts real DLSS Quality is active, with 667x667 internal rendering,
1000x1000 output, +2 EV, opaque test materials, overhead sun, GI/volumetrics off,
and the same source geometry. Hidden-window capture is optional through
`MATTER_FOREST_PREVIEW_HIDDEN=1`; this avoids minimizing the preview, which
interrupted one earlier visible-window run with a zero-sized framebuffer.
No editor launch or final full-forest timing is claimed here.

Final evidence under `C:/tmp/conifer-forest/`:

- `shadow-opaque/`: 32 corrected native screenshots.
- `shadow-opaque-*.log`, `shadow-opaque-matrix.log`: all eight successful runs.
- `sparse-shadow-opaque-preview-build.log`: the production preview build used
  by those captures.
- `measure-sparse-shadows.py`, `sparse-shadow-measurements.json`: measurements
  of native captures; no image edits or filtered deliverable images.
- `sparse-shadow-evidence.json`: source/input/binary/script/log/capture hashes,
  with forest quality and performance acceptance explicitly false.

The earlier triangle-shadow images in `shadow-preview/` and `shadow-reference/`
have zero shadow opacity and must not be used as opaque source references,
even when their protocol assertions and Vulkan validation passed. The initial
triangle run also omitted TLAS enrollment; its assertion rejected it. These
errors explain the initially reported large brightness mismatch. They do not
justify changing voxel density to match a transparent source material.

| Final comparison against opaque triangle rendering | Display RGB MAE | 8x8 masked mean error | Ground shadow ROI IoU |
| --- | ---: | ---: | ---: |
| Source triangles, coarse voxel shadows, static | .05144 | .04427 | .96151 |
| Source triangles, fine voxel shadows, static | .03036 | .02125 | .98867 |
| Hybrid, coarse voxel shadows, static | .06951 | .04873 | .96144 |
| Hybrid, fine voxel shadows, static | .05875 | .02398 | .98866 |
| Hybrid, coarse voxel shadows, moving | .06962 | .05095 | .95153 |
| Hybrid, fine voxel shadows, moving | .05874 | .02797 | .97964 |

These are display-space measurements at +2 EV, not linear radiometric error.
The foliage mask comes from the same-camera unshadowed source and excludes
some dark samples; it is not geometric coverage. The ground ROI excludes the
trunk. Exact formulas and limitations are in the measurement JSON/script.
Fine shadowing improves the matched lighting gaps and shadow silhouette but
does not establish exact appearance equivalence or temporal convergence.
The hybrid trunk strips still differ from the source isosurface.

Three warm captured shadow timestamps give median .0513 ms coarse and .2919 ms
fine for the static hybrid, .0552/.2864 ms for the moving hybrid. Fine shadows
on the triangle raster reference cost .4629 ms. These are isolated single-tree
shadow passes at 667x667 with validation enabled, not whole-tree rendering or
the 250,000-tree/5 ms acceptance metric. The fine diagnostic's 44,860 placements
per tree cannot be multiplied across the forest. A full-scale detailed shadow
path must retain shared whole-object hierarchies (for example, a root
acceleration structure querying shared object-space inner hierarchies), or
compile compact near foliage first; it must not flatten every needle placement
for each forest root. That architectural work is still open.

Next priority remains compact textured near foliage and source-derived solid
LODs, followed by shared forest shadow hierarchy, provider/editor publication,
and matched full-population views with complete tree-attributable GPU timings.
Colored transmission, foliage ambient occlusion, bark detail atlas integration,
wind, biological validation and final visual acceptance remain unfinished.


## Textured foliage and surface packet checkpoint (2026-09-13)

Previous goal turn classification: progress. It finished and verified the
opaque-shadow evidence manifest (80 artifacts). This turn implements a
component-cluster texture baker, corrects temporal cutout coverage, and changes
hierarchical surface submission from individual triangles to triangle packets.
The quarter-million-tree objective remains active and unachieved.

`surface_proxy::bake_clusters` groups disconnected thin components onto fixed
fitted planes. Each proposed merge checks every source vertex against an
absolute projection bound, each component plane against a normal-alignment
limit, and the combined bounding-box diameter. Stable largest-support seeds
and nearest acceptable merges produce deterministic groups. Round components
remain exact triangles even under a permissive absolute error limit. Shared
raster/mip code retains transparent gaps, albedo and normals; sparse texture
pages omit empty texels. Fitting/raster/texture budgets fail atomically, including
statistics. This is a geometric projection bound, not a bound on filtered
image error or on visibility from every direction. The diameter limits merges;
an individual source component may exceed it.

The existing source shoot has 5,800 triangles: 950 six-triangle needle
components and five twenty-triangle twigs. A .001 m bake produces 702 patches
plus 100 exact twig triangles, totaling 1,504 triangles. Its final fitting bound
is .00099979 m, with 64,861 candidate fits, 9,939,032 texel bytes and 2,213,936
page-table bytes. Nine native close-up directions completed with validation 0.
This close-up remains too thin: the first view covers 52,650 pixels versus
65,384 source pixels, with 19,148 misses and 6,414 extra pixels. Visibility
cost is about .0635 ms versus .0133 ms for the original triangles at this
magnification. Triangle reduction alone does not establish a useful renderer.
The CPU surface tests, including merge bounds, work-budget atomicity and
preservation of volumetric solids, pass.

At .004 m the four source shoot variants produce 756/774/784/788 triangles.
The first lit-tree test with fixed alpha cutoff lost most of its needles at
minification (`cluster-tree-e4/`); it is rejected. The shader now samples raw
filtered alpha as coverage probability when an actual accumulator is active.
The native fixed-cutoff fallback retains its prior mip coverage scales. Those
scales must not multiply a stochastic probability. Sampling uses stable tree
path, texture and pixel keys plus the successfully presented frame sequence;
the two triangles of each patch share samples, and distinct placements do not.
It uses the established motion/history mechanism and production DLSS.

Analytic GPU measurements give .253333 covered pixels for alpha 64/255
(expected .250980), and .436132 for two overlapping instances (expected
.438970). Rejected-frame retries reproduce identical samples. All previous
sparse visibility, motion, shadow, hierarchy and LOD checks pass too. This
checks sampling inputs and coverage, not the quality of a mocked denoiser.

The corrected .012 m bake uses 394/392/392/396 triangles per shoot variant,
including 100 exact twig triangles each, with roughly 6.95–7.39 MB of sparse
texture data per variant. The production preview attaches these four shared
assets to leaf prototypes; parent sparse aggregates retain their normal
hierarchical distance selection. The diagnostic uses a fixed bake and a
32-million-primitive limit. It is not yet the provider's near-foliage LOD
compiler; multiple error-bounded texture levels and their canonical switch
distances still need integration. All .012 m final runs select the intended
geometry with zero node or primitive budget fallbacks. The .004 m diagnostic
hits that primitive limit and is not a complete selected-detail reference.

The resulting single-tree primary cover contains 19,804,728 triangles and
46,261 allocated hierarchy nodes. Instrumentation exposed the old submission
path writing a separate 16-byte record and issuing a separate graphics instance
for each triangle. `SparseHierarchyConfig::surface_triangles_per_packet` now
supports powers of two from 1 through 128, default 64. Surface compaction emits
one record per contiguous packet. The vertex shader computes its triangle and
corner within that packet and clips unused tail vertices before buffer reads.
Voxel submission, LOD distances and the actual triangle-budget accounting are
unchanged. The final emission counter counts triangles independently of the
reservation; packet count and triangle count are both reported and validated.
The record buffers remain conservatively allocated for the primitive budget;
this change reduces records written and graphics instances, not reserved VRAM.

Native packet tests compare 1/8/64/128-triangle packets with 134 triangles in
two reflected/distinctly identified placements. Nonmultiple tail packets are
visible in the test. Depth, shading and picking agree exactly across packet
sizes. Invalid granularity rejects publication. The final native suite is
`surface-packet-tests.log`: ALL PASS, zero Vulkan validation errors, including
the earlier temporal coverage and sparse shadow cases. Build evidence is
`surface-packet-complete-build.log`. Earlier diagnostic runs include a stopped
GPU test, an unavailable test-only preview timing method, and an invalid mixed
surface/voxel switch ladder; final runs correct those conditions. No final
result is inferred from the interrupted or rejected runs.

`run-surface-packet-matrix.ps1` produces three 64-frame production previews,
with four native screenshots each: scalar static, packet-64 static and
packet-64 moving by 1.92 m. All require actual DLSS Quality at 667x667 internal /
1000x1000 output, +2 EV, overhead sun, opaque materials, .001 m light-ray bias,
zero triangle normal-offset cap, GI/volumetrics off and detailed sparse shadows.
All three complete with zero Vulkan validation errors.

| Same .012 m baked tree | Selection ms | Visibility ms | Combined primary ms | Shadow ms | Surface records |
| --- | ---: | ---: | ---: | ---: | ---: |
| Individual triangles, static | 2.7031 | 9.9502 | 12.6533 | .2781 | 19,804,728 |
| 64-triangle packets, static | 2.1820 | 3.6936 | 5.8771 | .3071 | 347,592 |
| 64-triangle packets, moving | 2.1893 | 3.6895 | 5.8874 | .2912 | 347,487 |

These are medians of three warm captured timestamps with validation enabled,
not a sustained framerate test. The combined column is the median of each
sample's selection + visibility, not the sum of independent medians. Primary
cost excludes shadows, composite and DLSS. These are one-tree results on the
RTX 4090 and do not satisfy the forest/5 ms objective. Packet submission is
about 2.15 times faster for this primary cover and emits roughly 57 times fewer
surface records without selecting coarser geometry.

The initial scalar/packet screenshot is byte-identical. Accumulated screenshots
are not: frame-63 display RGB mean absolute difference is .00003758 over the
full image, with 19,144 changed pixels and a maximum isolated channel difference
of 40/255. Do not claim exact production-DLSS equivalence from the small average.
Against the original opaque triangle source, the packet tree's masked display
RGB MAE is .06687 static / .06391 moving, with masked 8x8 mean error
.03136 / .03042. Static masked display luma is .35129 versus source .37754.
This is somewhat darker and differs in foliage detail and authored wood strips.
It does not establish final appearance acceptance. Masks omit dark pixels and
wood, and display-space errors are not radiometric or geometric bounds.

Final evidence under `C:/tmp/conifer-forest/`:

- `surface-packets/`: 12 final native screenshots, individually referenced by
  `surface-packets-*.log` and the successful matrix log.
- `surface-packet-tests.log`, `.stderr.log`, `surface-packet-complete-build.log`.
- `measure-surface-packets.py`, `surface-packet-measurements.json`.
- `surface-packet-evidence.json`: source, input, binary, script, log and capture
  hashes with all forest/performance/source-appearance acceptance flags false.
- `surface-packet-checkpoint/`: archived changed source files and built binaries
  so subsequent work does not overwrite this checkpoint's binary evidence.
- `cluster-shoot-e1/`, `cluster-temporal/`: intermediate source/temporal
  comparisons. `cluster-tree-e4/` is the rejected hard-cutoff forest image.

Next architecture work is shared whole-tree shadow traversal and the remaining
hierarchy-selection cost, followed by a moving-camera lit forest scaling test.
A promising shadow organization is an outer forest acceleration structure whose
candidates query shared object-space inner structures containing branch
placements. Its Vulkan interface and traversal cost still need verification;
this is an implementation direction, not a completed capability. It would
retain each tree's internal sharing instead of multiplying 44,860 shadow
placements per forest tree. Near texture LOD integration, source-derived solid
LODs, provider/editor publication, full population quality/performance, bark,
wind and biological validation remain open.

## Shared forest shadow hierarchy checkpoint (2026-09-13)

Previous goal turn classification: progress. It completed clustered textured
foliage, temporal alpha coverage and surface packet submission. This turn
implements reusable whole-object shadow acceleration structures and captures
lit forests with 250,000 tree placements. The goal remains active: neither
source appearance nor the full tree-attributable 5 ms budget is accepted.

`SparseShadowObject` describes fixed voxel part assets with object-local
placements and a separate array of world placements. A shared object-space
TLAS retains its branch placements and existing brick BLASes. The forest TLAS
contains one bounds instance per world tree. The shadow shader queries the
candidate object's inner TLAS through a device-address table and generates an
outer hit only when its brick traversal finds an actual occluder. Empty gaps
inside an object's bounds therefore remain transparent. Several unique object
assemblies can share this interface. This is a generic rigid assembly API,
not a tree-only shadow special case.

The outer object ray remains unnormalized, preserving its world ray parameter
through nonuniform scales and reflections. The inner brick traversal retains
the existing volume optics, ray clipping and sampled transmittance. Sampling
includes the outer instance identity so overlapping trees receive independent
coverage decisions; repeated candidates remain stable. The first root retains
the flat-path seed for exact single-object diagnostics. Invalid publication
keeps the previous scene, empty publication clears it, and the existing frame
attachment retains inner structures and address buffers until GPU work ends.
The single-object build reuses the already-built local TLAS. No world tree
expands into a separate branch placement array.

The native shadow suite passes with zero Vulkan validation errors. New cases
cover exact flat/nested HDR agreement, reflected nonuniform transforms, two
overlapping roots, distinct assemblies, empty gaps, world-space ray limits,
invalid replacement and removal. Measured nested half-sheet transmittance is
.252083; two outer half-sheet roots produce .248646, both near the expected
.25. An analytic capacity test uses 250,000 roots and 64 shared parts, equivalent
to 16 million flat placements, at 75,387,184 resident bytes. Its 2.58573 ms
shadow timing describes stacked synthetic cells, not the real-tree workload.

The real pine retains 44,860 local shadow part placements and 13 voxel part
prototypes once. Across 250,000 roots this avoids materializing 11,215,000,000
branch placements. Resident shadow storage is 23,075,472 bytes for one tree,
23,383,568 bytes for 1,024 and 98,437,264 bytes for 250,000. This is shadow
storage only, not total renderer VRAM. The diagnostic still uses the .01 m
leaf shadow cut and .012 m clustered primary foliage from the prior checkpoint.

Seven hybrid previews completed, each with 64 frames and native screenshots
at frames 0, 31, 47 and 63. These use actual DLSS Quality, 667x667 internal /
1000x1000 output, +2 EV, overhead sun, opaque materials, .001 m light-ray bias,
zero triangle normal-offset cap and GI/volumetrics off on the RTX 4090. Every
successful hybrid preview reports zero Vulkan validation errors. The moving
views translate the camera 1.92 m. Forest placements form an 8 m regular grid
of one pine prototype; many trees are obscured or offscreen in the ground view.

| Population / view | Selection ms | Visibility ms | Shadow ms | Measured tree passes ms |
| --- | ---: | ---: | ---: | ---: |
| One tree, flat shadows | 2.1860 | 3.7417 | .2934 | 6.2256 |
| One tree, shared shadows | 2.1857 | 3.6854 | .2984 | 6.1645 |
| One tree, shared, moving | 1.9790 | 3.7048 | .2898 | 6.0360 |
| Four trees, shared | 1.9967 | 5.8778 | .4176 | 8.3413 |
| 1,024 trees, shared | 2.6468 | 6.2147 | .6290 | 9.4901 |
| 250,000 trees, shared | 2.4862 | 5.8532 | .5993 | 8.8553 |
| 250,000 trees, shared, moving | 2.6497 | 6.3293 | .6383 | 9.6132 |

These are medians of three warm captured GPU timestamps with validation
enabled, not sustained frame-rate measurements. The last column is the median
of each sample's selection + visibility + shadow; it need not equal the sum
of the other medians. Composite, DLSS, CPU and other unmeasured tree costs are
excluded. The similar 1,024/250,000 timings do not demonstrate constant cost:
views differ in distant occupancy, and primary rendering hits a geometry cap.

In particular, the static quarter-million run reports 91,562–92,800 primitive
budget fallbacks, about 33.38 million surface triangles and roughly 587,000
surface packets. The moving final capture has 90,846 primitive fallbacks.
No node budget fallbacks occur. A fallback count is a hierarchy decision count,
not a count of distinct trees. The current budget forces coarser representations
and cannot be treated as faithful source-detail rendering. The images contain
recognizable full tree silhouettes but retain grainy and softened foliage;
lighting, regular repetition and source fidelity still need work.

The flat/shared single-tree frame 0 is byte-identical. Accumulated frame 63 has
display RGB MAE .0000375425 and a maximum isolated channel difference of 32/255.
The tiny average does not make the accumulated images exact. An attempted
four-tree original-triangle reference failed before its first capture with
`VK_ERROR_DEVICE_LOST`; its cause is not established. The fault log and NVIDIA
crash dump are preserved. Four-tree source equivalence is therefore unverified;
do not infer it from the successful hybrid capture. The earlier one-tree
source-quality limitations remain applicable.

A subsequent one-tree original-triangle reference using the final shared-shadow
binary completed all 64 frames, produced four captures and reported validation
0. Its overall silhouette is close to the hybrid single tree, with foliage and
wood differences still visible. The matching display-space foliage-mask error
is recorded in `shared-shadow-measurements.json`; it is not appearance
acceptance. The first attempt to launch this single case had a PowerShell array
unrolling error and exited on invalid arguments; the corrected script completed.

Build evidence is `shared-shadow-complete-build.log` and the subsequent preview
build `shared-shadow-preview-final-build.log`; native suite evidence is
`shared-shadow-tests.log` and `.stderr.log`. Earlier failed builds exposed GLSL
opaque-type argument restrictions and a move-only acceleration resource; the
final optimized shader/build corrects both. No optimization or signed-runtime
verification was disabled. The GPU runtime still reports stale external overlay
paths and an unavailable optional low-latency module; actual DLSS is asserted.

Evidence is under `C:/tmp/conifer-forest/`: `shared-shadows/` native captures,
`shared-shadows-*.log`, run scripts, `measure-shared-shadows.py`,
`shared-shadow-measurements.json`, `shared-shadow-evidence.json`, and archived
source/binaries under `shared-shadow-checkpoint/`. Prior checkpoint manifests
are unchanged. Graft was attempted twice this turn; both calls timed out after
8 seconds and reported no token savings.

Next work should reduce visible geometry and hierarchy selection costs while
improving source fidelity: integrate multiple source-derived textured foliage
LODs and source-derived wood simplification, select them by measured error,
and avoid resolving excessive tiny detail before budget fallback. Shared
shadows now work in the preview; provider/editor publication, sustained complete
GPU attribution and wider mixed-variant forest views remain necessary. Near
foliage quality, bark detail integration, wind and biological validation are
still unfinished. The four-tree original-triangle failure also needs diagnosis
before that reference mode can be relied on.

## Measured foliage LOD ladder checkpoint (2026-09-13)

Previous goal turn classification: progress. Shared whole-tree shadow traversal
and the quarter-million-tree captures are complete. This pass adds reusable
`surface_proxy::bake_cluster_lods` and `cluster_lod_switch_distances` APIs and
integrates them into the native forest preview. The goal remains active.

Each foliage rung is fitted directly from the original geometry. Increasing
projection-error targets are validated; candidates that do not reduce triangle
count are omitted. A cumulative payload budget covers the returned triangle
and texture data. Failure preserves the previous entire ladder. Perspective
switches use measured plane displacement, the finest patch bounds used by the
GPU, source-radius padding and the off-axis perspective derivative. Distances
are rounded outward and expressed in canonical radius-normalized LOD units.
This bounds vertex projection onto planes, not filtered visibility, texture
error, lighting or source-image equivalence.

The preview's opt-in `MATTER_FOREST_FOLIAGE_LODS` specifies a pixel projection
target. Six candidate targets (.001/.004/.012/.03/.07/.15 m) with progressively
larger patch diameters produce four or five retained levels per shoot. The
four 5,800-triangle sources become 1490–1504 triangles at the finest texture
level and 184–210 at the coarsest. All retain 100 solid twig triangles per
shoot. Some loose fits produced more triangles; the compiler discards them.
These reusable levels are not yet published through the provider/editor.

CPU tests cover merging separated planes only at coarse detail, independently
projecting displaced vertices at viewport corners and instance scales .25/1/4,
canonical switch selection, rejecting malformed projection/error metadata,
discarding redundant rungs, and preserving publication after a cumulative
budget failure while adding a later rung.

The native preview initially received a 1000x820 framebuffer because Windows
constrained its window to the desktop work area. Those initial captures are
excluded and preserved under `foliage-lod-extent-mismatch/`; the second run was
explicitly stopped during forest setup. Explicit size limits followed by a
size request now enforce 1000x1000, with a framebuffer assertion before GPU
setup. The final matrix verifies 1000x1000 output / 667x667 internal throughout.
See the [GLFW size-limit documentation](https://www.glfw.org/docs/latest/window_guide.html#window_sizelimits).

Seven final 64-frame previews completed, each with four native screenshots
and zero Vulkan validation errors: fresh fixed-bake single/forest baselines,
one- and two-pixel single trees, one- and two-pixel forests, and a moving
two-pixel forest. They use actual DLSS Quality, the RTX 4090, +2 EV, shared
detailed shadows and GI/volumetrics off. Source geometry and shading are
unchanged. Each forest has 250,000 grid placements of the same pine; many are
offscreen or obscured.

| View | Selection ms | Visibility ms | Shadow ms | Measured passes ms |
| --- | ---: | ---: | ---: | ---: |
| Fixed bake, single | 1.9790 | 3.7089 | .3024 | 6.0767 |
| LOD target 1 px, single | 1.9937 | 3.5953 | .2892 | 5.8762 |
| LOD target 2 px, single | 1.9932 | 2.7730 | .2806 | 5.0572 |
| Fixed bake, forest | 2.5247 | 6.2587 | .6455 | 9.4264 |
| LOD target 1 px, forest | 2.5020 | 5.2204 | .6503 | 8.3491 |
| LOD target 2 px, forest | 2.4948 | 5.2388 | .6621 | 8.4240 |
| LOD target 2 px, moving forest | 2.7600 | 5.2429 | .6428 | 8.6302 |

These are medians of three warm captured timestamps, not sustained frame
times. The combined column is the median of per-sample sums. Composite, DLSS,
CPU and other unmeasured costs are excluded. Single-tree geometry drops from
19,804,728 triangles to 17,043,398 at 1 px and 13,910,518 at 2 px, with no budget
fallbacks. Both forest settings still fill the roughly 33.4-million-triangle
budget. One-pixel forest samples report about 233–234 thousand failed detail
refinements; two-pixel samples about 168 thousand. These count hierarchy
decisions, not distinct trees. No node-budget fallbacks occur. Cheaper coarse
leaves permit more topology expansion, so fallback counts cannot be compared
as if each run visited the same nodes.

The source-mask display RGB MAE is .05967 for the fixed single tree, .06010
for the one-pixel ladder and .06123 for two pixels. Corresponding masked 8x8
mean errors are .01903/.01959/.02078. Display luma remains near .351 versus
source .353. This excludes wood and dark pixels and is not a radiometric or
geometric guarantee. Single-tree silhouette is similar; forest foreground
branches appear more defined but grain and softened distant foliage remain.
No source-appearance or 5 ms acceptance flag is true.

Evidence: `foliage-lod-complete-build.log`, final preview window-size build
`foliage-lod-preview-size-build.log`, final CPU build/test logs,
`run-foliage-lod-matrix.ps1`, `foliage-lods-*.log`, 28 PNGs in `foliage-lods/`,
and `measure-foliage-lods.py` / `foliage-lod-measurements.json`, all under
`C:/tmp/conifer-forest/`. Graft's two discovery/call-graph attempts timed out
after eight seconds each and reported no savings.

The next architectural experiment should reuse the shared assembly structures
for primary visibility: query fixed textured surfaces for nearby geometry and
sparse aggregates at distance, working per pixel instead of submitting all
selected hidden triangles. Start with a matched textured-surface reference,
including texture footprints, normals, depth, picking and motion; then measure
the full population with shared outer roots. The existing shadow timing does
not establish the cost of closest-hit primary traversal. This remains an
experiment to implement and verify, not a performance claim. Source-derived
wood LODs, provider/editor publication, full-frame/tree-cost instrumentation,
mixed variants, bark, wind and biological validation remain unfinished.

## Shared primary surface queries checkpoint (2026-09-13)

The architectural experiment is implemented and exercised on the actual pine
forest. `VkSceneRenderer::set_shared_surfaces` publishes an immutable assembly
of fixed textured/solid surfaces and outer placements. Each part has a hardware
triangle BLAS, each assembly a local instance TLAS, and the forest an outer TLAS
over conservative assembly bounds. The fragment shader queries these shared
structures per pixel and writes the established depth, albedo, normal,
roughness, identity, motion and reactivity attachments. It does not expand each
world tree into its 44,860 parts or emit a primary triangle work list. Texture
footprints come from adjacent camera rays intersected with the hit triangle
plane; filtering uses the same bilinear/trilinear implementation as raster.
Alpha cutouts support deterministic native cutoff and temporally sampled
coverage. World-root samples are decorrelated, while rejected temporal attempts
retain their sample sequence. Transforming inner rays without normalization
preserves world hit distances through nonuniform and reflected placements.

This checkpoint uses original solid wood/cone meshes plus fixed .012 m clustered
needle assets (392–396 triangles, including 100 solid twig triangles, per
5,800-triangle shoot source). There are 13 shared meshes, 1,025,546 unique
triangles and 44,860 local placements. Primary GPU residency is 287,694,892 bytes
for one tree and 403,056,524 for 250,000 roots. The separate shared fine sparse
shadow assembly remains 98,437,264 bytes. This primary implementation has no
distance voxel/mesh selection yet; it establishes shared surface traversal.
The existing authored hierarchy path and foliage LOD ladder remain available.

Native GPU tests pass with zero validation errors. They compare queried and
rasterized identical surfaces for affine/sheared local parts, nonuniform
reflected roots, cutouts revealing a rear surface, overlapping distinct
assemblies, empty gaps revealing other objects, ordinary-mesh foreground
occlusion, near/far clipping, failed publication and empty-scene replacement.
The eight comparison cases have zero coverage/identity mismatches and no color
differences beyond one byte; maximum depth delta is 5.36e-9. Direct normal and
roughness probes also pass. An initial .005 absolute HDR comparison failed
when one-byte albedo rounding was amplified by lighting (maximum .00586).
The corrected diagnostic independently checks stored normals/roughness and
compares lighting where stored albedo is identical; those comparisons pass.
The initial failing log is retained, not overwritten. Camera motion, jitter,
presentation retries, camera cuts, disocclusion and snapshot history also pass
for shared queries. Fractional needle coverage measures .24951 against .25098;
overlapping world roots measure .44124 against .43897 expected coverage.

The preview adds `queries` and `surfaces` modes using identical fixed assets,
plus an `empty` mode retaining the same ground and camera layout. The paired
64-frame single-tree screenshots look closely matched, but are not pixel
identical: foliage stochastic sequences differ. Masked display RGB MAE is
.05343 and masked 8x8 mean error .01074 between raster and query; mean foliage
luma .35220 versus .35082. Relative to the original triangle-source capture,
query MAE is .05968 and block error .01878. These display-space masks exclude
wood/dark pixels and are diagnostics, not appearance acceptance. Single-tree
visibility was about 1.3–1.6 ms versus roughly 25 ms for this fixed, original-
wood raster reference. This is not a comparison to the optimized hierarchy.

`MATTER_FOREST_PREVIEW_FRAMES=512` enables longer sampling. A separate timestamp
span encloses the recorded render work, including composite lighting, actual
DLSS and display conversion, before screenshot copies. It excludes CPU work,
presentation and readback. Four 512-frame runs completed with zero validation
errors: ground-only baseline, stationary forest, moving forest and stationary
repeat. Every run samples 56 frames at stride eight after excluding its first
64 frames. The moving camera travels 28.8 m by frame 511. These are short
multi-hundred-frame tests, not a multi-minute soak or end-to-end FPS result.

RTX 4090, 1000x1000 output / 667x667 internal, actual DLSS Quality, GI and
volumetrics off, +2 EV, existing overhead sun and shared fine sparse shadows:

| Run | Dedicated tree passes median ms | Recorded render median ms | Recorded render p95 ms | Recorded render max ms |
| --- | ---: | ---: | ---: | ---: |
| Empty ground | — | .31765 | .33872 | .36650 |
| 250k stationary | 3.17616 | 3.55301 | 3.78219 | 3.86442 |
| 250k moving | 3.22354 | 3.58928 | 4.18771 | 4.58608 |
| 250k stationary repeat | 3.14781 | 3.54757 | 3.84487 | 4.14749 |

Dedicated passes sum setup, primary visibility and sparse shadows per sample;
the table reports the median of those sums. Moving dedicated-pass p95 is
3.84718 ms and maximum 4.17152 ms. Forest-minus-empty differences between
independent recorded-render medians are 3.23536/3.27163/3.22992 ms. This is an
additional accounting cross-check, not paired exact cost attribution. The
measured scene clears 5 ms, including its complete recorded GPU render span,
but overall goal and appearance acceptance remain false. All 250,000 roots are
resident in an 8 m grid of one pine; many are obscured or outside the view.
There is no claim that all trees contribute visible pixels, nor that these
timings generalize to aerial views, other resolutions or other GPUs.

Evidence lives under `C:/tmp/conifer-forest/`: `surface-query-final-tests.log`,
the initial lighting-tolerance failure logs, build logs, the single/forest and
512-frame run scripts, `surface-queries/` screenshots, and
`measure-surface-queries.py` / `surface-query-measurements.json`. The immutable
source/binary/capture manifest is `surface-query-evidence.json`, with snapshots
under `surface-query-checkpoint/`. Earlier evidence manifests are unchanged.

Next: test elevated views and close branch cameras, native/higher resolution,
and deterministic variation before adopting the path in the editor/provider.
Use the verified surface traversal as the fidelity reference for adding
distance-selected surface LODs and sparse aggregate traversal. Preserve
source-like coverage and lighting at transitions; do not regain speed by
returning to ghostlike distant volumes. The remaining visual work includes
needle stability, bark and branch material quality, species variation,
redwoods/clumps, placement realism and wind. The new timing result establishes
useful headroom; it does not finish those requirements.

## 2026-09-14 — StreamMountain mixed forest integration

The mountain scene now uses four fixed `MountainEvergreen` assemblies: Scots
pine (14 m), silver fir (16 m), coast redwood (62 m), and a three-stem redwood
base (48 m nominal stems). Each receives deterministic yaw and 0.88–1.12
uniform scale. `shared-lib/mountain_forest.js` owns the catalog and habitat
selection. Redwood groves prefer moist low ground; slopes, treeline and forest
clearings mask density. Placement runs per fixed 64 m cell through terrain
bands 5..2, preserving positions and ownership across nested tile changes.
The old alpine conifer/deciduous/shrub/grass/flower/ground-cover scatter is gone.
The terrain, rock scatter and saved weather/lighting/LOD settings are retained.
The opening camera now faces the forest edge at (380,90,1600).

Production parts can opt into `static sharedSurfaces = true`. RNDR version 2
stores the flag and still reads version 1. PartStore compiles canonical source
geometry into immutable local assemblies: original wood/cone triangles,
clustered .012 m needle patches and sparse shadow coverage. It stops ordinary
child expansion at that boundary. Streamed roots are routed to shared primary
queries and shared sparse shadows; ordinary terrain still uses the usual
renderer. Local material identities and roughness survive the assembly.

Streaming publication updates only outer world placements while retaining
local BLASes, local TLASes, textures and geometry buffers. Empty catalog rows
are legal, invalid replacements retain the accepted scene, identical placements
are a no-op, and old GPU snapshots retain resources for in-flight submissions.
The engine also retains the CPU branch catalog across terrain updates rather
than copying hundreds of thousands of local poses for every tile change.

The first production bake exposed a pre-existing mismatch in the streaming
installer's child lookup: `eval_requires` emits short JSON decimals but the
placement binding formats doubles with `%.17g`. Its child table now uses the
same `params_to_json(params_from_json(...))` canonicalization as the ordinary
graph installer. All four tree variants then installed and pre-warmed.

Validation artifacts live in `C:/tmp/mountain-evergreens/`. The native editor
build, conifer anatomy tests, forest placement tests, ScriptHost suite, provider
suite and sparse Vulkan suite pass. New tests cover tile-partition invariance,
species availability, habitat exclusions, retained production assemblies,
material roughness, moving/removing roots, empty catalog rows, bounded resident
geometry, failed publication and full unload. Vulkan suite: zero validation
errors. The first mountain walkthrough loaded approximately 22k–33k tree roots
as the camera moved; these are streamed residency counts, not visible-tree
counts. Clear-weather and saved-weather screenshots are separate artifacts.

The earlier 250k-tree benchmark remains in its archived checkpoint. Its timing
is not a measurement of this mixed mountain scene. This integration uses fixed
source wood and fine needle surfaces with sparse shadows; it does not complete
the planned distance-selected sparse primary hierarchy or GI participation.
The shared query shader currently uses source geometry/color and local material
parameters; it does not yet sample the bark detail VT/POM pages. The scene binds
those existing detail materials without claiming that missing query feature.

Final production camera-movement verification is recorded in
`C:/tmp/mountain-evergreens/validation.json`.

The final build completed an 800 m camera move and return with all four
assemblies intact and no Vulkan validation errors, reaching 47,237 streamed
roots. Both 40 s idle waits expired while outer terrain continued filling;
subsequent frame barriers and screenshots completed. This is a streaming
walkthrough, not a settled performance benchmark. Source assets and compatible
texture caches were also seeded into the normal project cache, preserving
existing artifacts, so an ordinary StreamMountain launch can reuse the work.
