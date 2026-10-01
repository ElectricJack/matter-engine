# StreamMountain VT page-fill pacing — 2026-09-29

The final retry meets the **VT GPU-zone p99 <16 ms** target in both POM-off
captures: **1.91 ms after 45 s warmup and 8.25 ms after 300 s**, with maxima
2.22 and 8.69 ms. Composition and AO use adaptive 32-texel slices and exact
all-triangle pruning; incomplete pages remain private. The smaller slices
increase per-page latency and leave the queue backlogged. Earlier candidates
and failed rechecks are retained below as measurement history.

Task `clear-ridge.6`, POM off. Unless identified as a 300 s warmup, captures used
`VARIANTS=pom_off RUNS=1 WARMUP=45 SAMPLE=20
tools/streammountain_attribution.sh C:/tmp/<out>` on the RTX 4090 with the
MSVC RelWithDebInfo editor. Raw logs, traces, and perf JSON remain under
`C:/tmp/attr_clear_ridge6_*`; the executable hash is in each output folder.
Runs are single samples during a changing stream. The
[task-1 baseline](streammountain-pom-off-baseline-2026-09-28.md) used a larger
visible geometry population; VT residency also changes with scheduling.
Its absolute GPU times are context, not an equal-scene speed comparison.

## First-attempt history

| POM-off, 45 s warmup | Task-1 baseline (3 runs) | Before (`df8b673b`) |
|---|---:|---:|
| Sampled frames | 47 / 72 / 53 | 312 |
| GPU total median, ms | 227.5–248.3 | 36.67 |
| GPU total p99 / max, ms | 394.1–594.3 | 365.35 / 442.23 |
| Frame intervals over 100 ms | 161/172 pooled | 47/312 |
| Frame intervals over 1 s | 6/172 pooled | 0/312 |
| VT median / p95 / p99, ms | p99 162.6–354.6 | 0.00 / 165.55 / 331.69 |

The first implementation ran a second normal pass that resolved five chart
positions per texel. It regressed: GPU total median/p99/max
76.40/468.21/545.70 ms over 156 frames, 71/156 frame intervals above 100 ms,
none above 1 s, and VT median/p95/p99 38.43/305.69/433.63 ms. That pass was
replaced with one that reads the chart ID and base normal already written by
the compositor. A later one-resolve intermediate capture was stopped without a
sample after several minutes of near-idle CPU/GPU and unchanged log output; it
does not establish a performance result.

## First-attempt implementation

- Remove the 96-register initialization from each tape evaluation. The CPU
  packer checks backward references and writes every physical register before
  any use.
- Evaluate the ordinary direct-source tape once per texel and derive its
  normal from the page's composed R16 height image. Finite/periodic special
  sources keep their receiver-aware point probes.
- Add a shared 12 ms GPU fill target for tail and detail pages. The first
  frame admits one fill; retired `vt_fill` GPU timestamps adjust the quota,
  bounded by the existing per-class page caps. At least one page remains
  admissible even when one page alone exceeds the target.
- Add `vt_fill` and `vt_enrich` GPU timing subzones so the aggregate `vt` cost
  can be attributed without treating AO or table work as page bake time.
- Reuse the parallax loop's material gradient instead of fetching it twice.

## First attempt: zero-resolve capture

The final candidate used the composed height image and no additional chart
resolve in the normal pass. The POM-off sample contained 176 frames and no
Vulkan validation errors:

| 45 s warmup, 20 s sample | Before | Final candidate |
|---|---:|---:|
| GPU total median / p99 / max, ms | 36.67 / 365.35 / 442.23 | 99.85 / 304.55 / 308.62 |
| Frame intervals over 100 ms | 47/312 | 86/176 |
| Frame intervals over 1 s | 0/312 | 0/176 |
| VT median / p95 / p99, ms | 0.00 / 165.55 / 331.69 | 56.14 / 215.42 / 261.34 |
| VT fill median / p95 / p99, ms | unavailable | 45.39 / 129.92 / 163.89 |
| VT enrich median / p95 / p99, ms | unavailable | 20.71 / 119.34 / 136.17 |

