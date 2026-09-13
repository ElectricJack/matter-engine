# RT lighting implementation and measurements — 2026-09-12

This implements the first measurement and shader-cost milestone from the
[RT/GI review](rt-gi-lighting-performance-review-2026-09-11.md). The castle
worktree is `D:/tmp/matter-castle-assembly`, branch `castle/astra-assembly`.
Existing castle geometry, authored materials and unrelated work are retained.
Runtime diffuse lighting caches and a new deferred-lighting architecture are
subsequent work, not features delivered by this milestone.

## Changes

1. **Measure local direct lighting independently.** GPU query zone 20 brackets
   its full-rate dispatch. Existing zone numbers remain stable. Frame stats,
   editor diagnostics, issue captures and perf JSON expose the pass. A separate
   raw-query collector reports sample count, median and p95; legacy GPU fields
   remain moving averages and must not be called medians. Missing queries do
   not masquerade as zero-duration executions.
2. **Remove normal-run any-hit diagnostic contention.** The visibility shader's
   global diagnostic atomics are disabled by specialization in production.
   Transparent visibility, absorption and the per-ray layer limit remain.
   Fault-injection tests retain counters by default.
3. **Skip exactly zero transport work.** Zero-contribution diffuse and disabled
   reflection lanes avoid their rays and write defined zero output. Independent
   reflection and transmission RNG streams prevent lane skipping from changing
   the other lane's samples. The new streams change old stochastic realizations;
   they do not intentionally change sampling distributions.
4. **Cache effective light publication.** A revision/configuration-keyed cache
   reuses immutable authored light data, scales ranges once and builds the
   desired spatial index once. Unchanged frames no longer copy and re-index the
   full publication. Revision-zero startup data is validated; failure preserves
   the accepted cache. Existing castle range and cell-size policies remain.
5. **Remove unused ranking code from unlimited pipelines.** When both light
   budgets are zero, specialization eliminates the old runtime top-K branch and
   its dynamically indexed private arrays. Requested budgets retain their code.
   `MATTER_RT_KEEP_LOCAL_SELECTION=1` provides an equivalent-output comparison.
   Reduced register pressure is a hypothesis; register occupancy has not been
   measured with a GPU profiler.

