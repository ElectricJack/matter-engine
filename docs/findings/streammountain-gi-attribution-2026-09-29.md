# StreamMountain GI attribution and shader specialization — 2026-09-29

`clear-ridge.9`, queue row 1.14. Resumed captures span September 29–30.
The shader fix, early/late before/after and default-camera visual check are
complete. Claim epoch 4 captured the final late sample during the coordinated
GPU window on September 30; earlier attempts were blocked by shared-GPU occupancy.
Diffuse is the dominant GI lane: 12.02 ms versus 0.53 ms for combined
reflection/transmission in the early reference, and 18.90 versus 0.75 ms
in the late reference. It accounts for about 96% of the aggregate GI median.
The final early/late diffuse medians fall to 11.39/18.28 ms (−5.2%/−3.3%).
This is a modest shader-execution improvement; the much larger gains since
task 1 belong primarily to intervening work. Whole-frame tails and hitch
counts do not improve consistently.

## Change and output contract

The former GI recorder combined diffuse and reflection/transmission at equal
resolutions, leaving the child queries unavailable even with detail timers
enabled. Checkpoint `15753fbd` added a profiling-only split using the existing
signal ownership masks. `LIGHTING_DETAIL=1` enables those timers through
`WSLENV`; the `rt_gi` aggregate contains both child intervals, so do not sum
the children with their parent.

`46cb2266` adds two specialized raygen stages from the existing lighting
module. Specialization constant 11 fixes diffuse or reflection/transmission
ownership at pipeline compilation, allowing irrelevant lane code and the
runtime direct-lighting branch to be removed. The default now traces the
separate compiled lanes, including when their extents match. Two aligned
raygen records follow the existing SBT records; the optional primary-lighting
layout is retained and tested. The scene/material inputs are read-only and
the lanes write disjoint signal images before the existing denoiser.

`MATTER_RT_GI_SPECIALIZE=0` selects the original runtime-mask raygen; at equal
extents it combines signals unless detail timers request the profiling split.
`GI_SPECIALIZE=0|1` exposes this override in the attribution script. This
switch isolates raygen selection, not every change in the commit.

The shared closest-hit loader also used to compute the parallax triangle
metric and call the composed-height sampler when `pom_steps=0`. That sampler
resolved the VT address again but could only return a zero displacement.
The loader now skips this metric when the step count is zero, and the height
helper returns before march-only transforms and address resolution. Ordinary
ray-cone UV density and VT albedo, normal and ORM sampling remain. This skip
applies to both specialization modes, and to secondary hits outside GI.
POM-on marching retains its existing path.

Ray budgets, diffuse/reflection/transmission resolutions, source pixels,
random streams, estimator, instance selection and denoising are unchanged.
No lower-quality sampling mode was introduced.

## Protocol and artifacts

All captures use native MSVC RelWithDebInfo, RTX 4090 / driver 610.74,
1920×1080 visible window, hidden UI, IMMEDIATE presentation, frame limit 0,
StreamMountain's default camera, `LIGHTING_DETAIL=1`, POM off and a 20 s
sample after the static-geometry stability gate and stated warmup.
`PAGED_TERRAIN` is left at its default zero: these are the static/source path,
not the opt-in paged-terrain captures from task 8. GPU use is checked before
launch (<2,048 MiB) and logged every five seconds. Each JSON verifies
`pom_enabled=false`; no validation errors appear in the capture logs. Builds,
C++ tests and editor captures ran serially.

The original early reference executable was preserved from `15753fbd` as
`C:/tmp/clear-ridge9-before45/editor-gi-split.exe`, then copied to the local
`editor-gi-reference.exe`. Its SHA-256 is
`056d83939f3eeb3510cc1a036633217de257e84d573c1a21d824b2cb0881741c`.
The late reference used the specialization-only intermediate build with
`GI_SPECIALIZE=0`, before the POM-off skip. It retains the original executed
shader path but includes the unused specialized stage inventory. This
intermediate source was not separately committed; its executable is preserved
as `C:/tmp/clear-ridge9-specialize-before300/editor-reference.exe`, SHA-256
`f1609d3c394333126e241446558f66b388ee552383370b10cd4493f0bd965a2f`.
The final editor built from `46cb2266` has SHA-256
`3fc5cb0c70c9422a1a9d2aed1e2c0c18564d867823c862326f50d6764dc182f0`.
The script's `git_sha.txt` records checkout HEAD, which does not identify a
preserved reference binary or intermediate uncommitted source; use these
binary identities when interpreting the reference captures.

