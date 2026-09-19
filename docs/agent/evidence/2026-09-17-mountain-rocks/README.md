# Mountain rocks: geometry, placement and reusable material work

The first twelve analytic rock prototypes and deterministic boulder/scree
placement are integrated into StreamMountain. This is an implementation
checkpoint; rock art, material density, contact blending and the expanded
environment goal remain unfinished.

## Geometry and placement

`MountainRock` emits clipped convex JS meshes in three families: fractured
blocks, bedded slabs and weathered boulders. Secondary clipping planes wear
actual shared edges. The twelve prototypes contain 2,988 triangles in total
(212–312 each), with closed outward geometry and a ground datum. Instances
choose stable prototypes and poses independently of streaming sector identity.
No physics, voxel meshing, retopology or raycast bake is used.

The JS subdivision test compares one square kilometre with 256 individual
64 m cells: all 1,689 placements match exactly, including burial and rotation.
Tier changes retain existing poses, and forest clearance includes neighboring
large rocks. The initial placement system still needs contact/art review and
infrastructure integration. Conservative exclusion can clear too much forest.

Skipping polygon reconstruction for faces wholly inside a clipping plane
reduced the native twelve-prototype batch from 8,237.004 ms (`mountain-rocks-v1`)
to 1,393.176 ms (`mountain-rocks-v2`). Maximum vertex displacement between those
versions is 1.1102230246251565e-15 m. Both native and JS geometry checks pass.
This measures the prototype batch, not full-scene startup.

## Native captures before the new material

Gallery v1 is rejected: FIFO screenshot paths containing spaces were truncated.
Its audit records missing outputs. The verified stray PNG and sidecar were
moved into v1 as `truncated-path-final-shot.png` and `.done`.

Gallery v2 uses paths without spaces. All twelve images pass capture/source/
binary audits, including four verified native RT views. The low-poly forms
remain visibly plain and blocky, with a strong raster/RT lighting difference.
They use the old rock material; this is geometry evidence, not final art.

[StreamMountain v9](../2026-09-17-vt-resolution/v9/audit.json) retains all 37
images, with four actual native RT images verified by the renderer-receipt
audit. Editor exit is zero, inputs and executable remain unchanged, props are
restored, and command/validation error counts are zero. Overview and grazing
images show the new boulders and surrounding forest clearings. Their plain,
pale material contrasts poorly with the terrain and needs the new local source.

Full-route development measurements versus v6 (same terrain source, changed
rocks/placement):

| Measurement | v6 | v9 |
| --- | ---: | ---: |
| Root setup | 54.391 s | 63.508 s |
| Initial fill, 2,586 sectors | 173.17 s | 183.31 s |
| Used page high water | 1,437 | 1,516 |
| Resident variants high water | 998 | 1,010 |
| Indirection storage | 36.96 MiB | 36.96 MiB |
| Overview G-buffer median | 33.210 ms | 31.906 ms |
| Grazing median | 18.061 ms | 19.447 ms |
| Cliff median | 27.313 ms | 27.679 ms |
| Oblique cliff median | 37.678 ms | 41.002 ms |
| Close median | 6.700 ms | 8.939 ms |

Root setup and initial fill overlap; do not add them. Each view has thirty
samples. No other editor was open; other GPU workloads were not controlled.
These results show a regression signal requiring attribution, not performance
acceptance or proof that one specific change caused the regression. v6's four
RT-labelled frames were rejected previously and are not RT comparison evidence.

## Next material and placement checkpoint

Implement generic `static surface(p)` for ordinary standalone parts using the
existing local surface language. Publish an immutable procedural recipe on both
cold and cached geometry paths, with no finite solid or source-image job. Bind
it for eager/demand VT and export. The new rock recipe adds continuous mineral
variation, grain, pits and shallow fissures with coherent channels and bounded
POM height. Native validation and capture results are recorded below.

The rock support-plane orientation also now accounts for the actual Ry*Rz*Rx
placement order. An independent analytic sloped-plane test checks the transformed
normal; the original approximation was wrong when both local slope components
were nonzero. JS rock and forest checks pass after this correction.

Part-local detail scales with the instance. A gallery at 256 local texels/metre
will be a controlled material review, not proof of adequate production density
for 16–36 m landmark rocks. Per-part density and world-space contact overlays
remain explicit requirements, alongside terrain seams, vegetation, roads,
houses, tunnels, power lines and the original performance targets.

