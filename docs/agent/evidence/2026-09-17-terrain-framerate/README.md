# Terrain rendering below 5 ms

Active goal: reproduce the last three engine issues and bring terrain rendering
below 5 ms per frame. Preserve native 2796 x 1044 resolution and the authored
material/POM detail. Report whole GPU frame cost as well as G-buffer cost;
a G-buffer-only result does not establish the complete target.

## User issue evidence

Newest first, ordered by the report timestamp:

| Issue | RT | Filing GPU frame | G-buffer | Additional finding |
| --- | --- | ---: | ---: | --- |
| `14c876bc-00fd-6b9c-e50f-5fd8991afbe9` | Off | 164.205 ms | 156.613 ms | Cliff view; VT admission rejections despite variant/mesh headroom |
| `f9ee4cb7-dafa-191e-91a0-29c1e222b823` | Off | 29.224 ms | 26.829 ms | Valley view; steady VT CPU update approximately zero |
| `f6413de6-fa6a-964e-d5da-e9d47904abd8` | On | 157.655 ms | 26.727 ms | GI alone costs 124.069 ms |

The reports hide MountainRock, MountainEvergreen and the four Conifer modules.
Volumetrics are off; cloud-shadow resources remain enabled. POM distance is
1309 m, relief cap 0.316 m, maximum march 1.193 m. The actual internal and
output resolution is 2796 x 1044, Native. Read the reports' root `props` and
shot camera/history, not merely the later `at_file_time` state.

The issue executable predates the automatic terrain/weld surface connections.
Therefore the original raster bottleneck is not explained by that later change.
The current development executable also has a separate unclosed POM-enabled
device-loss failure from `../2026-09-17-sector-pom/stream-v1`. Its POM-disabled
control (`stream-v2`) completed all images with no device loss or Vulkan
validation errors, but failed readiness/connection-warning acceptance. That
control narrows diagnosis; one successful run does not prove the crash cause.

## Measurement harness

`capture_issues.py baseline-v1` uses the exact two raster-report cameras and
saved rendering/hide settings, disables validation for timing, and captures
100 samples each with POM disabled/enabled at native resolution. It preserves
and restores scene props, keeps the existing world cache, records executable
and shader hashes, and checks captures/errors. POM-off is only a diagnostic.

**The first run is exploratory, not a settled performance baseline.** Its
`wait_idle` condition can release when resident-sector count pauses while VT
registration and page work continue. The valley sample blocks were taken with
growing variant counts and nonempty page queues. Also, its hidden window and
default FIFO presentation can throttle cadence and GPU clocks. Do not compare
its frame-time samples to the 5 ms target or interpret the on/off delta as an
isolated shader cost. The next run needs explicit VT/variant stability and
uncapped presentation with a visible window, with effective mode recorded.

## Next investigation

1. Establish settled and streaming measurements separately at the issue views.
2. Attribute ordinary chart marching, connected-boundary searches, fallback
   shading, and GPU shader occupancy/work before retaining optimizations.
3. Diagnose which resource actually rejects VT admission; the warning banner's
   variant and mesh counters do not identify the exhausted resource.
4. Resolve the new connected-surface publication warnings/device-loss failure
   without conflating them with the older user performance reports.
5. Validate retained changes with native regression checks and matched visual
   captures, then repeat raster and RT/GI measurements separately.

Two earlier per-sample lookup caches were rejected after real-scene timings
showed no useful speedup. See `../2026-09-17-pom-footprint-reuse` and
`../2026-09-17-pom-page-reuse`; do not repeat those experiments as new work.
No performance fix or sub-5-ms acceptance is claimed here.

## Settled native baseline (`baseline-v2`)

The RTX 4090 run confirms native 2796 x 1044, a visible window, validation off,
and requested/effective IMMEDIATE presentation. Each phase waits until active
instance count, VT variant count and cumulative fills stay unchanged for
12 seconds with queue zero, then collects 100 samples. `census-audit.json`
independently checks every sampled VT row after capture.

| View / control | G-buffer median | GPU frame median | Presented frame median |
| --- | ---: | ---: | ---: |
| Valley, POM off (diagnostic) | 3.185 ms | 5.557 ms | 7.305 ms |
| Valley, POM on | 26.080 ms | 28.324 ms | 28.375 ms |
| Cliff, POM off (diagnostic) | 7.015 ms | 11.263 ms | 14.780 ms |
| Cliff, POM on | 133.728 ms | 138.101 ms | 138.495 ms |

