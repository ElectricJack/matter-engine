# Terrain virtual geometry + VT: raster baseline, 2026-09-18

## Scope and implementation

User-approved baseline: disable RT, GI, POM, forest/scatter, volumetrics and
cloud shadows; retain terrain geometry and VT. StreamMountain's `terrainOnly`
parameter defaults true. Disabling it restores scatter and lazily registers its
forest materials. The launcher uses immediate presentation, avoiding FIFO's
30 Hz display wait in the original test environment.

`MATTER_GEOMETRY_TERRAIN=1` plus `MATTER_GEOMETRY_RASTER_ONLY=1` compiles each
nonempty sector's owned charted mesh into the existing virtual geometry
hierarchy. It preserves child ownership, sector boundary records and the
original sector VT domain. Geometry-page raster draws borrow the controller
sector's rung-0 VT slot; page UVs retain that parameterisation. The source rung
still provides initial coverage and VT bake input. This is a bridge, not yet a
compact source-free sector/resolution cache.

Raster-only page registration skips RT vertex/index buffers, BLAS lookup/build,
RT scratch and BLAS readiness. Source terrain registrations use that policy too.
The normal RT page path remains and passes the existing BLAS restore test.

Terrain compilation now uses a 2 cm error-bound convergence tolerance and a
maximum subdivision depth of 8 instead of the generic 1 mm / depth 24 defaults.
The error estimator still returns conservative upper bounds when depth-limited;
this can retain extra detail, not understate displacement error. Original outer
boundary positions remain exact. The cache policy key changed to v2.

## Validation

- Native geometry GPU fixture: all pass; 24 CPU/GPU cut comparisons, BLAS cache
  restore, raster-only coverage/material parity, zero RT input allocation for
  raster pages, zero Vulkan validation errors.
- JS terrain-only test verifies terrain at several voxel rungs, no scatter or
  asset-catalog calls, no unused forest texture registrations, and restoration
  of forest material definitions when terrain-only mode is disabled.
- Native editor builds through the canonical MSVC wrapper.
- Terrain screenshot: `terrain-raster-v2/terrain-populated.png` shows VT detail
  on streamed page geometry. It is a local ground view, not proof of complete
  mountain coverage or all mixed-resolution seams.

## Measurements and limits

1280x720, RTX 4090. Results are **partial-scene diagnostics**, not acceptance of
the whole mountain or the one-second loading target. The perf harness's static
upload gate can trigger during a pause in background sector preparation.

| Run | Sampled terrain sectors at end | Frame median / p95 | Notes |
| --- | ---: | --- | --- |
| v1 | 8 | 31.20 / 34.93 ms | FIFO; cloud shadows still active; 14 static upload changes |
| v2 | 21 | 0.715 / 1.204 ms | Immediate; cloud shadows off; 9 static upload changes |
| warm v3 | 33 | 0.794 / 1.365 ms | Reuses prepared v1-policy sectors; no static upload changes during sample |
| bounded compiler v4 | 133 | 12.004 / 32.579 ms | New v2 preparation policy; 152 static upload changes during sample |

v2 raw GPU total median/p95: 0.596/0.658 ms. First sampled paged terrain appeared
48.96 s after process launch. v3 first sample already had 16 sectors at 1.90 s,
and 21 at 2.11 s. These are sampled readiness bounds, not exact completion
timestamps. First preparation is still far slower; v3 instrumentation observed
an uncached sector preparation taking 37.1 seconds with the old compiler policy.

v2 published 372 pages: page request-to-publication averaged 52.2 ms, maximum
372.6 ms. This **excludes earlier sector generation/charting/hierarchy cooking**.
Five geometry-payload read operations read 16,432,904 bytes; one 1 GiB CPU bank
backing allocation. No read failures, watchdogs, reservation stalls, evictions
or Vulkan validation errors in that stationary diagnostic.

## Remaining acceptance work

- Finish persistent sector/resolution packages: avoid re-charting and preparing
  full source meshes during cached admission; load desired detail directly from
  an independent hierarchy directory and large binary ranges.
- Cook/validate the complete working region, then measure cold OS-cache and warm
  loads, travel/return, full coverage and VT readiness rather than a small subset.
- Add actual surface displacement with receiver preservation. This pass pages
  the existing terrain shape; POM remains off.