## Local-source integration results

`local-rock-surface-v1` builds the generic declaration, provider preparation,
renderer binding and twelve rock recipes. Native recipe/provider/mountain
checks pass, as do the existing GPU direct-source, composed-POM and immutable
input tests. Every rock recipe packs 75 operations. The sampled local relief
is nonflat and remains inside its declared [-0.0046, 0] metre envelope; coarse
footprints return the stable -0.001 metre datum.

`local-rock-surface-v2` adds an actual renderer publication test: the receiver
starts with no surface tape or weights and obtains them from the prepared part
source. Raster and native RT agree with the analytical color/roughness and
height-normal oracle (maximum normal-component error 0.003922). Identical live
publication is accepted; changing an existing owner's immutable source is
rejected. The test passes with zero Vulkan validation errors.

### Rejected gallery, then retained material owners

[Gallery v3](v3/review-audit.json) has 28 valid image captures but is rejected
as material evidence. Its parent flattened all twelve child meshes into one
mesh. The child recipes were published but never bound to that new owner;
raw albedo stayed uniform and VT reported only one variant. The original
capture audit remains intact; the review audit records the semantic failure.

`local-rock-surface-v3` makes explicit `inlineBelowPx:0` preserve a child as a
permanent instance. Omitting the threshold retains 64 px. Permanent references
are separate from the coarse cutover of other hinted children, and retain
their local geometry/material owner at every distance. Both gallery and
StreamMountain rock placements use this contract. Default parent LOD ladders
support it; the authored/budget full-gather paths explicitly reject it because
those paths cannot yet preserve the references. Child LOD ladders remain usable.
The complete native flattening suite passes.

The first assembly test had an invalid `requires` fixture and failed before
placing its children. Its failed record is retained. `local-rock-surface-v4`
corrects the fixture and verifies two permanent instances of one textured rock
alongside a third, distance-limited plain child. Both textured references retain
their transforms and shared owner with zero cutover. Cold/warm publication,
source edits, atomic failure, and the actual mountain material checks pass.
This last revision changes only the test fixture; the renderer is unchanged.

[Gallery v4](v4/audit.json) passes all 28 captures, source/binary identity,
props restoration, and the new material-owner check: thirteen resident owners
for twelve rocks plus the ground. Eight shots are independently verified as
native RT. Raw albedo now shows the authored mineral variation; close views
show grain, pits and fissures. POM-on/off differences exceed one byte on
1.64–2.28% of the entire close-view raster frames and 1.91–2.63% in RT. These
are image differences, not a depth oracle; native analytic tests cover depth
and normal semantics separately.

![Native rock material review](v4/block-rt-lit.png)

This is a working material path, not final rock appearance. The shapes remain
blocky, fissures form overly regular loops, and lighting remains strongly blue
with a visible raster/RT brightness difference. The gallery requests 256 local
texels/metre; production props still default to 16. Part charts regenerate in
memory on load, so the proof density does not persist into a later production
session. Per-part density control and scale-aware microdetail need work before
the large boulders can meet the close-view goal. A full mountain capture of the
new material/instance implementation is still required; v9 predates it.

## Authored density and quieter weathering

`rock-density-v3` adds `static vtTexelsPerMeter` as an ordinary part declaration
(number or parameterized method, 1–2048 local t/m). RNDR v3 carries the value;
omitting it retains the exact v2 bytes and existing default. All three flat
chart builders and the shared disk/memory staging path consume the owner
setting. Terrain keeps its world setting. Merged geometry uses its parent
material owner's setting; retained child instances use their own. The
diagnostic override does not alter the persisted value, and malformed
overrides fall back to the asset setting.

The native provider and PartStore suites pass, covering cold/warm publication,
parameter merging, density edits, cached/retained/runtime-only chart equality,
legacy v1/v2 records, malformed v3 values, numeric and bounded method
declarations, terrain precedence, clustered/legacy flat paths and parent/child
ownership. Snapshot equality now checks shared-surface and density metadata.
The initial provider fixture omitted its output directory; the failed v1 log
is retained. v3 creates the fixture directory and passes. The v1 build driver
terminated with code 143 after two targets; a process inventory found no live
compiler/linker. v2 completed all four targets, and v3 rebuilt the corrected
tests and unchanged editor. These are native MSVC builds.

