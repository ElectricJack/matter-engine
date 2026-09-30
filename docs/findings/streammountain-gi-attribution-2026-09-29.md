# StreamMountain GI attribution availability — 2026-09-29

`clear-ridge.9`, queue row 1.14. The cost reduction is unfinished: the
required performance capture could not acquire an idle GPU. This checkpoint
provides instrumentation, not a measured optimization.

## Why the child zones had no samples

`MATTER_GPU_LIGHTING_DETAIL_TIMERS` defaults to zero. Even when enabled,
the former GI recorder combined diffuse and reflection/transmission at equal
resolutions. StreamMountain uses the default full-resolution setting for both
signals, so that recorder never wrote either child query pair.

Detailed profiling now splits equal-size dispatches using the existing
diffuse-only and reflection-only ownership masks. It retains both resolutions,
source pixels, independent random streams, ray budgets, denoising and output
images. Default rendering still uses its original combined dispatch. The
existing `rt_gi` aggregate remains the parent of both child intervals; do not
sum the children with the parent.

`tools/streammountain_attribution.sh` exposes this as `LIGHTING_DETAIL=1` and
passes the timer setting through `WSLENV`. Its usual POM-off check remains.
Profiling adds dispatch boundaries and timestamp barriers, so compare captures
with the same profiling setting when attributing a change.

## Measurement blocker and historical comparison

The unmodified MSVC editor at `ff27365d` was built and preserved as
`MatterEditor/build/windows-msvc/editor-gi-before.exe`. The attempted command
was:

```bash
EDITOR_NAME=editor-gi-before.exe VARIANTS=pom_off WARMUP=45 RUNS=1 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge9-before45
```

The script remained at its GPU idle check (requires less than 2,048 MiB in
use), before launching the editor. `nvidia-smi` showed 16,733 MiB initially,
then about 21,687 MiB before the native smoke test. `ollama ps` identified
`qwen3.8:27b`, 17 GB, 100% GPU residency, 65,536-token context. No other
Windows editor was running at the initial check. The native smoke test later
added memory use, reaching about 23,800 MiB; those readings are not idle-capture
evidence. The other process was left running. Supervisor and dashboard were
notified through aq.
The waiting capture was stopped before task close. Reference and profiling
executables were also preserved outside the recyclable slot under
`C:/tmp/clear-ridge9-before45/editor-gi-{before,split}.exe`, with SHA-256
checksums in `checkpoint-binaries.sha256`.

Consequently no before or after performance JSON exists for this task, and
no new GPU median/p99/max, hitch counts, dominant GI signal, or visible
comparison can be reported. The task-1 baseline remains:

| Metric | 45 s warmup, three runs | 300 s warmup, three runs | This checkpoint |
|---|---|---|---|
| GPU total median | 227.5–248.3 ms | 402.1–454.2 ms | unavailable |
| GPU total p99 = max | 394.1–594.3 ms | 433.5–811.4 ms | unavailable |
| Frame intervals >100 ms | 161/172 | 138/141 | unavailable |
| Frame intervals >1 s | 6/172 | 1/141 | unavailable |

See `streammountain-pom-off-baseline-2026-09-28.md` for that historical
protocol. Intervening changes already reduced other frame costs; this task
cannot claim those improvements.

## Verification

Native MSVC RelWithDebInfo editor and Vulkan smoke builds succeeded.
The initial detailed-timer `rt` smoke run passed with `validation errors: 0`
and `ALL PASS`. It covered GI history/retry determinism, secondary diffuse
and reflection filtering, mirrors and baked-AO-zero diffuse suppression.
The final detailed-timer gate also passed with the regression checks for both
child timestamp pairs at equal extents and unavailable child queries when GI
is disabled. Its first 240 s attempt timed out under shared-GPU pressure
(exit 124, no assertion or validation failure reported); the retry with a
480 s limit exited 0. The initial gate did not yet contain those assertions.
The same final `rt` gate with detailed timers disabled also exited 0, with
`validation errors: 0` and `ALL PASS`, checking that default child queries
remain unavailable.
The final `rt-transmission` gate with detailed timers enabled likewise
exited 0 with `validation errors: 0` and `ALL PASS`, covering smooth/rough
transmission, absorption, sun penumbra, GI-off fallback and alpha-tested
refraction. These small fixtures establish correctness, not StreamMountain
performance or scene visual acceptance.

```bash
env WSLENV=MATTER_GPU_LIGHTING_DETAIL_TIMERS:MATTER_VK_SMOKE_MODE \
  MATTER_GPU_LIGHTING_DETAIL_TIMERS=1 MATTER_VK_SMOKE_MODE=rt \
  timeout --kill-after=10 480 \
  MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

The default RT check uses the same command with
`MATTER_GPU_LIGHTING_DETAIL_TIMERS=0`; the transmission check uses
`MATTER_GPU_LIGHTING_DETAIL_TIMERS=1 MATTER_VK_SMOKE_MODE=rt-transmission`.

`shader_source_tests` built but exited 9 at its unchanged line-88 source-string
assertion for `if (instance.water_pad0 != 0u) return;`. This is the recorded
known failure in `streammountain-gbuffer-split-2026-09-28.md:184–187`; current
`cull.comp` combines water and cluster guards. Neither file was changed to
weaken that check. `bash -n tools/streammountain_attribution.sh` passed.

Raw build/test logs are `/tmp/clear-ridge9-*.log`; the waiting capture log
is `/tmp/clear-ridge9-before45-driver.log`. Native C++ suites run serially.

## Resume

With an exclusive GPU window, rebuild the editor and run
`LIGHTING_DETAIL=1 VARIANTS=pom_off RUNS=1 WARMUP=45` and then `WARMUP=300`
through the attribution script into fresh directories. Identify the dominant
child zone before changing ray budgets, resolutions, instance selection or
shader execution. Retain an instrumented reference executable for matched
before/after captures, run the native RT/reflection/transmission gates
serially, inspect fresh matching images, and update row 1.14 with actual
GPU median/p99/max and >100 ms/>1 s counts. The required cost fix and visual
acceptance remain open.