All sampled phases have constant variant/fill counts, queue zero and no
evictions. Valley has 791 variants and no rejected admissions. Cliff has 807
variants and continually retried rejected admissions; the first warning names
the indirection-table arena. Its live usage is below capacity, consistent with
the allocator's inability to reuse free blocks of other sizes. Retired-block
accounting still needs direct instrumentation to quantify the wasted capacity.
This is a separate allocation problem, not exhaustion of the 25,600 physical pages.
The table can also reach real capacity; allocator fragmentation is not the
only future pressure to handle.

The run completed normally with four correctly sized captures, no reported
device loss/validation/command errors, unchanged executable/shader hashes and
restored props. It produced 1,053 connection-publication warnings. These
warnings and rejected admissions remain failures to fix, even though they do
not invalidate the observed high cost. No acceptance result is claimed.

## Connected-path isolation

`connected-path-diagnostic.patch` adds an opt-in G-buffer specialization:
`MATTER_GBUFFER_POM_PATH=chart_only` keeps ordinary chart marching but omits
the connected-boundary fallback. `reference` (and an unset variable) retains
the complete path. Invalid values fail initialization. This is a diagnostic
to distinguish connected search/execution and compiled shader cost; it is
explicitly not a quality-preserving optimization. Native build evidence uses
the prefix `terrain-path-v2` in the shared build/check directory. The first
attempt compiled the smoke target successfully, but WSL interop failed before
launching the editor build. The second attempt completed both targets with
unchanged sources. Exactly three source files differ from the baseline:
`gbuffer.frag`, `vt_parallax.glsl`, and `vk_scene_renderer.cpp`.

Native `vt-composed-parallax` and `vt-pom-work` pass, including required RT
checks, with zero Vulkan validation errors. The normal path retains the prior
analytic depth and chart/connected diagnostic outputs. Editor SHA256:
`693f292e1591dc03df911e95f6816789e892b512171e3c3c49345bac5dafe08d`.

`chart-only-v1` completes all four phases and images, with no device loss or
command errors, unchanged executable/shaders and restored props. Native
resolution and IMMEDIATE presentation are confirmed. Its settled valley has
the same 791 variants and 1,603 fills as the baseline. Cliff has the same 807
variants; it stops at 3,969 fills because connected requests are deliberately
absent (the reference POM phase had 3,990).

| POM-enabled view | Full-path baseline G-buffer | Chart-only diagnostic G-buffer | Diagnostic whole GPU frame |
| --- | ---: | ---: | ---: |
| Valley | 26.080 ms | 3.916 ms | 6.553 ms |
| Cliff | 133.728 ms | 8.301 ms | 13.033 ms |

This identifies connected POM execution/compiled cost as the major target.
It does not prove how much is triangle seeding versus the subsequent walk,
and it is not an accepted optimization: boundary relief is omitted. Even this
diagnostic does not meet the whole-frame goal. Flat controls vary with GPU
clocks/CPU pressure, so small timing differences must not be overinterpreted.
The diagnostic run still has 1,042 connection-publication warnings and cliff
admission retries. A same-build full-path repeat is the next control.

## Next implementation candidate

The current `vt_walk_seed` scans up to 2,048 triangles for each boundary
invocation. Replace that search with a small deterministic bounding hierarchy
owned by the existing immutable geometry lease. Keep original triangle order
and first-hit rules, all geometry/footprint/snapshot checks, original POM
sampling/refinement, and the real connected walk. Do not remove connected POM
to obtain the diagnostic timing as a supposed fix.

One compatible representation is a compact node suffix in the existing GPU
triangle allocation. Chart records' unused range words name each hierarchy;
the geometry metadata's reserved count records the suffix bounds. This avoids
new descriptors or lifetime rules. CPU preparation builds bounds on its worker;
the existing bounded upload path must account for every added byte. Tiny charts
retain the linear route. Ordered traversal and conservative bounds need an
independent linear first-hit oracle, native accelerated/linear shader probes,
and existing raster/RT/boundary tests. Retain only a measured scene improvement
with preserved material/normal/depth and boundary coverage.

If seeding is not the dominant remaining cost, measure repeated neighbor
projections and connected filter reconstruction next. The below-5-ms objective
also requires fixing cliff admission/retry pressure and measuring the full GPU
frame and CPU render path; accelerating one search alone does not close it.


## Same-build full-path control (`reference-v4`)

The visible benchmark window is disabled for live mouse/keyboard input, using
only the exact editor process owned by the capture driver. This avoids the
camera drift that invalidated `reference-v3`. The normal path reproduces the
large POM cost with the diagnostic-capable executable (`terrain-path-v2`):