Trace counters show 130 fill frames in the final sample, each with one page.
The before sample had 23 fill frames in 312 frames, mostly eight pages per
batch. The quota spread the pending work over many more frames. The aggregate
VT p99 improved, but the VT median, GPU median, and count of intervals over
100 ms regressed. Even one fill page and AO enrichment each cost more than the
16 ms VT-zone target in this capture. A page-count budget cannot cap a single
page's GPU cost. Meeting the target requires splitting the page bake/BC work
across frames or reducing the cost of one bake, and separately budgeting AO
enrichment. The current candidate therefore does **not** meet this task's
performance acceptance criterion. The task-1 baseline's larger visible
geometry population makes its absolute totals unsuitable for a direct speedup
claim.

The focused C++ smoke tests passed for VT queue scheduling, direct-source
material, compositor, input snapshot, and surface material after guarding
timestamp writes in standalone raster fixtures. Builds and tests were run
serially with the MSVC RelWithDebInfo toolchain.


## Reopened retry: bounded slices and exact triangle pruning

The previous page-count quota could not divide one expensive page. The retry
uses independent **4 ms fill and 4 ms AO targets**, with prices from retired
GPU timestamps. A resumable producer records one page's 1–2 horizontal
32-texel tiles per frame (680 tiles cover all 136 rows, including gutters);
an expensive sample raises its estimated tile cost immediately, while recovery
is deliberately slower. When the two minimum-progress slice estimates exceed
their combined targets, fill and AO alternate frames. At least one tile remains
admissible for each scheduled producer. Setting either
budget to zero restores that producer's whole-page behavior.

Composition carries private intermediates forward through the fence-safe
batch rings. Its immutable owner, input snapshot, revision, mip and page key
must still match. The final height-normal resolve, BC encoding and destination
copy occur on a separate completion frame; partial rows never replace a
resident page or its material bindings. Continuations retain their queue age
and run before other pages. AO similarly retains a private factor lease and
tile cursor, separates an AS-build-only frame from tracing, and publishes only
a complete factor. Slot reuse, revision changes and setting changes cancel
and retire old factors through the existing GPU horizon.

Slicing alone was insufficient. The intermediate 8x1 dispatch candidate in
`C:/tmp/attr_clear_ridge6_sliced` measured VT p99 **89.43 ms**, fill p99
50.09 ms and AO p99 39.45 ms even with one-row work. Inspection found that the
shared surface resolver still scanned every triangle of each candidate chart.
The final candidate adds a plane-space bounds hierarchy over **all** triangles
for composition and AO. It prunes exact nearest-distance searches, preserving
original triangle order and strict first-triangle ties. It does not inherit
the existing 2,048-triangle POM seed bound. Small charts retain linear search.
Composition builds/uploads the immutable hierarchy through the existing
bounded CPU preparation and upload path; its allocation is included in the
memory census. Horizontal 32x1 workgroups keep a full warp in each row group.

### Initial sliced candidate: 45 s warmup / 20 s sample

The before run rebuilt `d6fe97de` (the previous failed candidate); after used
the retry source on that branch. Both use the default StreamMountain camera,
1920x1080 visible window, immediate presentation and POM off. Raw evidence:
`C:/tmp/attr_clear_ridge6_retry_before` and
`C:/tmp/attr_clear_ridge6_bvh_final`. The after editor SHA-256 is
`347d870678eac393bf63202166f06fd1444d5d751ea9400b14023aa4542a70a5`.
The after FIFO also appended read-only `stats` snapshots every 60 frames.

