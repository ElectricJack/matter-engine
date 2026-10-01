# StreamMountain POM-off G-buffer split — 2026-09-28

Task `clear-ridge.2`, queue row 1.12. The comparison baseline is
[`streammountain-pom-off-baseline-2026-09-28.md`](streammountain-pom-off-baseline-2026-09-28.md)
from `clear-ridge.1`. Raw perf JSON, logs, traces and GPU-use logs are kept in
`C:/tmp/clear-ridge-2-profile/`, `C:/tmp/clear-ridge-2-w45/`,
`C:/tmp/clear-ridge-2-w300/`, `C:/tmp/clear-ridge-2-cutout-w45/` and
`C:/tmp/clear-ridge-2-cutout-w300/`; they are not committed.

## Method

`gbuffer` is a single raster pass with one fragment shader, so Vulkan GPU
timestamps cannot be inserted between its shader operations. This diagnosis
uses four fragment specializations selected by `MATTER_GBUFFER_PROFILE_MODE`:

| Variant | Work retained inside the same depth/MRT pass |
|---|---|
| `geometry` | Raster/vertex work, seven attachment writes, depth and basic VT feedback address/request. Material shading is bypassed. Impostor alpha cutout is also bypassed, so its timing can overstate the raster floor where impostors cover pixels. |
| `geometry_cutout` | The same early return for ordinary fragments, while impostors run the full alpha cutout and depth path. This checks how much the `geometry` probe changes coverage. |
| `no_vt` | The above plus base material, impostor and water shading. Ground tileset and VT material sampling are bypassed; basic VT feedback remains active. |
| `pom_off` | The shipped full shader, with POM disabled and all surface sampling enabled. |

`no_vt − geometry` probes non-VT shading, and `pom_off − no_vt` probes
ground tileset plus VT material sampling. They are **differential probes**,
not additive timestamp sub-zones: specializations change register pressure and
fragment coverage, and StreamMountain continues streaming during each 45/300 s
warmup. The diagnostic feedback does not recreate every near-detail or POM
neighbor request the full shader can emit; VT page-fill timing may therefore
diverge. Compare the scene counters and run spread before assigning small
differences. `tools/streammountain_attribution.sh` checks `pom_enabled=false`
and the recorded `gbuffer_profile_mode` for every run. Each result uses a 20 s
sample, 1920×1080, visible immediate presentation and the default camera.

## Static shader profile

The RTX 4090 (driver 610.74) exposes
`VK_KHR_pipeline_executable_properties`. With `PIPELINE_STATS=1`, the script
records the driver's compiled executable statistics in each editor log.
The full POM-off shader's fragment executable uses **168 32-bit registers per
thread**, and `vt_composite.comp` uses **168** with a 32-thread subgroup and
16,640 bytes of shared memory per 8×8 workgroup. The diagnostic fragment
specializations use **60** registers for `geometry`, **168** for
`geometry_cutout` and **96** for `no_vt`. The cutout variant retains the full
impostor branch, so its compiled resource pressure does not isolate geometry.
On Ada's 64K-register, 48-warp SM, 168
registers/thread permit at most `floor(65536 / (168 × 32)) = 12` warps, a
**25% theoretical occupancy ceiling** before other limits. This is an
architectural ceiling, not a measured active-warp counter. [RTX 4090 compute
capability](https://developer.nvidia.com/cuda/gpus); [NVIDIA Ada occupancy
limits](https://docs.nvidia.com/cuda/ada-tuning-guide/).

The NVIDIA driver reports `Local Memory Size` as 68,719,486,112 for the full
fragment executable, 68,719,478,160 for `no_vt`, 68,719,482,480 for
`vt_composite.comp`, and 68,719,476,736 even for the light geometry fragment
executable. Each exceeds
the GPU's physical memory. The low-word differences from the geometry value
are 9,376, 1,424, and 5,744 bytes respectively, but the driver does not provide a
separate spill count or define that 64 GiB offset. These figures establish
local-memory pressure as a lead to investigate, **not** measured spill traffic
or spill bytes. The available statistics cannot measure achieved occupancy.

## Timed results

The first post-instrumentation `pom_off` capture at 45 s warmup (`C:/tmp/clear-ridge-2-profile/`)
sampled 48 frames: GPU total median/p99/max **244.17/667.02/667.02 ms**,
`gbuffer` median **166.67 ms**, 48 frames over 100 ms and 2 over 1 s.
Its medians are within task 1's 45 s ranges (GPU total 227.5–248.3 ms,
`gbuffer` 159.4–170.2 ms), and two over-1 s hitches match task 1's 1–3 per
run. Its GPU p99/max is 72.69 ms above task 1's highest 45 s run. The capture
completed, but the script footer hit a shell parse error because the file was
being edited while it ran; `attribution.md` and `hitches.md` were generated
directly from the completed perf JSON with `tools/frame_attribution.py`.

