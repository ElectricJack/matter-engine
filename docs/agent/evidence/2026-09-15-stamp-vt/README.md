# Finite brick sources through VT — 2026-09-15

**Status: first C++ source binding and native brick-page composition pass.**
The JS-authored clay sources now pass through the actual VT compositor and
BC7/BC5/R16 page outputs. The production provider/JS connection to low-poly
whole-brick walls remains open; `ClayBrickWall` still uses its inspection meshes.

## Visible result

These are plots of native GPU channel readbacks, not editor screenshots.

- [Color compressor comparison](vt-compression-comparison.png): identical
  source and mortar, before/after the endpoint-fit correction.
- [Color, normal and height across VT pages](vt-clay-channels.png): dashed
  lines mark page boundaries; depth is relative to the receiver plane.
- [Eight composed clay variants](vt-clay-variants.png).

Each front face occupies a 256×96 source grid at 1 mm/texel. Its receiver chart
has padding and spans three physical VT pages. Dents, small pores, scratches
and edge chips survive composition; the base supplies gray mortar wherever
the finite source has no coverage. The images expose a remaining art issue:
some large dents still have conspicuously smooth outlines. A lit wall with
peeling paint, weathering and terrain contact is still needed for visual review.

### A compression defect found by the real brick

The old mode-6 BC7 encoder connected componentwise minimum and maximum colors.
That assumes channels rise together. At a red-clay/gray-mortar boundary, red
rises while green/blue fall, so that line misses both real colors and produces
a fringe. The new candidate associates channel endpoints using signed
covariance against the widest channel. It replaces the original candidate
only if its exact decoded integer squared error is lower. Positive channel
correlations retain the original endpoints; block size and BC7 mode stay the
same. This affects VT albedo and ORM compression, including legacy sources.

Across eight clay variants, linear RGB RMSE improves from approximately
0.0076–0.0142 to 0.00214–0.00228. Maximum channel error falls from 0.0902 to
0.0336. The original output and figure are retained in
[binding-v4-clay-pages.zip](binding-v4-clay-pages.zip) and
[binding-v4-clay-channels.png](binding-v4-clay-channels.png).

## Source projection and composition

Raw geometry depth and normal-oriented material microheight have different
axes. `prepare_projected` solves the lateral displacement
`q + detail(q) * geometricNormalUV(q) = targetUV`, samples all material channels
at that same hit, and converts the depth to
`geometryDepth(q) + detail(q) * geometricNormalN(q)`. A tilted-plane analytic
case distinguishes this from simply adding the two height scalars. All 48 clay
faces converge with a maximum recorded UV residual below 1 micrometre.
The conversion has a 24-iteration cap, bounded memory, cancellation and stale
generation checks. Failure retains the previous complete immutable source.
Its stored normal is the sampled baked normal; this is not a general solver
for differential geometry or folded displacement surfaces.

`VtFiniteSources` contains immutable prepared sources plus rigid physical face
bindings. Each receiver vertex carries a categorical, one-based binding ID;
zero uses only the direct base. All corners of a triangle must agree, and its
vertices must lie on the declared plane. GPU plane/facing checks reject an
unrelated receiver. There is no UV wrapping or implicit source scaling.

The compositor samples the finite mip chain with its physical footprint and
combines color, squared roughness, AO/metal, normals and height with coverage.
It adds the height slope of a coverage transition to the normal without
differentiating interior relief twice. Final height bounds include source,
base and receiver-plane tolerance, and publish with the existing five output
channels. AUX and composed POM keep their current formats.

The first API is a per-face source assignment. General ordered overlapping
splats, spatial candidate queries, weathering layers and cross-object contacts
remain separate required work.

## Uploads, ownership and memory

- Source pixels are immutable and deduplicated within a catalog. Receivers
  sharing the same catalog share one GPU upload. Different catalogs currently
  own separate GPU payloads even when some source prototypes overlap.
- Geometry and surface snapshots own their selector arrays and retain source
  storage. Updating a source preserves old readers while publishing a new
  snapshot. The residency update API invalidates preparation; its caller must
  still invalidate affected owners within the existing edit transaction.
- Uploads use the existing per-frame byte/allocation limits and 64 KiB copy
  slices. No extra GPU submission or wait is introduced by source preparation.
  Descriptors become usable only after all source buffers are complete.
- A 349,504-byte test source takes 687 copying frames under an artificial
  512-byte/one-allocation quota. Repeated requests in the same frame cannot
  reset that allowance. Superseding a partial upload releases unpublished
  resources and leaves the visible page intact; the completed newest source
  matches an independently prepared reference in all output channels.