| 45 s warmup | Task-1 baseline (3 runs) | Retry before | Retry after |
|---|---:|---:|---:|
| Sampled frames | 172 pooled | 230 | 475 |
| GPU total median / p99 / max, ms | median 227.5–248.3; p99=max 394.1–594.3 | 50.29 / 251.09 / 330.16 | 40.71 / 54.73 / 64.17 |
| Frame interval median / p99 / max, ms | 233.23 / 3807.05 / 3939.87 pooled | 52.09 / 287.68 / 469.56 | 40.70 / 118.65 / 156.17 |
| Frame intervals over 100 ms | 161/172 | 81/230 | 8/475 |
| Frame intervals over 1 s | 6/172 | 0/230 | 0/475 |
| VT median / p95 / p99, ms | p99 162.6–354.6 | 15.91 / 172.40 / 218.23 | 1.77 / 4.73 / 5.48 |
| VT fill median / p99, ms | unavailable | 25.96 / 171.57 | 1.11 / 4.80 |
| VT enrich median / p99, ms | unavailable | 4.74 / 123.54 | 0.00 / 3.94 |
| Static uploads in sample | 5–7/run | 6 | 8 |
| Peak whole-GPU VRAM, MiB | 12,545–12,688 | 12,725 | 12,746 |

After maxima are **6.62 ms VT**, 5.70 ms fill and 4.53 ms AO. POM is false
and Vulkan validation errors are zero. GPU p99 is below 100 ms; the eight
frame intervals above 100 ms are therefore not VT GPU spikes. Geometry and
lighting still dominate the GPU: G-buffer median 18.67 ms and GI median
16.14 ms. Scene population continues changing; task 1 had a larger G-buffer
cost, so its absolute GPU totals are contextual, not an isolated code speedup.

The aggregate target is met while work advances: immediately before sampling,
`STATSVT` showed 4 completed fills and no published AO factors; during the
sample those counts reached 13 and 9. Variants grew 163 to 172, there were zero
admission rejects or evictions, and queued demand remained 193 at the last
snapshot. The trace tail has 512 fill attempts, 503 row-slice frames spanning
1–8 rows, and nine remaining completion frames. This is paced background
work, not a claim that the whole queue finished within the sample.

### Initial sliced candidate: later queued pages at 300 s

`C:/tmp/attr_clear_ridge6_bvh_w300` used the same measured binary and added
read-only snapshots every 30 frames. This extends the audit to later queued
pages; there is no matched retry-before 300 s run. Compare with task 1's late
baseline as context:

| 300 s warmup | Task-1 baseline (3 runs) | Sliced candidate |
|---|---:|---:|
| Sampled frames | 141 pooled | 268 |
| GPU total median / p99 / max, ms | median 402.1–454.2; p99=max 433.5–811.4 | 74.55 / 90.01 / 91.18 |
| Frame interval median / p99 / max, ms | 409.49 / 922.18 / 1061.75 pooled | 74.17 / 145.05 / 192.97 |
| Frame intervals over 100 ms | 138/141 | 4/268 |
| Frame intervals over 1 s | 1/141 | 0/268 |
| VT median / p95 / p99 / max, ms | p99 0.02–371.1 | 9.84 / 14.31 / 15.22 / 15.95 |
| Fill median / p99 / max, ms | unavailable | 5.32 / 7.43 / 7.70 |
| AO median / p99 / max, ms | unavailable | 5.04 / 7.92 / 8.25 |
| Static uploads in sample | 2–3/run | 4 |
| Peak whole-GPU VRAM, MiB | 12,927–13,071 | 15,409 |

POM is false and validation errors are zero. The VT target still holds, with
less margin: every priced fill/AO slice in the trace tail has only one row.
The minimum-progress rule can exceed the 4 ms estimate for a complex row.
Completed fills grew 57 to 59 and published AO factors 50 to 52 during the
sample; 221 variants and 261 queued requests remained at the final snapshot.
G-buffer/GI medians increased to 37.30/22.80 ms. The snapshot near the sample
start has 2,354 active instances and 17.07 million raster triangles. This run
checks continuing work; it does not establish fully drained VT coverage or a
whole-frame 16 ms target.

### Corrected-binary recheck and pacing refinement

The AO build-only review moved its factor clear to the first tracing frame,
so an AS-only submission does not redundantly clear row-zero storage before
its retry. The native test now forces a cold AS and checks that deferral before
comparing every factor row. Source commit: `06d8923a`.

