# High-density geometry performance decision — 2026-09-30

Task `clear-ridge.10`. POM-off StreamMountain, three repeats per setup at
45 s and 300 s warmup. **Keep the tuned static/source path as the shipping
default. Retain VG as an opt-in development path; do not promote it to the
default on this evidence.** The 16.7 ms whole-frame endpoint and elimination
of all intervals over 100 ms remain unachieved. This is a completed
measurement/report task, not a performance-target pass.

## Decision and limits

The static path is the existing default. Both current paths include the
static-buffer, VT slice, vertex-cache and GI changes accumulated through
`46cb2266`. Only the paged path enables virtual-geometry page streaming
for terrain and rocks; ordinary source-sector streaming runs in both.
The matched current-build comparison drives the default recommendation;
the six preserved task-1 captures supply historical context.

Early VG can have lower GPU medians and lower whole-device VRAM, but its
end-to-end cadence has more >100 ms intervals and its captured view is less
complete and coarser. The late measurements and residency census below
must be read together: identical camera and wall-clock warmup do not fix
the streamed detail population. These results establish behavior of the
two launch paths, not an isolated algorithmic speedup or equal-quality
VG efficiency.

At 300 s, GPU median-of-run-medians is 58.26 ms static versus 268.51 ms
VG (4.61×); all 186 VG intervals exceed 100 ms versus 17/987 static.
Peak whole-GPU VRAM is 15,436–15,467 MiB static versus 10,019–10,034
MiB VG, at different detail/residency. Neither path has a sampled interval
over one second, which does not cover startup or teardown.

VG remains useful to develop bounded geometry admission, persistent cuts
and selective scene reuse. Production promotion needs a fully covered
view at equivalent detail/material readiness, a fixed-population A/B,
bounded changed-scene/instance publication, and repeating the same
early/late cadence gates. Retaining this opt-in research path does not
justify its current default adoption.

## Protocol and provenance

| Item | Value |
|---|---|
| Current source | `7e65df70e`; final implementation `46cb2266`. The difference is documentation only. |
| Build | Native MSVC RelWithDebInfo, `./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor`, exit 0. |
| Current executable SHA-256 | `71327dc9a9f1894d951b50f963cc521525ca82eea8ae5b2fa0c0db0f41cd73f2` |
| GPU / driver | RTX 4090, NVIDIA 610.74; each run admitted by the <2,048 MiB whole-GPU idle gate. |
| Camera / output | Default eye (380,90,1600) → target (420,55,1420); 1920×1080 native, visible window, hidden UI, IMMEDIATE, frame limit 0. |
| Scene | StreamMountain world authoring/props unchanged since task 1; RT/GI and volumetrics enabled, POM verified disabled. |
| Warmup gate | Static uploads unchanged for 30 frames, then 45 or 300 wall-clock seconds; 20 s timed sample. Streaming can continue. |
| Current repeats | 3 static + 3 VG at each warmup, all serial; no concurrent C++ suites or other editor captures. |
| VG settings | Terrain/pages enabled, MountainDetailRock, minimum 16,384 triangles; root/CPU 1,024 MiB; GPU 3,072 MiB; VT 2,048 MiB; cache-only. |
| Historical baseline | `3bafe0495456602f1472f468a50b28272d984b97`, September 28; 3 repeats each at 45/300 s, same camera/output/POM-off protocol. |
| Current host load | Monitored WSL load1 1.83–9.78 from 02:58:28 to 04:13:53 PDT, including loading; the first static run predates this logger. Host load and GPU clocks were not locked. |
| Baseline caveat | Original WSL load 8.7–16.5, Windows CPU sample 42–53%; different streamed populations and intervening changes. Supervisor authorized reuse, not fresh recapture. |
| Artifacts | Current: `C:/tmp/clear-ridge10-20260930/{static,vg}-w{45,300}`; baseline: `C:/tmp/clear-ridge-1-w{45,300}`. |