### 45 s matched variants (`C:/tmp/clear-ridge-2-w45/`)

| variant | frames | GPU total median / p99 / max ms | `gbuffer` median / p99 ms | >100 ms | >1 s | static vertex uploads | mean LOD scanned |
|---|---:|---|---|---:|---:|---:|---:|
| `geometry` | 63 | 213.98 / 535.04 / 535.04 | 145.65 / 162.13 | 60 | 2 | 7 | 156 |
| `no_vt` | 64 | 223.85 / 534.73 / 534.73 | 145.81 / 189.52 | 63 | 2 | 7 | 162 |
| `pom_off` | 56 | 232.65 / 621.04 / 621.04 | 154.50 / 186.20 | 53 | 2 | 6 | 169 |

All three runs report `pom_enabled=false`, zero Vulkan validation errors and
1920×1080 raster output. The geometry diagnostic costs about 145 ms even
with the fragment shader cut from 168 to 60 registers. `no_vt − geometry` is
0.16 ms at the median, below the run spread. `pom_off − no_vt` is 8.69 ms,
also below task 1's 10.8 ms `gbuffer` spread. The mean LOD-scanned instances
rise 156 → 169 across these sequential runs, so those deltas are not a
same-scene attribution. What is resolved is the scale: the simple geometry
shader still takes roughly the full G-buffer time. The shipped `pom_off` GPU
median and hitch count remain within the task-1 45 s range; its 621.04 ms
GPU p99/max exceeds task 1's highest 594.33 ms run by 26.71 ms. The
diagnostics are not shipping performance improvements.

The independent `geometry_cutout` check (`C:/tmp/clear-ridge-2-cutout-w45/`)
sampled 100 frames after 45 s: GPU total median/p99/max
**197.01/217.03/357.88 ms**, `gbuffer` median/p99 **137.83/149.96 ms**,
97/100 frames over 100 ms and none over 1 s. Its mean LOD-scanned count in
the sample was **153**, versus 156 for the original `geometry` run. Preserving
impostor discard does not raise the raster floor; it lowers this sample's
G-buffer median by 7.82 ms. Because it was a separate launch, this difference
also includes scene and streaming variation. The cutout variant's 168-register
executable means it should not be read as a pure low-register geometry shader.

### 300 s matched variants (`C:/tmp/clear-ridge-2-w300/`)

| variant | frames | GPU total median / p99 / max ms | `gbuffer` median / p99 ms | >100 ms | >1 s | static vertex uploads | mean LOD scanned |
|---|---:|---|---|---:|---:|---:|---:|
| `geometry` | 43 | 434.17 / 798.57 / 798.57 | 312.82 / 351.00 | 41 | 1 | 5 | 263 |
| `no_vt` | 49 | 405.43 / 427.68 / 427.68 | 306.19 / 326.75 | 48 | 0 | 3 | 256 |
| `pom_off` | 47 | 409.59 / 648.05 / 648.05 | 297.32 / 328.30 | 45 | 0 | 3 | 254 |

All three runs again have POM off, zero validation errors and the same camera,
resolution and presentation. The `geometry` diagnostic takes **313 ms** at
about 263 LOD-scanned instances. This overlaps task 1's full-shader G-buffer
median range of **307–330 ms** at 248–268 instances. The diagnostic-to-full
median differences are negative (`no_vt − geometry = −6.64 ms`,
`pom_off − no_vt = −8.87 ms`); the scene gets less dense in the same order
(263 → 256 → 254 scanned instances). A negative "shading cost" shows why
these cross-run deltas cannot be used as literal sub-zone times. Task 1's
full-shader 300 s G-buffer run spread alone was 23 ms.

