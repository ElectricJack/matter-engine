# Separate receiver coverage from shared VT material pixels

Status: the fixed physical chart grid and bounded source-candidate filtering
produce **54 shared material references** in eight passing real-wall views.
The final pass uses 141 unique material allocations versus 183 before this work,
with about 9.8% less occupied channel payload after receiver padding is counted.
Reserved GPU memory is unchanged. Production periodic wall reuse remains incomplete.
Seven native terrain views pass for the earlier storage foundation.

## Fixed physical chart grid

`ChartBakeOptions::align_material_grid` anchors the projected origin of large
two-triangle charts to a physical 128-texel grid, independently of wall bounds.
Chart origins and vertex UVs move together, preserving geometry and material
scale. Leading padding is bounded to less than one payload page on each axis.
Small faces and complex charts retain compact packing. Runtime prop loading
enables this; terrain and callers using the default options remain unchanged.
The enabled callers regenerate charts in `PartStore`; serialized source
geometry and default bake parameterisation are unchanged, so no artifact
version bump is needed for this runtime chart policy.

The chart suite now has a native CMake target. It exercises translated walls
with negative coordinates, both front/back normals, repeated builds, UV/chart
projection agreement, ordinary coverage/gutter/distortion gates, compact
fallback and exact material-page identities across different sizes. Both larger
fixtures reuse all 18 eligible pages on each face of the smallest fixture.
Chart pages are 106→106, 206→206 and 286→302; packed atlas pages are 121→121,
225→225 and 306→342. These are synthetic translated fixtures, not a measurement
of the authored scene's occupancy.

`grid-v1-tests.json` records nine passing native checks: chart atlas, residency,
compositor and the six GPU modes listed below, with zero Vulkan validation
errors and no RT skips. Its editor link hit LNK1104; a process inspection found
no running editor. The unchanged-source `grid-v2` retry built all five targets.
`grid-v2-carried-checks.json` verifies identical source hashes and all four
tested executable hashes before carrying those results forward.

The first aligned capture,
[`grid-shared-walls-v1`](../2026-09-15-wall-surface/grid-shared-walls-v1-result.json),
passes all eight views with unchanged sources/binary, empty recorded queues,
no evictions/rejections and no validation/capture errors. Close and grazing
inspection shows preserved brick relief; the close POM status is fully green.
It uses **195 receiver pages / 189 material allocations / six shared references**,
versus 183/183/zero before alignment. Actual packed atlas dimensions are
unchanged. Thus six duplicate payloads were removed, but twelve extra receiver
pages were requested: this pass is **not a net occupied-memory improvement**.
Reserved pool size remains 4,064 MiB and is unaffected by sharing.

### Final source-candidate filtering result

Spatial-index cell membership no longer creates a material dependency when a
source cannot affect any stored page texel. The conservative bound includes
every finite mip's possible filter support and an FP guard; touching or uncertain
cases remain candidates. Candidate order is preserved. The native GPU fixture
deliberately gives one receiver a coarser index with an irrelevant distant stamp:
all four encoded channels and gutters still match byte-for-byte and share a key.
A second fixture moves that stamp so only its filter fringe reaches the page;
the output height and key must change. Both pass.

`grid-v3-build-manifest.json` records five successful native MSVC builds.
`grid-v3-tests.json` records all nine checks passing with unchanged source and
test executable hashes, zero GPU validation errors and no RT skips.
The final
[`grid-shared-walls-v2` capture](../2026-09-15-wall-surface/grid-shared-walls-v2-result.json)
passes all eight views, exits normally and retains empty recorded queues,
zero evictions/rejections and unchanged source/binary hashes.

| Capture | Receiver pages | Material allocations | Shared references |
| --- | ---: | ---: | ---: |
| Before grid alignment | 183 | 183 | 0 |
| Grid alignment only | 195 | 189 | 6 |
| Grid plus candidate filtering | 195 | 141 | 54 |

`grid-v3-wall-storage-comparison.json` records the raw counters and calculation:
136² × (4 × receiver pages + 5 × material allocations). Final occupied channel
payload is 27,466,560 bytes versus 30,462,912 before alignment, a 2,996,352-byte
(9.84%) decrease. This excludes metadata/retirement and is not reserved GPU
memory: the configured 4,064-MiB image pool remains unchanged. Material
allocations alone decrease 22.95%. Atlas dimensions and the 36-triangle
receiver geometry are unchanged; extra requested coverage pages are included
in the comparison. These static camera stops do not establish frame-time or
moving-camera acceptance.

Close, grazing and RT inspection retains brick relief without a newly visible
page seam. `grid-v3-image-diff.json` records the image comparison against the
grid-only capture: group, left close and both POM diagnostics are identical;
other raster mean absolute channel errors are below 0.00055/255. One pixel in
the right close view has a larger change (maximum channel difference 68).
The RT mean is 0.0663/255, maximum 7. The rendered scene is therefore not claimed
pixel-identical or visually approved; the exact-byte acceptance belongs to the
isolated GPU payload fixture. The fully green close POM diagnostic is unchanged.