The campaign driver records its ordered commands in `protocol.json`;
the final executable is preserved outside the recyclable slot in the
artifact root. GPU memory/utilization is sampled every five seconds.
The committed [capture summary](../agent/evidence/2026-09-30-hdgeo-performance/summary.json)
retains every cadence sample, per-pass GPU statistics, trace summaries,
settings, residency snapshots and input hashes. The
[reducer](../agent/evidence/2026-09-30-hdgeo-performance/reduce_captures.py)
recomputes it from the retained raw artifacts and validates all 18 runs.

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
# Execute one command at a time; use fresh output directories.
VARIANTS=pom_off RUNS=3 WARMUP=45 tools/streammountain_attribution.sh C:/tmp/hdgeo-repeat/static-w45
PAGED_TERRAIN=1 PAGED_CACHE_ONLY=1 VARIANTS=pom_off RUNS=3 WARMUP=45 tools/streammountain_attribution.sh C:/tmp/hdgeo-repeat/vg-w45
VARIANTS=pom_off RUNS=3 WARMUP=300 tools/streammountain_attribution.sh C:/tmp/hdgeo-repeat/static-w300
PAGED_TERRAIN=1 PAGED_CACHE_ONLY=1 VARIANTS=pom_off RUNS=3 WARMUP=300 tools/streammountain_attribution.sh C:/tmp/hdgeo-repeat/vg-w300
# Revalidate/reduce the original retained 18 captures:
python3 docs/agent/evidence/2026-09-30-hdgeo-performance/reduce_captures.py
```

## All-run comparison

GPU values use the timestamped `total` pass, not the legacy moving-average
`gpu_total_ms`. Cadence values use `frame_times_ms` and include CPU stalls.
Quantiles are nearest-rank; cadence medians use the sample median.
Hitches are strictly >100 ms and >1,000 ms. GPU quantiles remain per run;
only cadence samples are pooled. GPU p99 equals max when a run has fewer
than 100 GPU samples. These fixed-camera, warm-cache twenty-second
windows do not cover cold startup, moving-camera streaming, or teardown.
For example, static late r2 logged a 3,203.4 ms eviction operation after
the trace/sample was written; it is outside the timed hitch census and
must not be interpreted as eliminated by a zero sampled >1 s count.

| Setup / warmup | GPU medians, range ms | GPU p99, range ms | Worst GPU max ms | Pooled cadence median / p99 / max ms | >100 ms | >1 s | Peak VRAM range MiB |
|---|---|---|---|---|---|---|---|
| historical-baseline / 45s | 227.50–248.29 | 394.12–594.33 | 594.33 | 233.23 / 3807.05 / 3939.87 | 161/172 (93.60%) | 6/172 | 12545.00–12688.00 |
| static / 45s | 34.24–46.99 | 45.23–100.39 | 108.89 | 39.88 / 101.84 / 191.48 | 23/1400 (1.64%) | 0/1400 | 12492.00–12575.00 |
| vg / 45s | 24.86–33.66 | 43.61–103.19 | 106.48 | 34.48 / 133.22 / 208.07 | 87/1359 (6.40%) | 0/1359 | 9452.00–9527.00 |
| historical-baseline / 300s | 402.11–454.17 | 433.52–811.37 | 811.37 | 409.49 / 922.18 / 1061.75 | 138/141 (97.87%) | 1/141 | 12927.00–13071.00 |
| static / 300s | 57.50–58.51 | 79.51–80.78 | 102.95 | 58.41 / 147.93 / 209.05 | 17/987 (1.72%) | 0/987 | 15436.00–15467.00 |
| vg / 300s | 260.91–270.76 | 281.36–285.87 | 285.87 | 309.21 / 597.16 / 612.25 | 186/186 (100.00%) | 0/186 | 10019.00–10034.00 |

Historical GPU median-of-run-medians changes (descriptive, not causal):

- 45s: baseline 238.57 ms → static 41.35 ms (82.7% lower); VG 32.89 ms. Current VG/static GPU ratio 0.80×, with unequal residency/detail.
- 300s: baseline 402.35 ms → static 58.26 ms (85.5% lower); VG 268.51 ms. Current VG/static GPU ratio 4.61×, with unequal residency/detail.

### 45-second warmup: individual repeats

| Setup / repeat | Frames | GPU median / p99 / max ms | Cadence median / p99 / max ms | >100 ms | >1 s | Static uploads | Peak VRAM MiB |
|---|---|---|---|---|---|---|---|
| historical-baseline / r1 | 47 | 248.29 / 594.33 / 594.33 | 253.03 / 3698.51 / 3698.51 | 44 | 3 | 6 | 12679 |
| historical-baseline / r2 | 72 | 227.50 / 394.12 / 394.12 | 226.47 / 3807.05 / 3807.05 | 68 | 1 | 5 | 12688 |
| historical-baseline / r3 | 53 | 238.57 / 421.66 / 421.66 | 238.26 / 3939.87 / 3939.87 | 49 | 2 | 7 | 12545 |
| static / r1 | 479 | 41.35 / 45.23 / 62.61 | 41.19 / 51.97 / 134.73 | 2 | 0 | 3 | 12511 |
| static / r2 | 559 | 34.24 / 45.73 / 57.64 | 35.14 / 81.40 / 124.04 | 2 | 0 | 7 | 12575 |
| static / r3 | 362 | 46.99 / 100.39 / 108.89 | 47.63 / 114.08 / 191.48 | 19 | 0 | 8 | 12492 |
| vg / r1 | 572 | 33.66 / 43.61 / 44.56 | 33.59 / 78.12 / 96.29 | 0 | 0 | 104 | 9452 |
| vg / r2 | 306 | 32.89 / 103.19 / 106.48 | 55.35 / 154.14 / 208.07 | 68 | 0 | 180 | 9527 |
| vg / r3 | 481 | 24.86 / 85.51 / 86.82 | 30.83 / 123.93 / 178.70 | 19 | 0 | 283 | 9472 |

### 300-second warmup: individual repeats

| Setup / repeat | Frames | GPU median / p99 / max ms | Cadence median / p99 / max ms | >100 ms | >1 s | Static uploads | Peak VRAM MiB |
|---|---|---|---|---|---|---|---|
| historical-baseline / r1 | 50 | 402.11 / 434.67 / 434.67 | 394.48 / 835.10 / 835.10 | 49 | 0 | 3 | 12955 |
| historical-baseline / r2 | 50 | 402.35 / 433.52 / 433.52 | 403.53 / 847.15 / 847.15 | 50 | 0 | 2 | 12927 |
| historical-baseline / r3 | 41 | 454.17 / 811.37 / 811.37 | 451.17 / 1061.75 / 1061.75 | 39 | 1 | 2 | 13071 |
| static / r1 | 338 | 57.50 / 79.51 / 102.95 | 57.63 / 155.43 / 209.05 | 5 | 0 | 8 | 15436 |
| static / r2 | 328 | 58.51 / 80.78 / 93.50 | 58.88 / 144.12 / 183.43 | 6 | 0 | 9 | 15467 |
| static / r3 | 321 | 58.26 / 79.69 / 88.54 | 59.03 / 131.35 / 207.50 | 6 | 0 | 9 | 15467 |
| vg / r1 | 63 | 268.51 / 285.87 / 285.87 | 310.72 / 562.97 / 562.97 | 63 | 0 | 63 | 10019 |
| vg / r2 | 67 | 260.91 / 281.36 / 281.36 | 293.19 / 572.70 / 572.70 | 67 | 0 | 67 | 10034 |
| vg / r3 | 56 | 270.76 / 285.37 / 285.37 | 337.27 / 612.25 / 612.25 | 56 | 0 | 56 | 10021 |

### Pooled cadence histograms

| Frame interval | Baseline 45s | Static 45s | VG 45s | Baseline 300s | Static 300s | VG 300s |
|---|---|---|---|---|---|---|
| 0–16.7 ms | 0 | 6 | 18 | 0 | 7 | 0 |
| 16.7–33.3 ms | 1 | 193 | 613 | 1 | 5 | 0 |
| 33.3–50 ms | 0 | 1040 | 429 | 0 | 53 | 0 |
| 50–100 ms | 10 | 138 | 212 | 2 | 905 | 0 |
| 100–250 ms | 119 | 23 | 87 | 2 | 17 | 8 |
| 250–500 ms | 33 | 0 | 0 | 118 | 0 | 170 |
| 500–1000 ms | 3 | 0 | 0 | 17 | 0 | 8 |
| 1000–2000 ms | 1 | 0 | 0 | 1 | 0 | 0 |
| >2000 ms | 5 | 0 | 0 | 0 | 0 | 0 |

## GPU attribution, CPU work and residency

Ranges below are across three repeats at each warmup. Passes nest and
do not sum. Missing child GI timers in the historical baseline are not
zero cost; the aggregate `rt_gi` is the comparable field.

| Setup / warmup | G-buffer median ms | Aggregate GI median ms | VT p99 ms | VT worst max ms |
|---|---|---|---|---|
| historical-baseline / 45s | 159.40–170.15 | 54.28–55.76 | 162.61–354.59 | 354.59 |
| static / 45s | 17.19–17.74 | 11.84–11.96 | 3.70–67.78 | 68.66 |
| vg / 45s | 9.99–14.39 | 10.72–12.05 | 0.62–4.51 | 4.63 |
| historical-baseline / 300s | 306.95–329.98 | 80.34–102.22 | 0.02–371.12 | 371.12 |
| static / 300s | 30.69–32.12 | 19.01–19.41 | 7.42–23.49 | 23.93 |
| vg / 300s | 134.65–138.28 | 55.36–57.83 | 0.28–0.87 | 0.87 |

The previous VT fill finding’s <16 ms p99 result is not consistently
reproduced by these static repeats: the early maximum p99 is 67.78 ms,
late 23.49 ms. Adaptive fill budgets remain estimates, and the underlying
per-page latency/backlog and budget calibration remain open. The much
slower late VG path has VT p99 below 1 ms; its G-buffer, GI and CPU
geometry/RT work dominate instead. Pass medians do not add into a causal
whole-frame breakdown.

CPU measurements use the matching perf window from ProfileLib’s
512-frame ring. If a sample is longer, only its available tail is used;
the denominator below makes that explicit. `geometry.update` contains
nested scene work. The final JSON `loop_render_ms` is a latest EMA and
is intentionally excluded from window means.

| Current run | Trace frames / perf frames | Geometry update mean / p95 ms | Scene assembly mean / p95 ms | RT-scanned mean | LOD-scanned mean | Scene-reuse frames |
|---|---|---|---|---|---|---|
| static 45s r1 | 479/479 | absent | absent | 1720 | 195 | — |
| static 45s r2 | 512/559 | absent | absent | 1722 | 198 | — |
| static 45s r3 | 362/362 | absent | absent | 1710 | 187 | — |
| vg 45s r1 | 512/572 | 3.91 / 11.17 | 0.13 / 0.00 | 1511 | 140 | 491/512 |
| vg 45s r2 | 306/306 | 12.86 / 39.28 | 2.89 / 17.03 | 3408 | 131 | 243/306 |
| vg 45s r3 | 481/481 | 8.11 / 26.99 | 1.51 / 12.00 | 2059 | 122 | 408/481 |
| static 300s r1 | 338/338 | absent | absent | 2470 | 315 | — |
| static 300s r2 | 328/328 | absent | absent | 2463 | 313 | — |
| static 300s r3 | 321/321 | absent | absent | 2469 | 316 | — |
| vg 300s r1 | 63/63 | 26.16 / 32.07 | 2.71 / 0.00 | 26298 | 269 | 60/63 |
| vg 300s r2 | 67/67 | 26.06 / 47.67 | 2.54 / 0.00 | 25878 | 271 | 64/67 |
| vg 300s r3 | 56/56 | 50.12 / 216.56 | 14.78 / 99.31 | 28278 | 268 | 46/56 |

### Geometry coverage and memory

These are the last logged coverage observations and independent maxima
of consumer reservation counters over each whole launch, not necessarily
a simultaneous snapshot or a settled scene. Whole-GPU `nvidia-smi` peaks
include Windows and every other GPU allocation, cover load/warmup/sample
and shutdown, and have five-second sampling granularity. Engine budget
reservations and host-visible allocations are different metrics.

| VG run | Visible ready / desired roots | Visible unready / assets | Global source fallbacks | Peak geometry reserved MiB | Peak VT pool MiB |
|---|---|---|---|---|---|
| 45s r1 | 1535/2443 | 41/53 | 424 | 257.34 | 2032.03 |
| 45s r2 | 1612/1811 | 2/45 | 5 | 400.77 | 2032.03 |
| 45s r3 | 1335/1443 | 1/41 | 4 | 341.45 | 2032.03 |
| 300s r1 | 5280/5684 | 5/93 | 5 | 1087.79 | 2032.03 |
| 300s r2 | 5525/6156 | 7/95 | 8 | 1137.62 | 2032.03 |
| 300s r3 | 5286/5597 | 4/92 | 4 | 1093.73 | 2032.03 |

Cache-only forbids missing geometry-page compilation. Ordinary
source-sector preparation and streaming continue. Cache-only
does not prove every source fallback has a ready paged replacement.
The initial thirty-static-stable-frame gate also does not prove world
or VG residency is idle. Continued upload counts and incomplete visible
coverage are acceptance limits, not reasons to drop inconvenient runs.

## Visual observations

Warmup screenshots are requested outside timed samples, 20 s after
observing the early warmup marker and 180 s after the late marker.
The `.done` markers
must say `captured`. The early static image contains the detailed rock
field, trees and ground; the early VG image has large missing
terrain/vegetation areas and visibly faceted, smoother rock surfaces.
It does not demonstrate retained high-density material/detail quality.
The late VG r2 image has substantially more terrain and vegetation,
but many foreground rocks show prominent triangular facet/crease
patterns and the terrain appears brighter than static r2. These
appearance differences prevent a visual-parity acceptance; their
cause is not isolated by this benchmark. Scene/VT readiness and
geometry/material output need a controlled comparison before promotion.
Both retain the existing dark-blue lighting and horizon/sky artifacts.
Preserved images: [static early](../agent/evidence/2026-09-30-hdgeo-performance/static-w45-r1.png),
[VG early](../agent/evidence/2026-09-30-hdgeo-performance/vg-w45-r1.png),
[static late](../agent/evidence/2026-09-30-hdgeo-performance/static-w300-r2.png),
[VG late](../agent/evidence/2026-09-30-hdgeo-performance/vg-w300-r2.png).
The evidence README records their repeat IDs and hashes. This is a visual/load comparison, not a pixel-equality
oracle. No sampling, ray-budget or quality setting was lowered for timing.

The screenshot monitor hit `OSError: [Errno 61] No data available`
while reading active native log files. The capture runner continued
successfully. The monitor was restarted with read-error retries;
the late static screenshot therefore comes from a later repeat.

Geometry diagnostics also interleaved inside the first late VG warmup
`printf`. Phase and duration tokens plus the preserved launch recipe
verify that run; the reducer does not require one uninterrupted log line.
The late VG screenshot is likewise from repeat 2.

## Verification and remaining gates

- Canonical MSVC editor rebuild: exit 0; build log and executable retained.
- All twelve current captures: runner exit 0, POM false, full production G-buffer mode, 1920×1080, effective RT, validation errors 0. Six baseline artifacts revalidated with the same field/recipe checks.
- `python3 -m unittest discover -s tools/tests -p test_frame_attribution.py`: 11 tests pass.
- `bash -n tools/streammountain_attribution.sh`: pass. `git diff --check`: pass.
- Retained checks for unchanged final implementation: native Vulkan `rt`, `rt-transmission`, `rt-local-direct` and `vt-composed-parallax` all report `ALL PASS`, validation errors 0. These were inspected and preserved, not rerun during this measurement campaign. Task 8’s cut/hierarchy/PartStore/geometry-pages checks remain in its findings.
- Retained `shader_source_tests` exits 9 at the unchanged source-string assertion for `if (instance.water_pad0 != 0u) return;`, already recorded in G-buffer split/GI findings; no assertion or shader was weakened.

The static default is the better shipping choice supported by the
current coverage/pacing evidence. It still needs geometry/raster/GI
work to reach 16.7 ms, and publication/fill work to remove the remaining
>100 ms intervals. VG must first retain equivalent scene coverage and
detail, then bound cut/publication/RT-instance work and prove benefit
on the same loaded population. No default toggle or engine implementation
changed in this task; its deliverables are measurements, evidence and
the review submission.

Related authority: [task-1 baseline](streammountain-pom-off-baseline-2026-09-28.md),
[frame attribution](streammountain-frame-attribution-2026-09-27.md),
[persistent cuts](streammountain-persistent-cut-2026-09-29.md),
[GI final acceptance](streammountain-gi-attribution-2026-09-29.md),
[work queue](../vg-vt-work-queue-2026-09-19.md).