- Complete reusable GPU/BLAS/staging banks and pressure/eviction tests.
- Validate cross-sector/mixed-resolution seams and VT appearance through all
  refinement transitions. Restore terrain RT material routing before enabling
  RT for the experimental terrain bridge.

Neither complete-world sub-second loading nor full-scene <=10 ms is proven.


## Larger working set and POM-off CPU cost

v4 published 10,154 pages by its final sampled window, with no page failures,
watchdogs, reservation stalls or Vulkan validation errors. It reached 133 terrain
assets and 10,166 page entries during a ~53-second process run. Payload bank
occupancy was about 104 MiB; geometry GPU reservations about 62 MiB. Rendering
was still streaming during the sample, and complete mountain admission was not
achieved. Raw GPU total median/p95 was 4.636/7.153 ms.

The CPU trace identified VT's pre-pass as a major hitch source. Cross-sector
surface-walk tables were being rebuilt even though POM was disabled. The next
change gates that publication on effective POM enablement, preserving requested
connections for live re-enable. These tables are consumed by the POM surface
walk; ordinary VT material sampling remains active. A native Vulkan regression
checks zero connection compilation/upload while disabled, successful publication
on re-enable, and the existing source-edit/slot-reuse behavior.

## Admission and per-frame bookkeeping fixes

The next larger run (v6) reached 380 assets and the old 65,536 page-entry limit.
Its 35.223 / 40.130 ms median/p95 and 13.136 / 15.853 ms GPU total were **not a
complete-scene acceptance run**: the 128 MiB root bank and page ceiling stopped
admission. Earlier v5 measured 13.885 / 22.202 ms frame median/p95 while still
streaming; removing POM connection work eliminated the observed VT-prepass
hitches, but did not solve all frame cost.

Follow-up changes:

- RootCache preallocates a configurable bank and reports budget failure before
  decoding. The terrain launcher now reserves 1 GiB each for root payloads,
  worker payloads, and geometry GPU admission. These are separate budgets;
  the GPU admission limit is not yet a unified GPU memory bank.
- Terrain metadata ceilings are 262,144 page entries and 1,048,576 known nodes.
- Residency dispatch visits queued pages rather than all resident pages, with
  constant-time in-flight accounting. Native tests cover publication,
  cancellation, retries, deferral and stale completions.
- Raster scene descriptions are reused while sources, transforms, detail
  settings and residency are unchanged. GPU traversal still follows the live
  camera; new pages and memory pressure invalidate cached descriptions.
- Raster hierarchy draws reserve per-page draw capacities directly instead of
  creating one dummy scene instance per resident page. Native GPU tests exercise
  fine and coarse cuts with a controller-only instance list, matching selected
  triangles and preserving sampled material/depth. Existing RT tests still pass.

These changes do not yet provide direct desired-resolution sector loading,
source-free admission, compact indirect commands or complete mountain coverage.

## v7: larger admission exposes further scaling failures

The controller-only build passed native CPU/GPU tests, but its larger mountain
run did **not** pass performance acceptance. It continued preparing/admitting
terrain for 257 seconds without reaching the perf harness's static-stability
gate. The run was explicitly stopped with a screenshot and orderly FIFO quit.
No steady-state `perf.json` exists for this run.

The last eight seconds of the VT presentation trace contained 32 intervals:
228.712 ms median, 341.322 ms p95. Latest-retired GPU readback median was
62.017 ms (not a synchronized per-frame GPU sample). The workload is larger and
still streaming, so this is not a controlled regression comparison with v6.
It demonstrates that the frame target is still far away at this scale.

The profile tail shows expensive command-layout rebuilding (~16.2 ms mean),
VT slot layout (~16.8 ms mean), and command preparation (~7.0 ms mean).
Hundreds of thousands of tiny page registrations continue expanding/rebuilding
renderer-wide command and VT tables. Removing dummy scene instances alone does
not address this representation cost. Sector bundles with persistent draw and
VT allocations must replace it before simply raising capacities further.

The final VT record had 624 variants and 891 used pool pages, with zero fill
failures, invalidations, evictions, dropped requests or dropped pages. This is
not a full-scene VT-readiness or texture-quality claim. Screenshot captures
local ground texture and distant terrain, not all seams or travel coverage.