- Source interchange is still 64-byte float texels, not the final compressed
  asset representation. The existing VT pool remains nine bytes per texel
  plus its existing slot metadata. Prepared-source compression/cache and
  sharing across different wall catalogs remain open.

## Native validation and retained failures

Canonical MSVC RelWithDebInfo, native Vulkan on the RTX 4090. All accepted GPU
runs require zero Vulkan validation errors. Source hashes are recorded before
and after each command; the accepted runs contain no intervening source edits.

- [projection-v1-checks.json](projection-v1-checks.json): tilted-plane CPU
  projection and all 48 projected clay faces pass.
- [binding-v3-checks.json](binding-v3-checks.json): standalone CPU residency
  build/run pass; a subsequent test compilation typo failed and was corrected.
- [binding-v4-checks.json](binding-v4-checks.json): all 48 material/projection
  cases pass and write the eight complete front-face source fixtures. Analytic
  VT and bounded upload cases pass. Real clay then exposes the color defect.
- [binding-v5-checks.json](binding-v5-checks.json): all eight real-clay cases,
  analytic compositor regressions, bounded source upload, native renderer VT
  smoke and editor build pass after the compression correction.
- [binding-final-checks.json](binding-final-checks.json): `vt-queue` passes,
  including source edit/no-op/rejection/removal, owned selectors, memory
  accounting and retained old source snapshots. `vt-direct-source` passes.
  Its last command used the unrecognized name `vt-composed-pom` and ran the
  default smoke path; that result is not evidence for composed parallax.
- The correctly named composed/legacy parallax runs both pass, with zero
  Vulkan validation errors, in [binding-parallax-checks.json](binding-parallax-checks.json).

The original `binding-v1` test mistakenly addressed slot 120 in a 16-slot test
pool, causing out-of-bounds copies and a device-loss cascade. The corrected
fixture uses slots 12–15 and bounds-checks readback. `binding-v2` then passed
GPU tests but exposed a missing public include path in the standalone CPU
test target. These failed attempts remain in this directory.

The real-clay height comparison initially allowed R16 quantization plus
0.5 micrometres. Two variants reached about 0.73 micrometres near sharp chips
after float chart/barycentric reconstruction at 1000 texels/metre. Its final
integration bound is R16 quantization plus 1 micrometre (1/1000 of a source
texel). The reference also handles roundoff above unit source coverage exactly
as the consumer does. No production height algorithm or existing analytic
sampler/POM threshold was changed for this adjustment. Final maximum measured
height error is 0.7293 micrometres; average source-to-decoded-normal cosine is
at least 0.99907 over the covered samples used by this diagnostic.

Single-run synthetic GPU timestamps are 0.0124 ms/page for the base fixture
and 0.0253 ms/page for its 62-op tape, versus 0.0114 and 0.0245 before the
encoder change. These small isolated samples do not establish percentiles,
real-wall generation latency or StreamMountain performance. Full native
performance acceptance remains deferred under the original requirements.

## Reproducible channel evidence

[summary.json](summary.json) records the comparison measurements, 48 projection
results, hashes of all raw fixtures/readbacks, and final revalidation of 124
source files with no changes. [clay-vt-readback.zip](clay-vt-readback.zip)
contains eight `.fst` source fixtures and eight `.vt.bin` outputs (9,780,529
compressed bytes). The fixture header/reader is in
`MatterEngine3/tests/vt_finite_source_fixture.h`; it is a same-build test
interchange, not an asset-cache format. Each decoded output is a 96×256 array
of ten little-endian float32 values: RGB, ORM, normal XYZ, height metres.

Extract the archive and run `python3 plot_vt_channels.py <extracted-directory>`
to reproduce the plots. The optional compressor comparison reads the retained
V4 archive next to the script. To regenerate native data, build the
`solid_face_projection_gpu_tests` and `vt_compositor_tests` targets using the
canonical wrapper. Run the former from the repo root with
`MATTER_CLAY_FACE_DUMP` set to an existing native Windows directory, then the
latter from `MatterEditor` with `MATTER_VT_CLAY_FIXTURE` set to that directory.
Use `MATTER_VK_VALIDATION=1` for both. Exact executed commands and binaries are
in the linked check/source manifests.

## Next implementation step

Connect the shared JS whole-brick layout to low-poly receiver faces and the
provider's prepared-source lifecycle. Generate matching source dimensions:
the source inspection default is 112 mm deep while the modular header bond
uses 117.5 mm. This must be an explicit matching bake, not texture stretching.
Then review the lit multi-size wall, add weathering/splats and continue the
terrain proof. The composed-POM internal-chart seam gate remains open.