Remaining work: periodic module mapping across arbitrary wall extents and mips,
lookup before composition to avoid repeated generation, sparse weathering
overrides/local dirty bounds, maze/corner/curve and moving-camera acceptance,
then terrain/contact blending and the original performance gates.

## Canonical material-coordinate integration

Fully covered planar rectangular pages now evaluate material position from an
explicit origin and two texel-step vectors. This avoids triangle-size-dependent
barycentric rounding. Chart coverage, AUX and POM traversal remain private.
An ordered, bounded page candidate list also makes finite-stamp accumulation
independent of the receiver's spatial-grid dimensions. The producer supplies
an identity only after proving full stored-page coverage, including gutters,
and validating the material inputs, frame, height decode and source bindings.
Boundaries, curved geometry, vertex fields and incompatible sampling phases
retain private ownership.

`canonical-v2-tests.json` records CPU and compositor passes with unchanged
sources/binaries and zero GPU validation errors. The native comparison uses
different rectangle widths and atlas locations, plus an irrelevant extra stamp
on the larger receiver. Both pages contain source relief and recessed base;
all four encoded material channels, including gutters, match byte-for-byte.
Shifting the physical sample phase produces a different identity. The first
build attempt (`canonical-v1`) caught a Windows `far` macro collision in a test
variable; its failed log is retained separately.

Installed enrichment with a zero-page budget may now share pixels. Enabling
the budget invalidates shared content for private replacement before enrichment
writes. The native publication fixture passes: four receiver pages first share
one allocation, then use four private allocations, with four distinct enrichment
targets on the following frame. The earlier v2 fixture incorrectly expected
enrichment in the same frame as publication; its failing result is retained.
Only that test changed for v3. `canonical-v3-carried-checks.json` verifies the
CPU/compositor/editor binaries are identical to v2.

`canonical-v3-tests.json` records six passing native smoke modes with zero
validation errors and no RT skips: `vt-queue`, `vt-composed-seam`,
`vt-composed-parallax`, `vt-input-snapshot`, `vt-direct-source`, and
`surface-parallax`. The CPU/compositor passes are retained in
`canonical-v2-tests.json`; the source and build manifests identify both sets.

This step does not implement arbitrary periodic remapping or early lookup to
skip composition. Whole-program weathering and whole source-bank dependencies
still constrain sharing. New `STATSVT` counters distinguish material allocations
and saved duplicate references from receiver occupancy.

### Pre-grid wall result and diagnosis

`SharedBrickWallProof` contains 8×12, 12×16 and 16×20 brick/course layouts,
represented by three boxes (36 raster triangles). The
[`canonical-shared-walls-v4` result](../2026-09-15-wall-surface/canonical-shared-walls-v4-result.json)
records eight native views, including matching close views of all three walls,
grazing, RT, POM status and chart IDs. Sources and binary remained unchanged;
there were no validation/capture errors, evictions or rejected variants, and
every recorded VT queue was empty. The editor exited normally.

The final capture uses **183 receiver pages and 183 material allocations**,
with **zero saved duplicate references**. Thus the isolated byte-identity test
does not establish sharing for actual authored walls. The shaded close view
contains brick relief, and its full-green diagnostic reports successful POM
throughout the visible face; this is not general visual approval. The first v3
capture falsely rejected uniform diagnostic images. The v4 harness accepts a
uniform green success status and a single chart while retaining the shaded
image presence checks; the earlier failed result is preserved.

Source inspection identifies a concrete phase mismatch: `plane_basis(+Z)`
uses `T=-Y`, while `build_chart_rung` anchors its origin at the minimum projected
extent, which is `-wallHeight`. Different heights shift the physical sampling
grid even though packed chart rectangles are page-aligned. The source-derived
calculation in `canonical-v3-wall-grid-diagnosis.json` records distinct phases
at 512 texels/m for all three heights. It is not an extracted GPU chart dump and
does not exclude further candidate/input mismatches.

This checkpoint motivated the physical grid and candidate filtering described
above. Their keys retain exact material inputs rather than rounding unequal
payloads together. Periodic module identity, early composition reuse, sparse
weathering and full acceptance remain required afterward.

## Change

Indirection continues to select one receiver page. Its AUX coverage/chart IDs
and POM geometry remain private, while its metadata now selects an independent
material allocation for color, normal, ORM and height. Both ordinary sampling
and connected-POM filter taps use that material address. Geometry and source
lifetimes retain the existing queue-ordered publication and retirement rules.

A producer may supply a 128-bit identity only when all four encoded material
channels, including their gutters, are identical. Shared source images alone
are insufficient: material coordinates, phase, transforms, mip/filter footprint
and dependencies must establish equal results. Zero retains private ownership.
Height decode and a non-recycled input-snapshot identity also qualify sharing.
At the `pixels-v2` foundation checkpoint the ordinary compositor supplied zero.
The canonical integration above now supplies identities for proven interior
pages; that does not establish complete periodic sharing in the maze.