```bash
# Original executed paths, before the final shader changes.
LIGHTING_DETAIL=1 EDITOR_NAME=editor-gi-reference.exe VARIANTS=pom_off RUNS=1 WARMUP=45 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge9-resume-before45
LIGHTING_DETAIL=1 GI_SPECIALIZE=0 VARIANTS=pom_off RUNS=1 WARMUP=300 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge9-specialize-before300

# Final binary, after both shader changes.
LIGHTING_DETAIL=1 GI_SPECIALIZE=1 VARIANTS=pom_off RUNS=1 WARMUP=45 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge9-final-after45
LIGHTING_DETAIL=1 GI_SPECIALIZE=1 EDITOR_NAME=editor-gi-final.exe VARIANTS=pom_off RUNS=1 WARMUP=300 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge9-epoch4-final-after300
```

The late reference command above was run before rebuilding the final editor;
reproduce it using `EDITOR_NAME=editor-gi-specialize-only.exe` from the preserved
intermediate binary. Final outputs contain JSON, trace, editor/driver logs,
binary checksum, `attribution.md`, `hitches.md` and `cpu_zones.md`. Raw data and
images remain under `C:/tmp` rather than being committed.

## Current before/after results

All GPU and interval times below are milliseconds. Hitch counts refer to
frame intervals, not GPU total time.

| Warmup / binary | Frames | GPU total median / p99 / max | Interval median / p99 / max | >100 ms | >1 s |
|---|---:|---|---|---:|---:|
| 45 s reference | 536 | 36.26 / 40.77 / 59.24 | 36.54 / 93.27 / 215.12 | 3 | 0 |
| 45 s final | 563 | 34.81 / 45.84 / 53.07 | 34.67 / 85.76 / 126.13 | 1 | 0 |
| 300 s reference | 327 | 61.11 / 66.05 / 83.25 | 61.23 / 70.34 / 130.28 | 2 | 0 |
| 300 s final | 326 | 59.82 / 79.43 / 95.57 | 59.80 / 141.01 / 197.45 | 4 | 0 |

| Warmup / binary | Diffuse median / p99 | Reflection/transmission median / p99 | Aggregate GI median / p99 | G-buffer median / p99 | VT p99 |
|---|---|---|---|---|---|
| 45 s reference | 12.02 / 13.39 | 0.53 / 0.58 | 12.54 / 13.96 | 19.04 / 20.33 | 2.30 |
| 45 s final | 11.39 / 11.70 | 0.51 / 0.53 | 11.91 / 12.22 | 18.35 / 20.22 | 3.65 |
| 300 s reference | 18.90 / 19.16 | 0.75 / 0.76 | 19.65 / 19.91 | 32.62 / 35.49 | 7.70 |
| 300 s final | 18.28 / 18.75 | 0.76 / 0.77 | 19.05 / 19.51 | 32.73 / 35.99 | 8.41 |

The early final diffuse median is 0.63 ms lower (−5.2%), aggregate GI
0.63 ms lower (−5.0%), and GPU total 1.45 ms lower (−4.0%). GPU p99 rises
from 40.77 to 45.84 ms while the maximum falls. Thus neither an across-the-board
tail improvement nor a fixed-workload causal estimate follows from this pair.
The early reference/final scans average 1,723/1,713 RT instances and 198/190
sector-LOD instances; static uploads in the sample are 5/8 and peak GPU memory
12,529/12,501 MiB. Streaming continues after the stability gate and GPU clocks
are not locked. Even the G-buffer changes despite no new raster changes here.
The late reference/final average 2,349/2,478 RT and 295/318 sector-LOD
instances; static uploads in the sample are 2/7 and peak GPU memory
15,201/15,436 MiB. The final late diffuse median is 0.62 ms lower (−3.3%),
aggregate GI 0.60 ms lower (−3.1%) and GPU total 1.29 ms lower (−2.1%).
GPU p99/max rise from 66.05/83.25 to 79.43/95.57 ms and intervals over
100 ms rise from 2/327 to 4/326. The larger final streamed population and
continuing uploads prevent a fixed-workload causal estimate. Neither sample
has an interval over 1 s; neither demonstrates elimination of all hitches.

A provisional early specialization-only capture (before the POM-off skip),
`C:/tmp/clear-ridge9-specialize-after45`, measured diffuse 11.02 ms, GI 11.52 ms,
GPU total 34.61 / 45.49 / 53.66 ms, 3/568 intervals >100 ms and zero >1 s.
Its G-buffer median was also lower, 18.17 ms, and RT/LOD scans averaged
1,709/186. The final early capture is slightly slower than this intermediate
run. These independent streamed runs do not isolate the POM-off skip's
individual savings or support adding separate percentage improvements.