`MountainRock` now requests 256 local t/m in production. Weathered boulders
clip against an ellipsoid envelope, reducing the large rectangular remnants
of the box-based prototype. Fissures are sparser and have less contrast and
depth. All twelve shapes remain closed, outward, bounded meshes; 3,148 total
triangles versus 2,988 previously. The 1 km² scatter remains exactly 1,689
deterministic placements. Native prototype baking took 1,389.689 ms in this
single development sample; this is not a full-scene startup measurement.
Recipes still pack 75 operations, with sampled local heights from -2.552 to
-0.448 mm inside the declared conservative envelope.

[Gallery v5](v5/audit.json) runs **without a density override**. All 28 captures,
source/binary checks, thirteen material owners and zero Vulkan validation
errors pass. [Renderer receipts](v5/renderer-audit.json) confirm eight actual
native-RT captures. The reviewed weathered view has a less rectangular outline
and quieter fissures. Strong blue lighting, raster/RT brightness differences,
visible planar faces, large-instance microdetail scale and ground contact
blending still prevent final appearance approval.

![Authored-density weathered rock, native raster](v5/weathered-lit.png)

### Streamed asset integration

The [v10 mountain capture](../2026-09-17-vt-resolution/v10/review-audit.json)
has 37 valid images and four confirmed native-RT frames, but fails rock material
acceptance. Its overview albedo shows uniformly pale rocks; no local recipe
publication is logged. `WorldSession::Impl::install_world` uses its own asset
dependency walk and direct `HostBaker` calls, bypassing the provider's ordinary
graph material service. Its timing data describes the fallback material, even
though the new geometry and density metadata were present.

`stream-rock-source-v1` connects this installer to the same
`LocalProvider::ensure_part_surface` service used by ordinary scenes, on both
fresh geometry and cache hits. Failed preparation aborts installation with a
material error. World surface receivers reject conflicting local part sources.
The native provider suite now includes an asset outside the graph bake plan,
cold/warm geometry, one queued publication per shared asset, reuse and failure
propagation. It passes; the native editor build also passes. A full-scene repeat
is required before this is accepted as a rendered integration fix.

[Mountain v11](../2026-09-17-vt-resolution/v11/audit.json) completes that repeat:
37 captures, four actual native-RT frames, no validation errors, and unchanged
source/binary hashes. All twelve published source identities match the gallery.
The [material integration audit](../2026-09-17-vt-resolution/v11/material-integration-audit.json)
checks reviewed raster albedo regions. The large boulder changed from a constant
RGB (139,135,127) to mean (33.81,32.12,29.65) with spatial variation; a neighboring
terrain control remained pixel-identical. This proves the streamed-source fix.
The full-scene RT views focus on terrain; rock-focused RT evidence remains the
separate gallery. Final art, motion, scale-aware detail and contacts are open.

![Streaming Mountains with the local rock materials](../2026-09-17-vt-resolution/v11/overview-lit.png)

### Development costs, with materials actually active

v11 root setup took 53.635 s; initial fill took 171.87 s for 2,586 sectors.
Those clocks overlap and must not be added. v10 took 56.992/172.32 s; v9 took
63.508/183.31 s. These are sequential development runs with different cache
histories, not an isolated speedup result. No other editor was open; other GPU
workloads were not controlled.

VT peaked at 1,486 resident pages, 1,010 variants and 36.96 MiB of indirection
memory, with zero evictions. Those high-water values match v10; v9 used 1,516
pages with the same variant count and indirection memory.

Thirty-sample G-buffer medians (milliseconds):

| View | v9 | v10 fallback material | v11 local material |
| --- | ---: | ---: | ---: |
| Overview | 31.906 | 32.876 | 32.348 |
| Grazing | 19.447 | 18.591 | 21.417 |
| Cliff | 27.680 | 24.443 | 28.182 |
| Oblique cliff | 41.002 | 38.403 | 40.326 |
| Close ground | 8.939 | 8.374 | 8.731 |

Performance is mixed. Grazing is 10.1% slower than v9 and 15.2% slower than
v10; the v11 cliff measurement is also 15.3% above v10. This warrants a matched
repeat and POM work attribution. It does not establish that every difference
comes from the source fix. The runtime performance goal remains open.
# Physical size classes and density recovery