The bounded material allocator shares complete payloads, splits on edits, and
rejoins matching payloads when an override is removed. A deleted receiver's
reader horizon protects shared pixels even if a remaining receiver edits them.
Failed allocation, producer refusal and stale generations retain the previous
complete surface. Geometry-dependent tier-2 ORM enrichment keeps private
payloads; sharing that result requires an explicit context-specific override.

## Costs and limits

- Receiver coverage uses the existing RGBA8 AUX channel (4 bytes/texel).
- Shareable color/normal/ORM/height total 5 bytes/texel in the current formats.
- Metadata grows from 48 to 64 bytes per configured page slot.
- Image pool reservation and receiver-page capacity are unchanged. Reduced
  unique material allocations are **not** a reduction in reserved GPU memory.
- Producers still execute before publication resolves an identity. Avoiding
  repeated composition requires earlier canonical module/page lookup.
- The renderer can install an enricher even with a zero-page enrichment budget.
  The canonical integration permits sharing there and checks that subsequent
  enabling of enrichment makes shared content private before writing it.
- Arbitrary wall charts do not yet map to a shared periodic material grid.
  Different grid origins cannot be aliased merely because they use the same
  bricks. Sparse weathering placement and local dirty bounds remain open.

## Validation

`pixels-v1-cpu-test.log`: native MSVC `vt_residency_tests`, ALL PASS. Covers
independent receiver/material indices, sharing, copy-on-write, override removal,
owner deletion, retirement, exhausted-pool fallback, input identity and height
decode. Version 2 removes a full-capacity scan from retirement collection;
its native results are recorded separately.

The GPU publication fixture now requests four receiver pages with identical
material pixels but distinct receiver AUX and geometry metadata. It checks
shared storage, independent appearance edits, refused/stale writes, input-bank
reuse, and multiple frame records in one submission. Its geometry addresses are
synthetic and are never dereferenced; the composed-POM tests separately exercise
real geometry and deliberately different material/coverage addresses.

Build/source/test manifests distinguish each checked binary and source set.
No visual approval or full performance acceptance is claimed here.

### Version 2 native results

MSVC `RelWithDebInfo` builds of `vt_residency_tests`, `vulkan_smoke_tests` and
`matter_editor` completed with exit 0. CPU tests pass. These GPU modes each
report ALL PASS and zero validation errors, without RT skips:

- `vt-queue` (includes sharing, copy-on-write and immutable publication).
- `vt-composed-seam` (flat, folded and diagonal chart boundaries plus gaps).
- `vt-composed-parallax`.
- `vt-input-snapshot`.
- `vt-direct-source`.
- `surface-parallax`.

The 90-degree fold has actual crossings and maximum position error 1.06 micrometres;
the two diagonal directions remain fully displaced with maximum depth errors
0.47 and 0.71 micrometres. Geometry coverage in these sampling tests is real;
their material allocation deliberately differs from the receiver AUX slot.
All 915 captured source hashes and the smoke binary remained unchanged during
the tests. See `pixels-v2-build-manifest.json`, `pixels-v2-tests.json` and the
individual raw logs. Rerun with `run_checks.py pixels-v2 <mode>...` against the
recorded source set; the harness refuses changed sources and stops on failure.

### Terrain capture

`../2026-09-15-material-look/terrain-shared-pixels-v2-result.json` records seven
native views (close/middle/far/grazing/RT/POM status/chart IDs), unchanged sources
and editor binary, no validation/capture errors, zero rejected variants or
evictions, and an empty VT queue at every view. The editor exited normally.

The matched status comparison against `terrain-connected-pom-v7` is exact in
all four diagnostic classes: 776,314 displaced green pixels, zero red or orange
fallback pixels and 7,265 blue travel-limit pixels in both captures. This retains
the earlier diagonal-chart fix; it does not establish complete terrain art,
streamed-boundary correctness, visual approval or performance acceptance.
See `pixels-v2-terrain-comparison.json` and the scene capture's source/result
manifests. The terrain remains an early material proof requiring visual work.

## Next required integration

1. Add canonical periodic material coordinates/page lookup that works across
   different receiver chart origins and wall sizes; do not alias misaligned
   blocks based solely on brick/source identity.
2. Supply validated producer identities and reuse before repeated composition.
3. Keep context-dependent weathering/enrichment in sparse overrides, with local
   dirty bounds and source invalidation; handle enrichment budget changes.
4. Demonstrate actual different wall geometries sharing material pixels, real
   POM traversal for each owner, independent edits and owner deletion. The current
   synthetic publication test plus private-payload traversal test is groundwork,
   not that combined wall acceptance.
5. Measure unique material/coverage/override storage separately from reserve,
   then satisfy the original memory, edit-latency and frame-time gates.