Primary area-light
visibility can use one through four samples (default four). Secondary POM can
be footprint-adaptive or disabled while keeping material/normal maps (default
reference). Secondary local-light visibility can use a one-item importance
reservoir (now default for unlimited secondary lights after the measured and
visual comparisons). Explicitly configured nonzero secondary budgets retain
the previous algorithm; mode 0 remains the exact/budgeted comparison. These
are pipeline-creation settings, not live FIFO
properties; see the [control surface](agent/control-surface.md#vulkan).

The reservoir evaluates every candidate's RGB BRDF, sums unshadowed lights
exactly, selects a shadowed light with probability proportional to positive
luminance, and divides its visible RGB contribution by that probability. It
does not discard the remaining lights' expected energy. Numerical failure
falls back to exhaustive evaluation. Primary direct and transmission retain
their existing algorithms. One contributing shadow light avoids a replacement
random draw and RGB normalization round trip.

**The reservoir is unbiased before existing nonlinear processing.** The diffuse
and reflection firefly caps at luminance 8 and temporal neighborhood clipping can lose
energy when occasional high-weight samples are clipped. A raw estimator test
does not prove final denoised diffuse energy parity or motion quality. Keep the
exact mode available and do not treat a single static screenshot as acceptance.

## Measurement method

Runs use a real, verified visible Windows editor window on RTX 4090, driver
610.74, at 1280×720 and Native resolution. No headless graphics tests are used.
The same CastleUpgraded hall camera and lighting settings are applied through
the command file. Impostors are disabled, local-light budgets are unlimited,
and primary POM is enabled. The editor waits for static uploads to remain
stable for 30 frames, warms for 15 seconds, then samples for 20 seconds.

`tools/castle_rt_perf.py` records executable SHA-256, environment, commands,
window visibility, logs, screenshot and perf JSON. Visibility now requires the
actual `GLFW30` renderer window, excluding CUDA/D3D and GLFW helper windows.
Artifacts are under
`C:/tmp/castle-rt-milestone1/<case>/`. `metadata.json` disambiguates binaries
across incremental builds. Screenshots are captured during warmup; they are
useful for geometry/material checks but are not converged image-quality metrics.

The earliest `baseline-hall` attempt lacked verified window visibility and is
excluded. Earlier `baseline-visible-hall` runs used the frozen old executable
with a less strict visibility helper that could also show driver utility
windows. Their numbers are retained as development measurements. The final
`baseline-glfw-verified` / `optimized-glfw-verified` pair uses the stricter
renderer-window check. The old executable predates raw GPU per-pass statistics,
so comparisons use measured frame times, not EMA/raw mixtures.

| Hall case | Median frame, ms | Median FPS | Raw local direct median, ms | Raw GI median, ms |
|---|---:|---:|---:|---:|
| Visible baseline, GI on | 223.604 | 4.472 | unavailable | unavailable |
| Visible baseline, GI off, RT direct retained | 40.872 | 24.467 | unavailable | unavailable |
| Initial fixes, four primary samples, reference POM | 144.767 | 6.908 | 4.049 | 135.802 |
| Initial fixes, one primary sample, reference POM | 148.686 | 6.726 | 1.483 | 141.224 |
| Initial fixes, one primary sample, adaptive POM | 150.127 | 6.661 | 1.481 | 142.587 |
| Unlimited selection code specialized out, one sample | 122.551 | 8.160 | 1.361 | 115.114 |
| Secondary importance reservoir, one primary sample | 36.644 | 27.290 | 1.194 | 31.219 |
| Secondary importance reservoir, four primary samples | 38.687 | 25.849 | 4.078 | 30.279 |
| Final baseline, renderer-window visibility verified | 211.203 | 4.735 | unavailable | unavailable |
| Final optimized, renderer-window visibility verified, four samples | 42.541 | 23.507 | 3.437 | 34.194 |

One primary sample clearly reduces its own pass by about 2.6ms, but the initial
whole-frame pair did not improve because GI timing varied. Adaptive secondary
POM did not improve this hall view; reference POM remains the default. These
are separate observations, not evidence that reducing primary samples makes
GI slower. No texture detail or geometry was removed for these comparisons.
The authoritative final pair retains the original four-sample direct-shadow
budget and reduces median frame time from 211.203ms to 42.541ms: **79.9% lower
frame time, 4.96× FPS (4.735 to 23.507)**. Frame p95 falls from 234.111ms to
46.041ms. Earlier development runs reached 25.8–27.3 FPS, but use the final
stricter pair for the headline. Timing varies between runs; these are fixed
hall measurements, not a promise for every camera or a confidence interval.

Both final runs had zero static-upload deltas during sampling and zero recorded
validation errors. Full Vulkan validation is separately covered by the native
suite. The final editor is left open on the hall using production defaults;
`live-castle/` contains its launch receipt, effective-setting log and screenshot.

## Validation notes

Native CPU tests cover light cache reuse/invalidation, legacy index parity,
transactional failure, and raw GPU sample aggregation/availability/percentiles.
The graphics tests run in visible GLFW windows with Vulkan validation.

The local-direct test initially exposed an old test-only readback offset bug:
the ORM buffer became RGBA16F, but GI-history diagnostic reads still used the
earlier offsets. The reflection rejection/aux offsets now match the recorded
buffer layout. Production history logic was unchanged.

Independent transmission RNG exposed another invalid test premise: rough rays
could escape the finite supposedly uniform receiver wall into directional sky.
All four outlier paths reported the 100m miss distance. The energy fixture now
encloses the actual slab in a constant-emission inward-facing receiver, checks
every path reaches a finite surface, and keeps the original sample count and
2% energy tolerance. Production refraction was not retuned to satisfy the test.

The broader RT suite also found a real whole-frame reset-notification bug.
An earlier GI candidate reset could swallow an explicit RT/GI/light edit.
Explicit pending notifications now return once without that unrelated filter;
bridge-owned mode transition notifications are coalesced when queued, preserving
the existing no-double-reset behavior. Three other suite failures were test
source-file lookup assumptions about working directory. Lookup now resolves
the checkout; the original production contract predicates remain unchanged.

The native multilight reservoir test passed. It compares 256 raw reflection
samples against a separate exhaustive renderer, including an occluded strongest
light, unequal RGB emitters and additive unshadowed light. Reference RGB means
were `(0.039514, 0.066854, 0.091474)`, sampled means
`(0.038120, 0.067590, 0.085970)`, and measured standard errors
`(0.001928, 0.002983, 0.005791)`. All channels agreed within measured uncertainty;
this finite test is not a claim of exactly equal measured energy.
The corrected transmission test also passed: all four roughness rungs measured
0.95951, with finite surface hits, unchanged 2% tolerance and zero Vulkan
validation errors.

Final native status:

| Check | Result |
|---|---|
| MSVC RelWithDebInfo editor and native test build | PASS |
| `local_light_index_tests.exe` | ALL PASS |
| `perf_gpu_stats_tests.exe` | PASS |
| `vulkan_smoke_tests.exe`, `rt-local-ris`, sampling 1 | ALL PASS |
| `vulkan_smoke_tests.exe`, `rt-transmission`, sampling 1 | ALL PASS, validation errors 0 |
| `vulkan_smoke_tests.exe`, full `rt`, production defaults | ALL PASS, validation errors 0 |

The full final RT run includes direct-light glass weighting, diffuse and
reflection filtering, visibility classification, material queries, BLAS
lifecycle, A-trous behavior, temporal reset transitions, secondary sun
visibility, mirror/rough reflection, baked AO and GI disable. Its log is
`/tmp/castle-rt-full-release.log`; dedicated estimator and glass logs are
`/tmp/castle-rt-ris-acceptance.log` and `/tmp/castle-rt-transmission-final.log`.
Graphics runs were sequential and visible. Python helper compilation and
`git diff --check` also passed.

## Visual comparison

`quality-exact/` and `quality-ris/` under the artifact directory contain matching
visible capture runs: hall after 128 frames, a camera pan after 1/8/32 frames,
and gold/glass after 128 frames. The sampler retains wall relief, window
transmission and gold reflections. Camera-pan captures show additional transient
noise but no obvious new persistent trail or missing-light region in these views.

Mean absolute RGB differences in normalized **display PNG values** were 0.00297
for settled hall, 0.00456/0.00379/0.00310 during the pan, and 0.00331 for settled
gold/glass. Overall means shifted slightly downward. These are single-image
comparisons with different stochastic realizations; direct lighting and tone
mapping dominate much of each image. They do not isolate diffuse GI energy or
prove quality for every castle room, moving light, or continuous fly-through.

## Next architectural experiment

Compile separate diffuse and reflection/transmission ray-generation variants,
preserving the existing combined variant for output and timing comparison.
They can share the current descriptors, hit groups, raw signal images and
denoisers. Every variant must write only its own signal, including miss and
background clears, and have an independent GPU timestamp. Additional dispatch
and shared closest-hit payload costs could erase an occupancy benefit; measure
before adopting. The unlimited-ranking specialization result motivates this
experiment without proving a register-pressure diagnosis.

Runtime irradiance caching remains promising but requires a separate design:
current histories are screen-space, not persistent world lighting records.
Begin with static opaque diffuse receivers, retain exact reflection/transmission
and ray fallback, and define surface identity across LOD changes, revision
invalidation, off-screen residency and visibility-aware interpolation around
castle walls and doors. The atmosphere's irradiance texture is sky lighting,
not an existing cache of local bounced light.