The `rock-metric-v2` build introduces four physical reference sizes (0.5, 2, 8,
32 m) per silhouette, for 48 shared prototypes. Local grain and relief retain
their metre dimensions; only broad mineral variation follows the rock size.
Scatter applies a residual scale between 0.5 and 2. The JS test preserves all
1,689 world placements over 1 km², including shape, rotation, burial and
clearance; the twelve normalized silhouettes are unchanged (12,592 triangles
across the full library). No physics or source-image baking is added.

The initial native run, `rock-metric-v1`, correctly **fails**: the largest
comparison boulder requests 192 t/m but atlas packing drops it to 96. The old
fallback halves density after any packing failure. The revised chart builder
keeps successful requests unchanged and makes six bounded intermediate probes
after finding a fit. It restores the complete accepted packing before writing
UVs. The 16K virtual-atlas limit and physical cache capacity are unchanged.

Native `mountain`, `chart` and `partstore` suites all pass on `rock-metric-v2`,
with unchanged source and binary hashes during each run. Density is now checked
on **all 48** prototypes. The 32 m class retains 157.5–192 t/m, including 189
t/m for the comparison boulder. The smaller classes retain 192. The analytical
40 m plane fixture recovers 408 from a 512 request instead of falling to 256;
the 32 m cube recovers 166.5 from 192 instead of 96. Both aligned and compact
charts pass coverage, border, overlap, projection and deterministic-repeat gates.
The 48 native geometry bakes take 5.898 s, with a further 103.8 ms staging all
48 chart ladders in this test. These are test costs, not full-scene startup.

## Scale proof

[scale-v1](scale-v1/audit.json) and [scale-v2](scale-v2/audit.json) each contain
63 native images, including 18 confirmed native-RT captures. Both exit 0 with
four direct source publications, no missing shots, no validation/command
errors and no source/binary changes. No density environment override is used.
The near and grazing cameras stay at fixed physical distances from matching
surface faces in every size class.

[Material analysis](scale-v2/material-analysis.json) measures a central 200×200
pixel region. The largest boulder's median effective proxy density rises from
about 93 to 185 t/m, matching the smaller comparison rocks; the diagnostic PNG
quantizes density in roughly 3.3% steps. This validates the runtime density
path, not final displaced coordinates or appearance quality.

**Art acceptance remains open.** Close-up texture is still soft, and the raster
POM-on/off difference is small. The largest grazing region changes by 0.073
byte levels on average after the packing fix versus 0.0019 before; that proves
an effect, not correct displacement. The inspection face normal dotted with
the comparison direction-to-sun is -0.612, so these views are in blue sky
shadow. A separate directly lit control uses sun azimuth -120/elevation 25
degrees (dot 0.833) before drawing conclusions about material albedo or a
lighting defect. The first attempt (`sunlit-v1`) is rejected: the editor clamped
240 degrees to 180, and desktop decoration limits reduced the framebuffer to
1156×694. `sunlit-v2` repeats at -120 degrees with the undecorated trace window;
all 32 captures are 1280×800, including 16 confirmed native RT images. Capture
audits now check the actual sun acknowledgement and image dimensions.

## Integrated size-class validation

The full StreamMountain `v12` run exits 0 with 37 audited 1280×800 captures,
including four native RT images, no validation/command failures and unchanged
sources/binary. It publishes 48 sources, including all four scale-proof owners.
The landmark's raw material changes as expected while the selected terrain
control remains identical. See its [material audit](../2026-09-17-vt-resolution/v12/material-integration-audit.json).

[Comparison with v11](metric-stream-comparison.json): peak VT pages 1,486 →
1,557; variants 1,010 → 1,038; indirection 36.96 → 48.36 MiB; zero evictions.
G-buffer medians (ms) are overview 32.348 → 33.299, grazing 21.417 → 19.216,
cliff 28.182 → 25.776, cliff-grazing 40.326 → 38.064, close 8.731 → 8.645.
Root setup rises from 53.635 to 75.045 s; initial fill is 171.87 → 173.13 s.
Those startup clocks overlap and must not be added. Cache histories differ and
other GPU workloads are uncontrolled, so these are development measurements,
not evidence of a causal speedup.