| View / control | G-buffer median | GPU frame median | Presented frame median |
| --- | ---: | ---: | ---: |
| Valley, POM off | 3.878 ms | 6.706 ms | 10.565 ms |
| Valley, POM on | 26.984 ms | 30.036 ms | 30.840 ms |
| Cliff, POM off | 6.891 ms | 10.986 ms | 15.050 ms |
| Cliff, POM on | 132.305 ms | 136.928 ms | 137.305 ms |

All four phases have 100 samples, queue zero, constant variants/fills and no
evictions. Valley retains 791 variants / 1,603 fills. Cliff has 798 variants
and 3,959 / 3,980 fills, versus 807 and 3,969 / 3,990 in the earlier runs:
admission under table-arena pressure is not completely deterministic. Thus the
cliff comparison confirms the broad bottleneck, not a finely controlled small
performance difference. There are 1,068 connection warnings and continued
cliff admission retries. Four images are present at native resolution; hashes
are unchanged; the process exits normally with no reported rendering errors.
The under-5-ms target remains unmet.


## Ordered seed hierarchy: implementation and focused evidence

`vt_seed_bvh.h` builds conservative, ordered bounds on the CPU preparation
worker. `vt_seed_walk.glsl` traverses their immutable suffix in the existing
triangle buffer; the eight-triangle leaves retain original first-hit order.
Charts below 64 triangles keep the original linear route. The original 2,048
triangle search limit, barycentric/plane tolerances, subsequent geometry walk,
filter reconstruction, POM steps/refinement, page requests and fallbacks remain.
The new bytes participate in worker admission, bounded upload, geometry leases
and GPU memory census. No new descriptor or per-frame CPU rebuild is needed.

The standalone CPU oracle reports 8,194 queries with zero mismatches against
independent double-precision triangle projection, including edges/vertices and
accepted tolerance extensions. Candidate tests drop from 8,201,374 to 35,486.
This portable check supplements native tests; it is not native acceptance.

Native build `terrain-seed-v1` completes editor and smoke targets with unchanged
source hashes. The editor SHA256 is
`01d146bf78c2d279a9270659fb38482f7f1dc298f6ca776f92a3efd6c0ebca77`.
The actual GPU probe compares accelerated and original searches on the same
uploaded 2,048-triangle geometry: 12 queries choose identical triangles and
projected points, with 12,084 versus 44 candidate tests and zero validation
errors. `vt-composed-parallax` and `vt-composed-seam` also pass, including the
required RT checks and curved/90-degree/diagonal boundary coverage. These are
correctness/work-count results; real-scene speedup is still to be measured.


The remaining `vt-sector-seam` and `vt-surface-connections` native checks pass
as well. `terrain-seed-cpu-v1` builds and runs `vt_compositor_tests`, which
executes the 8,194-query CPU oracle, bounded worker/preparation tests and GPU
compositor checks; all pass with zero Vulkan validation errors. Build/test
manifests and raw logs live in `../2026-09-16-shared-vt-pixels/` under these
prefixes. `capture_issues.py ... --channels` additionally captures raw albedo,
normal and depth after each POM phase, outside the timing samples. It restores
the ordinary view before the next timed phase.


## First real-scene result (`seed-v1`)

The full connected path remains enabled. Native 2796 x 1044, visible locked
window, IMMEDIATE presentation, issue properties and original camera positions
are retained. The run exits normally after 466 seconds; all ten lit/color/
normal/depth images are present, binary/shader hashes stay unchanged, and no
render/command errors are reported. Scene props are restored. Six focused
native checks pass, but the whole goal remains open.

| View / control | G-buffer median | Whole GPU median | Presented median | CPU resolve / build / draw medians |
| --- | ---: | ---: | ---: | ---: |
| Valley, POM off | 3.572 ms | 6.262 ms | 7.375 ms | 2.015 / 0.410 / 3.515 ms |
| Valley, POM on | 16.008 ms | 18.815 ms | 19.510 ms | 2.460 / 0.460 / 3.630 ms |
| Cliff, POM off | 7.130 ms | 11.492 ms | 12.770 ms | 2.960 / 0.555 / 7.565 ms |
| Cliff, POM on | 105.660 ms | 110.008 ms | 110.460 ms | 3.060 / 0.535 / 7.070 ms |

