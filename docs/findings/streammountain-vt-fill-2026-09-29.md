# StreamMountain VT page-fill pacing — 2026-09-29

Task `clear-ridge.6`, POM off. All captures used
`VARIANTS=pom_off RUNS=1 WARMUP=45 SAMPLE=20
tools/streammountain_attribution.sh C:/tmp/<out>` on the RTX 4090 with the
MSVC RelWithDebInfo editor. Raw logs, traces, and perf JSON remain under
`C:/tmp/attr_clear_ridge6_*`; the executable hash is in each output folder.
Runs are single samples during a changing stream. The task-1 baseline used a
larger visible geometry population, so its absolute GPU times are context,
not an equal-scene speed comparison.

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

## Changes under measurement

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

## Final zero-resolve capture

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