The bake trace exposes an avoidable cost: budget-based LODs run the same
analytic generator again without reducing geometry. v12 has 197 `part-bake`
trace nodes versus v11's 17, with 19.053 s inside those nodes; the budgets also
leave the full and coarsest library at the same 12,592 triangles. This motivates
deriving coarser meshes from one detailed source instead of rebuilding it.

## Resolvable relief and derived geometry LODs

`rock-relief-v2` requests the same density and keeps the material at 75
operations. Pits expand from 37 to 21 noise cycles/m and use the medium feature
filter; fine grain retains its own filter. Erosion, pits and fissures now carry
millimetre-to-centimetre relief inside a conservative [-0.018,0] m envelope.
The native check requires at least 3 mm of variation at a 6.5 mm production
footprint, bounded physical height, identical heights across reference sizes,
and a stable unresolved datum. All 48 materials pass.

`MountainRock` uses `LOD.decimate` at radius divisors 128 and 32. Native checks
exercise actual authored flat artifacts for all three families at every size:
240 → 172 → 80 triangles for the block, 228 → 176 → 60 for the slab, and
324 → 276 → 172–174 for the boulder. The detailed mesh is unchanged, all three
levels refer to one source bake, and every representation retains valid VT
charts. The JS placement/silhouette checks also pass.

The first LOD test incorrectly used the compositional worker loader instead
of the flat-asset loader; its rejected run remains `rock-relief-v1`. The corrected
test uses `get_or_load`, matching streamed asset installation. Production's
experimental `MATTER_VT_UNIFY` switch is off: the new geometry levels currently
have separate chart parameterisations. Prepared recipes remain shared by each
prototype, but cross-LOD page reuse and moving-camera stability are **not**
established by this native check and remain explicit follow-ups.

`relief-validation-v2-result.json` records completion of both captures and their
renderer/analysis audits. `sunlit-v3` has 32 images, including 16 verified native
RT views. At grazing incidence the central POM-on/off mean byte differences
increase from 0.136 to 1.525 (2 m), 0.178 to 2.066 (8 m), and 0.110 to 1.800
(32 m). This proves a more visible effect, not analytic depth or visual approval.
Close-up softness, angular silhouettes and lighting still need art review.

Full-scene `v13` has 37 images, including four verified native RT views, at
1280×800. No missing shots, command/validation errors, source changes or binary
changes occurred. Root setup is 68.844 s versus v12's 75.045 s; initial fill is
175.99 s versus 173.13 s. These intervals overlap. The bake trace removes 96
redundant analytic generator runs; accumulated part-bake time falls from
19.053 s to 7.759 s, while chart/flatten and publication costs increase. Nested
trace durations are not additive. Coarsest library geometry falls from 12,592
to 5,068 triangles with the full-resolution geometry unchanged.

| Development measurement | v12 | v13 |
| --- | ---: | ---: |
| Used pages high water | 1,557 | 1,505 |
| Resident variants high water | 1,038 | 1,055 |
| Indirection storage | 48.36 MiB | 49.24 MiB |
| Overview G-buffer median | 33.299 ms | 33.867 ms |
| Grazing median | 19.216 ms | 20.846 ms |
| Cliff median | 25.776 ms | 25.348 ms |
| Oblique cliff median | 38.064 ms | 37.570 ms |
| Close median | 8.645 ms | 9.026 ms |

Thirty samples per view; zero evictions/rejections. Cache history and other GPU
workloads are not controlled. v13 also enables VT trace instrumentation to
guarantee an undecorated, exact-size framebuffer. These mixed results are not
an overall performance pass or a causal comparison of frame time.

`motion-v1-result.json` records successful 900-pose out-and-back paths in raster
and native RT. The actual GPU draw trace for the largest rock traverses
0→1→2→1→0 (raster frames 117, 314, 586, 783), with no LOD trace errors,
Vulkan errors, fill failures, evictions or dropped requests. Peak pages are
118; the oldest mandatory request reaches nine frames in raster and ten in RT.
All 42 screenshots have verified render paths and dimensions. They sample the
outward leg and early return, **not the final LOD0 return**; the full return is
proved by the GPU trace only. These distant, dark views cannot establish close
material continuity or absence of flashing. Cross-LOD page reuse and visual
transition acceptance remain open.

Terrain POM continuity, rock art/contact overlays, vegetation expansion,
infrastructure, remaining wall work, lighting parity, performance acceptance
and user visual approval are still unfinished. The full goal remains active.