Relative to `reference-v4`, the POM-enabled whole GPU medians fall from 30.036
to 18.815 ms (valley) and 136.928 to 110.008 ms (cliff). Drawn geometry is
identical: 86 batches / 252,163 triangles in the valley and 108 / 449,699 at
the cliff. Valley also has identical 791 variants / 1,603 fills. Cliff has
806 variants / 3,983 fills versus 798 / 3,980 in the reference; admission under
arena pressure differs, so its timing is not a perfectly controlled A/B.
All 400 samples have queue zero, constant variant/fill counts and no evictions.
Cliff admission retries and 1,083 connection warnings remain unresolved.

Visual inspection retains the same broad geometry and material detail. Cloud
shadows move between runs, so the lit captures do not establish pixel equality.
The new raw channel captures provide a baseline for the next change; matching
old raw-channel captures are not available. The native accelerated/linear
oracle establishes exact query agreement on its fixture, not on every scene
pixel. Do not overstate visual or performance acceptance from these results.

The hierarchy is a useful partial optimization. It does not remove most of
the remaining cliff cost. Next: profile residual seed attempts (including small
charts), repeated neighbor projections/filter reconstruction and shader
occupancy before selecting the next change. Preserve complete connected POM;
do not substitute the chart-only diagnostic for the user-visible path. The
flat controls still exceed the whole-frame target, so admission/connection
retry work and other GPU/CPU rendering costs also remain.

For the user's VT-benefit question: all 400 settled POM samples across
`baseline-v2` and `reference-v4` have zero shared-material references and zero
coverage-only pages. This terrain currently benefits from cached composition,
not the shared material-page reuse exercised by the wall implementation. There
is not yet a matched simpler-texture baseline proving a net time or memory
benefit for VT itself. POM search savings do not establish that comparison.

## Residual work census (`terrain-work-v1`)

`MATTER_GBUFFER_POM_PATH=work` enables G-buffer specialization constant 1.
Ordinary launches leave it false, removing counters and diagnostic color writes.
The complete connected path still runs. Raw albedo encodes each count as
`round(log2(count + 1) / 24 * 255)`:

- Horizon diagnostic 0: RGB = seed triangle predicates, seed hierarchy nodes,
  all calls to the geometry projection predicate (including seeding).
- Horizon diagnostic 9: RGB = locate-loop iterations, connected height/material
  samples, individual reconstructed filter-tap attempts.

Only surviving visible fragments appear in these images; they do not count
occluded shader invocations, wave divergence or actual hardware instructions.
The counters can also inhibit compiler dead-code removal. Treat this as a
semantic-work census, not a GPU-cost attribution or timing acceptance.
`analyze_work.py CAPTURE_DIRECTORY` decodes integer lower/upper bounds for each
logarithmic bin, checks that projection counts can contain the seed predicates,
and reports population, totals and percentiles. It rejects saturated counts.

The capture helper accepts `work`, collects 20 samples per phase instead of
100 and always captures channels plus a separate walk-work image. Its timings
are instrumented and must not be compared to the 5 ms goal. Native build
`terrain-work-v1` passes `vt-pom-work` and `vt-composed-parallax`, including
required RT checks and zero Vulkan validation errors. These default-path logs
are identical to the corresponding `terrain-seed-v1` logs.

A candidate if residual reconstruction dominates is to reduce state carried
through the walk. `VtWalkContext` copies an 80-byte immutable metadata record
through current, neighbor and refinement contexts. Keep a reference instead:
root metadata can be read by physical-page index; crossed-owner metadata can
be addressed inside the existing retained connection-table record (header 96
bytes, link stride 176, metadata offset 32). This must preserve the complete
snapshot/material/revision comparisons, original immutable ownership and all
retirement rules. Do not replace them with mutable current-owner metadata.
Also inspect the position and normal fields carried in `VtWalkPoint`: position
is only needed locally to compute UV, and interpolated normal is used at final
material sampling, not height steps. Any resulting simplification requires
native rotated-sector/seam/depth tests and an uninstrumented issue-camera run;
register pressure remains a hypothesis until that comparison is measured.


`work-v1` completes all 12 captures at native resolution, with unchanged
executable/shaders, no reported rendering errors and restored props. It still
reports 1,056 connection warnings and cliff admission retries. The work images
show a byte value (41) that does not fit an ideal integer log bin; the decoder
therefore reports conservative intervals including one extra byte step for
the render/display/readback path. These are bounded estimates, not exact
hardware instruction counts. This uncertainty does not change the distinction
between seeding and subsequent traversal:

| View | Pixels with connected work | Seed triangle predicates | All projection predicates | Locate iterations | Height/material samples | Filter taps |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Valley | 21,968 (0.753%) | 0.186–0.209 M | 3.39–4.10 M | 0.681–0.805 M | 0.394–0.456 M | 0.286–0.342 M |
| Cliff | 856,425 (29.34%) | 8.50–9.63 M | 106.92–129.21 M | 23.15–27.32 M | 19.24–22.57 M | 3.91–4.72 M |

Nearly all seeded pixels begin height sampling; only 3,962 cliff pixels have
seed predicates without a locate call. The remaining problem is repeated
geometry reconstruction and associated shader execution, not predominantly
failed linear seeding. The metadata-reference/late-normal simplification above
is the next bounded experiment. If that is insufficient, precomputed static
edge planes can remove repeated neighbor-normal reconstruction; external links
must retain their dynamic transforms and complete validation.

## Compact walk state (`terrain-compact-v1`)

The implementation keeps a three-word metadata reference instead of twenty
metadata words in each walk context. Root references select the existing
physical-page metadata record; crossed-owner references address the metadata
inside the already-retained immutable link record. The host now asserts its
32-byte record offset alongside the existing 96-byte header/176-byte stride.
No CPU/GPU allocation or retirement scheme changes. Root publication uses
`record_input_snapshot_indices`' captured `vkCmdUpdateBuffer` data and full
read/write barriers, so the metadata record stays stable during the draw.

`VtWalkPoint` now carries eight words instead of fourteen. Projected position
is local to UV reconstruction, and final material sampling reconstructs the
interpolated normal from the retained triangle/barycentrics. Height samples no
longer calculate/store unused shading normals. Seed rejection also restores
the original negative-predicate form, preserving its exceptional-value behavior.
`compact-walk.patch` and `compact-before/` record this isolated experiment.

Native build succeeds with unchanged source hashes. Editor SHA256:
`57f0ae14c374c094615a2857b151be8064db0493ae2c733cfe8d197207ef4fa6`.
The native `vt-pom-work` log is identical to the prior passing log, including
12 accelerated/linear query pairs and zero validation errors. Broader checks
and uninstrumented scene timing must finish before retaining a performance claim.


All five compact-state native checks now pass: `vt-pom-work`,
`vt-composed-parallax`, `vt-composed-seam`, `vt-sector-seam` and
`vt-surface-connections`. Every raw result-log SHA256 matches the corresponding
`terrain-seed-v1` result, including depth error/crossing metrics and validation
counts. The uninstrumented `compact-v1` scene run is in progress; no performance
claim is inferred from those correctness checks.


`compact-v1` finishes normally with ten native-size captures and 100 samples
per phase, no reported rendering errors and unchanged executable/shaders.
Valley still has 791 variants / 1,603 fills; cliff has 792 / 3,942 (different
admission outcomes from `seed-v1`). The full GPU medians are 17.841 ms valley
and 109.346 ms cliff, versus 18.815 and 110.008 for `seed-v1`. G-buffer medians
are 15.128 and 105.137 ms. This is a modest valley difference and no meaningful
cliff improvement; it does not support metadata copying as the dominant cost.
The flat controls are 6.333 / 11.331 ms whole GPU. Presented POM medians are
18.320 / 109.635 ms; CPU resolve/build/draw medians are 2.615/0.410/4.930 ms
and 3.605/0.680/8.945 ms. The target is still unmet.

Raw-channel comparison with `seed-v1`: valley depth is byte-identical; albedo
and normal differ at only 191 and 142 of 2,919,024 pixels, by at most one byte.
Cliff depth differs at two pixels by one byte, normals at 13,128 pixels by at
most three bytes. Cliff albedo differs at 17,889 pixels, with larger changes
consistent with its different admitted-page population; this comparison is
not a matched-material proof for those pixels. Full counts are in
`compact-v1/raw-channel-comparison.json`. There are still 1,069 connection
warnings and continuing admission retries.

The clock observation during cliff settling shows 2,745 MHz graphics /
10,501 MHz memory. The later 210 MHz observation was taken after rendering
ended (paired with the last historical STATS row), so it must not be used as
the clock of the POM timing samples. Better synchronized clock logging would
be needed for clock-normalized comparisons.

Next implementation: immutable cached edge planes for triangles whose three
neighbors belong to the same retained geometry and have well-conditioned
normals. A conservative interior test may skip repeated neighbor projections;
anything close to or outside an edge uses the original full walk. This avoids
changing crossing decisions and keeps dynamic external links on their current
path. Plane bytes, temporary CPU storage and bounded upload must all be charged.
Validate against an independent geometry oracle, actual GPU fast/reference
queries, existing seam/RT tests and uninstrumented issue-view timings.