That binary's `C:/tmp/attr_clear_ridge6_corrected_final` recheck sampled 463
frames: GPU median/p99/max 41.24/54.76/63.58 ms, intervals above 100 ms 5/463,
above 1 s zero, POM false and validation errors zero. **VT p99/max was
16.56/17.95 ms, so the target was not met in this recheck.** AO p99/max was
15.40/16.05 ms, fill 10.83/11.98 ms. It reached more completed pages than the
initial early capture (32 fills and 28 AO factors in its last snapshot),
exposing another expensive page and the delay in learning its price.

The next scheduler limited each priced slice to two rows, including after
cheap earlier pages. If the fill and AO minimum-row estimates together exceed
their combined targets, it schedules them on alternating frames. The CPU
budget checks cover paired-cost decisions and disabled targets. The Vulkan
queue test injects expensive retired samples, verifies fill-only then AO-only
progress, and still requires both factors to complete after cancellation.

The `53e4d04a` two-row candidate in
`C:/tmp/attr_clear_ridge6_pacing_final_w45` sampled 468 frames. GPU
median/p99/max was 38.64/55.04/70.38 ms, with 5/468 intervals above 100 ms
and none above 1 s; POM was false and validation errors zero. **VT p99/max
17.61/17.82 ms still missed the target**, with fill p99/max 17.62/17.81 ms
and AO 3.98/12.39 ms. The trace tail contains 261 one-row and 135 two-row
fill slices. A whole row is still too large a minimum-progress unit.

The final refinement flattens composition and AO dispatch into independent
horizontal 32-texel workgroups. The last workgroup in each row masks its
padding lanes. Adaptive prices, paired scheduling, cancellation and private
publication now operate in tiles, so an expensive row can advance across
multiple frames. The same native GPU comparison checks all channels and
packed AO values against full-page output, including slices that cross rows.

### Final 32-texel slice captures

Source `f10ae70193842b81a57d00cae6c8a6eb40d6b7a2`, editor SHA-256
`fea527707fe010a7048b0dc92d23caea5e383b71ac967a685dc71e373109b64c`.
Raw evidence: `C:/tmp/attr_clear_ridge6_tiles_final_w45` and
`C:/tmp/attr_clear_ridge6_tiles_final_w300`. Both retain the canonical camera,
presentation, readiness, warmup and sampling protocol. The POM-off command
file uses frame waits and read-only census snapshots every 30 frames after
perf readiness, so a previously fired bake event cannot block the snapshots.

| 45 s warmup / 20 s sample | Task-1 baseline (3 runs) | Matched retry before | Final tile slices |
|---|---:|---:|---:|
| Sampled frames | 172 pooled | 230 | 486 |
| GPU total median / p99 / max, ms | median 227.5–248.3; p99=max 394.1–594.3 | 50.29 / 251.09 / 330.16 | 38.97 / 52.61 / 65.85 |
| Frame interval median / p99 / max, ms | 233.23 / 3807.05 / 3939.87 pooled | 52.09 / 287.68 / 469.56 | 39.67 / 113.97 / 145.79 |
| Frame intervals over 100 ms | 161/172 | 81/230 | 8/486 |
| Frame intervals over 1 s | 6/172 | 0/230 | 0/486 |
| VT median / p95 / p99 / max, ms | p99 162.6–354.6 | 15.91 / 172.40 / 218.23 / 296.49 | 0.78 / 1.74 / 1.91 / 2.22 |
| Fill median / p99 / max, ms | unavailable | 25.96 / 171.57 / 172.64 | 0.60 / 1.91 / 2.22 |
| AO median / p99 / max, ms | unavailable | 4.74 / 123.54 / 160.95 | 0.00 / 0.64 / 1.05 |
| Static uploads in sample | 5–7/run | 6 | 9 |
| Peak whole-GPU VRAM, MiB | 12,545–12,688 | 12,725 | 12,747 |

The early final-binary run meets the VT target, with POM false and zero
validation errors. In its timed sample completed fills increase **1 to 2**;
AO factors remain zero. The trace tail contains 512 fill attempts, 511 actual
tile slices (86 one-tile and 425 two-tile), one completion frame and 175
two-tile AO slices. The early sample checks recorded AO work, not a completed
factor. Queued demand grows 196 to 207 and variants 163 to 175, with zero
evictions or admission rejects. G-buffer and GI medians are 18.96 and
15.55 ms.