VT residency is another mismatch: the `vt` pre-pass p99 is 362.53 ms for
`geometry`, 0 for `no_vt`, and 290.59 ms for `pom_off`. The diagnostic
specializations preserve basic visible feedback but do not reproduce the full
shader's complete demand. Their frame totals and hitch counts describe each
diagnostic world, not a faster shipping renderer. The shipped `pom_off` GPU
median 409.59 ms, p99/max 648.05 ms and 0/47 over-1 s hitches are within
task 1's 300 s run ranges (402.1–454.2 ms median, 433.5–811.4 ms p99/max,
0–1 over-1 s). Its 45/47 frames over 100 ms match the baseline's
near-universal over-100 ms cadence.

The separate `geometry_cutout` check after 300 s
(`C:/tmp/clear-ridge-2-cutout-w300/`) sampled 45 frames: GPU total
median/p99/max **442.05/598.53/598.53 ms**, `gbuffer` median/p99
**329.49/346.50 ms**, 45/45 frames over 100 ms and none over 1 s. It had
**269** mean LOD-scanned instances and no static vertex upload in the sample.
That is slightly denser than the matched `geometry` run's 263 and full run's
254; its 329.49 ms G-buffer median is within task 1's full-shader 307–330 ms
range. The cutout check therefore leaves the same order of raster cost while
restoring impostor coverage; the 16.67 ms rise over `geometry` is not an
isolated cutout cost because scene density and launch state differ.

## Decision and fix order

**Geometry/raster work dominates the steady POM-off G-buffer.** Removing
nearly all material work and cutting the fragment executable from 168 to 60
registers still leaves 146 ms at 45 s and 313 ms at 300 s. A separate
cutout-preserving check leaves 138 ms and 329 ms at similar scene density.
The diagnostic
includes vertex processing, primitive setup, depth/attachment writes and basic
feedback; this experiment does not split those geometry-side costs further.
Non-VT shading and VT/tileset sampling cannot be assigned positive independent
median costs above run and scene variation. Their large compiled resource
footprints remain real but do not explain the 146–302 ms steady zone by
themselves.

Fix order for the clear-ridge goal:

1. **Bound static-buffer rewrites (queue 1.7)** to remove the 2–4 s render
   stalls seen in task 1's 45 s windows. This addresses the >1 s hitch goal;
   it will not by itself remove a 300 ms steady G-buffer.
2. **Attack drawn geometry before shader micro-optimizations.** Resolve the
   root-bank paging failure (1.13), then measure vertex/primitive invocations,
   visible triangle count and overdraw at the default camera before choosing
   a raster workload reduction. The persistent cut (1.5) is gated by 1.13 and
   is a CPU/runtime change; `geometry.*` is absent from every default trace,
   so 1.5 alone is not a demonstrated cure for this G-buffer.
3. **Keep VT tape work (1.9) for page-fill spikes.** `vt_composite.comp` has
   the same 168-register compiled pressure and the `vt` pre-pass still has
   200–360 ms p99 bursts, but it is not a steady 300 ms charge. Split page
   fills, uploads and AO within `vt` before attributing those bursts to tape
   spills or changing the tape interpreter.

The driver exposed no explicit spill-count or achieved-occupancy statistic.
An NVIDIA graphics shader profile with local load/store counters would be
needed to turn the raw `Local Memory Size` values into measured spill traffic.

## Verification

MSVC RelWithDebInfo `matter_editor`, `vulkan_smoke_tests` and
`vt_compositor_tests` built successfully. Sequential CTest runs passed
`vulkan_smoke_tests`, `vt_compositor_tests`, `smoke_vt_feedback_pair` and
`perf_gpu_stats_tests`.
`shader_source_tests` fails on a pre-existing source assertion expecting
`if (instance.water_pad0 != 0u) return;` in `cull.comp`; the current shader
has a combined water/cluster guard. Other stale water and lighting source
assertions surfaced when that test was temporarily advanced; its file was
restored with no changes in this task. `tools/tests/test_frame_attribution.py`
passed 11 tests; the capture script passes `bash -n`.