## Task-1 comparison

The POM-off task-1 baseline is `3bafe049`; its three-run figures are in
[the baseline findings](streammountain-pom-off-baseline-2026-09-28.md).

| Metric | Task 1, 45 s | Task 1, 300 s | Final, 45 s | Final, 300 s |
|---|---|---|---|---|
| GPU total medians | 227.50–248.29 | 402.11–454.17 | 34.81 | 59.82 |
| GPU total p99 = max (task 1) | 394.12–594.33 | 433.52–811.37 | 45.84 / 53.07 | 79.43 / 95.57 |
| Interval median / p99 / max | 233.23 / 3807.05 / 3939.87 | 409.49 / 922.18 / 1061.75 | 34.67 / 85.76 / 126.13 | 59.80 / 141.01 / 197.45 |
| Intervals >100 ms | 161/172 | 138/141 | 1/563 | 4/326 |
| Intervals >1 s | 6/172 | 1/141 | 0/563 | 0/326 |
| Aggregate GI medians | 54.28–55.76 | 80.34–102.22 | 11.91 | 19.05 |

This historical comparison includes changes to geometry, raster and VT work,
different streamed populations and different profiling boundaries. It is
context for the epic, not the speedup attributable to this GI task. Even the
final early total median remains above a 16.7 ms frame budget.

## Quality and verification

Native MSVC editor, `vulkan_smoke_tests` and `shader_source_tests` builds pass.
Final native Vulkan modes `rt`, `rt-transmission`, `rt-local-direct` with the
optional primary stage, and `vt-composed-parallax` all exit 0, report
`ALL PASS` and `validation errors: 0`. The RT and transmission modes compare
fixed-frame raw signals against the alternate shader selection to 1e-6;
detail timers are off so the reference also exercises the original combined
equal-extent dispatch. RT checks retain the failed-presentation retry/history,
secondary filtering, mirror and baked-AO-zero oracles. Transmission retains
smooth/rough, absorption, GI-off fallback and alpha-tested refraction checks.

The composed-parallax fixture explicitly sets POM off and checks that a
secondary proxy hit retains VT color/normal sampling, has no invalid address,
and stays within 0.00006 m of the raster proxy. Existing coarse, scaled,
connected and curved POM-on checks also pass without changing thresholds.
The earlier profiling checkpoint's detail-on/off timestamp checks passed.

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo shader_source_tests

env WSLENV=MATTER_GPU_LIGHTING_DETAIL_TIMERS:MATTER_VK_SMOKE_MODE \
  MATTER_GPU_LIGHTING_DETAIL_TIMERS=0 MATTER_VK_SMOKE_MODE=rt \
  timeout --kill-after=10 480 \
  MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
# Repeat serially with MATTER_VK_SMOKE_MODE=rt-transmission.
env WSLENV=MATTER_GPU_LIGHTING_DETAIL_TIMERS:MATTER_VK_SMOKE_MODE:MATTER_RT_PRIMARY_ONLY \
  MATTER_GPU_LIGHTING_DETAIL_TIMERS=0 MATTER_VK_SMOKE_MODE=rt-local-direct MATTER_RT_PRIMARY_ONLY=1 \
  timeout --kill-after=10 480 \
  MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
env WSLENV=MATTER_GPU_LIGHTING_DETAIL_TIMERS:MATTER_VK_SMOKE_MODE \
  MATTER_GPU_LIGHTING_DETAIL_TIMERS=1 MATTER_VK_SMOKE_MODE=vt-composed-parallax \
  timeout --kill-after=10 480 \
  MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

Reference and final default-camera screenshots were requested 180 s into
the 300 s warmup, outside the timed samples. Both `warmup.png.done` markers
contain `captured`. Images are in `C:/tmp/clear-ridge9-specialize-before300`
and `C:/tmp/clear-ridge9-epoch4-final-after300`. Inspection finds no apparent
lighting, material or detail regression in the shared foreground rocks/trees;
the final has more far-left terrain loaded. The dark blue landscape and
pre-existing rectangular horizon/sky artifacts remain. This accepts the
tested view, not every camera or the existing scene defects.

`visual-comparison.json` records image differences on 8-bit RGB: the lower
half (rows 540–1079) has mean absolute channel error 0.475/255, p99 maximum
channel error 10 and 1.13% of pixels exceeding 8 levels. Whole-image mean
error is 3.421/255, with differences concentrated at the changed horizon.
These streamed screenshots are not a pixel-equality oracle; the native
fixed-frame raw-signal checks above provide controlled estimator evidence.