| 300 s warmup / 20 s sample | Task-1 baseline (3 runs) | Final tile slices |
|---|---:|---:|
| Sampled frames | 141 pooled | 303 |
| GPU total median / p99 / max, ms | median 402.1–454.2; p99=max 433.5–811.4 | 65.03 / 74.89 / 86.29 |
| Frame interval median / p99 / max, ms | 409.49 / 922.18 / 1061.75 pooled | 65.51 / 79.02 / 155.56 |
| Frame intervals over 100 ms | 138/141 | 1/303 |
| Frame intervals over 1 s | 1/141 | 0/303 |
| VT median / p95 / p99 / max, ms | p99 0.02–371.1 | 2.84 / 7.55 / 8.25 / 8.69 |
| Fill median / p99 / max, ms | unavailable | 2.61 / 6.69 / 6.81 |
| AO median / p99 / max, ms | unavailable | 0.43 / 8.14 / 8.69 |
| Static uploads in sample | 2–3/run | 1 |
| Peak whole-GPU VRAM, MiB | 12,927–13,071 | 15,436 |

The same final binary meets the target on later queued pages, with POM false
and zero validation errors. During warmup completed fills advance 0 to 9 and
AO factors 0 to 6. In the late timed sample fills remain at **9**, while AO
factors increase **6 to 7**. Fill work continues: the 512-frame trace tail
contains 351 fill slices covering 545 tiles and 351 AO slices covering 501
tiles, each slice one or two workgroups. Queued demand grows 246 to 248 and
variants 221 to 223, with no admission rejects or evictions. This demonstrates
ongoing production, not a drained queue or fast complete-page latency.
G-buffer/GI medians are 34.33/23.29 ms, so total-frame cost remains above the
VT-specific target. There is no matched retry-before 300 s capture; task-1
totals are contextual because geometry and residency populations differ.

### Verification and limits

MSVC RelWithDebInfo builds passed for `matter_editor`, `vulkan_smoke_tests`,
`vt_compositor_tests` and `vt_residency_tests`. C++ suites ran serially:

Final tile-source checks: CPU budget/residency 0.30 s, compositor 28.60 s,
input snapshot 22.93 s, surface material 21.25 s, queue 1.28 s and link
inventory 0.55 s. The direct-source fixture first exhausted its old 240-frame
settle limit; after waiting for bounded tile completion, its retest passed
47.75 s. All targeted tests pass on the final source.

- `vt_residency_tests`: budget feedback/recovery/disabled behavior and complete
  hierarchy leaf coverage/escape structure, including a 2,200-triangle chart.
- `vt_compositor_tests`: 8,395 pages, zero skips and zero validation errors.
  Full accelerated output matches an independent linear GPU oracle beyond the
  seed limit, including gutters and ties. Sliced composition matches all five
  whole-page channels after supersession and multiple ring wraps. The native
  AO shader writes every packed factor row and matches whole-page output.
- `smoke_vt_queue`: incomplete pages remain unpublished; continuations keep
  priority and age; private AO leases/cursors survive slices and are discarded
  on invalidation without stale publication.
- `vt_direct_source_tests`, `vt_surface_material_tests`,
  `vt_input_snapshot_tests` and `vt_link_inventory_tests`: all pass. Surface
  fixtures now wait for bounded completion. The descriptor-bank pressure
  fixture explicitly uses whole pages for its deliberate edit-every-frame
  stress; sliced lifetime/publication checks are separate above.

The budgets are scheduling estimates, not GPU preemption. One tile, completion
encoding or an acceleration-structure build can exceed its estimate; measured
aggregate p99/max establish the result for these captures. Per-page latency is
longer than whole-page dispatch, and pending pages retain existing coverage or
legacy material fallback. Legacy ORM-in-place enrichment remains whole-page;
the production separate-factor path supports slicing. CPU registration,
geometry uploads and AS preparation are not bounded by this GPU slice budget.