`shader_source_tests` exits 9 at its unchanged line-88 source-string assertion
for `if (instance.water_pad0 != 0u) return;`. This is the recorded failure in
[G-buffer split findings](streammountain-gbuffer-split-2026-09-28.md), lines
184–187; current `cull.comp` combines water and cluster guards. Neither file
was changed to weaken that check. `bash -n tools/streammountain_attribution.sh`
and `git diff --check` pass; invalid `GI_SPECIALIZE` is rejected with exit 2.
Build/test logs are `/tmp/clear-ridge9-final-*.log`.

## Prior resource blocker and completed late acceptance

The initial task attempt could not pass the idle-GPU gate while Qwen held
16.7–21.7 GiB. It closed with an instrumentation checkpoint and no cost claim.
The resumed worker obtained idle windows for all measured runs (about
903–910 MiB and 0% GPU utilization before launch). The final late capture
then waited about 30 minutes at the same gate, September 30 approximately
00:03–00:33 PDT. Qwen occupied about 21.6 GiB and repeatedly renewed its lease;
no Windows editor was running. Supervisor and dashboard notifications were
queued through aq. The worker stopped only its waiting capture and image
monitor; other processes were left running. That attempt produced no final-late
performance JSON, trace, screenshot or elapsed rendering time. Its driver wait log is
`/tmp/clear-ridge9-final-after300-driver.log`; the output directory retains
launch recipe/provenance, not a measured result.

Claim epoch 3 retried the remaining capture on September 30, approximately
00:46–00:52 PDT, after verifying the published checkpoint and all four retained
native Vulkan pass logs. The restored final binary has the SHA-256 above and
was launched through `EDITOR_NAME=editor-gi-final.exe` into
`C:/tmp/clear-ridge9-epoch3-final-after300`. The idle gate again recorded
21,589 MiB throughout, with 79–92% GPU utilization at spot checks and an active
`qwen3.8:27b` worker. No editor log, perf JSON, trace or screenshot was created.
The worker requested a coordinated GPU window from the project supervisor and
dashboard through aq; no response arrived before stopping only its pending
capture and screenshot monitor. This repeat provides no new performance or
quality result. Its wait log is
`/tmp/clear-ridge9-epoch3-after300-driver.log`. An exclusive window coordinated
across projects was required for the final acceptance retry.

Claim epoch 4 completed the final late capture on September 30, approximately
02:03–02:17 PDT, after checking the preserved final executable's checksum.
The GPU was at 909 MiB / 0% utilization before launch. The supervisor secured
a four-hour window by disabling competing OpenCode/Ollama launches and
preserving the idle worker's work. The driver and screenshot monitor both
exited 0; the perf JSON confirms POM off, 1920×1080, effective RT and zero
validation errors. Launch-to-exit elapsed time was 839.40 s, versus 639.03 s
for the late reference; these totals include scene preparation and teardown,
not just rendering. Artifacts are `C:/tmp/clear-ridge9-epoch4-final-after300`,
with driver/monitor logs at `/tmp/clear-ridge9-epoch4-after300-driver.log` and
`/tmp/clear-ridge9-epoch4-monitor.log`.

No implementation changed in epoch 4 and no native suite was rerun. The four
retained final native pass logs were inspected; their checks apply to the
unchanged source and verified executable. The task's split, cost reduction,
before/after reporting, tested-view quality check and queue update are complete.
Added raygen stages increase pipeline inventory; cold pipeline compilation
was not isolated or benchmarked. The measured GI improvement does not eliminate
all hitches or reach the epic's 16.7 ms whole-frame target.

The final binary is also preserved outside the recyclable slot as
`C:/tmp/clear-ridge9-final-after45/editor-final.exe`; the early reference is
`C:/tmp/clear-ridge9-resume-before45/editor-reference.exe`, and the late
reference remains in its directory above. All three checksums are recorded
in `C:/tmp/clear-ridge9-final-after45/checkpoint-binaries.sha256`.

To reproduce with an idle GPU window, restore the final executable to
`MatterEditor/build/windows-msvc/editor-gi-final.exe` (or rebuild `46cb2266`),
then run the final 300 s command into a fresh directory with
`EDITOR_NAME=editor-gi-final.exe`. At 180 s after the log reports the 300 s
warmup has started, append `stats gi-visual` and
`shot_now C:/tmp/<fresh-directory>/warmup.png` to its command file and verify
`warmup.png.done` contains `captured`. Compare against the retained late
reference, report GPU median/p99/max, >100 ms/>1 s interval counts and
population variation, and inspect matching images. Repeat native tests only if
implementation changes or a new concern warrants it; the recorded final checks
already pass.
