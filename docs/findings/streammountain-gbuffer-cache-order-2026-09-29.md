# StreamMountain G-buffer vertex reuse — 2026-09-29

Task `clear-ridge.7`, queue row 1.12. POM stays disabled. The task-2
[split](streammountain-gbuffer-split-2026-09-28.md) identified geometry/raster
work as the dominant steady G-buffer cost. This task measures vertex reuse
and reorders triangles within each existing cluster/rung mesh at GPU prebuild.
It changes neither LOD selection nor the set of triangle corner triplets.

## Current baseline and workload

The implementation commit is `4a1697e2`; the starting commit is `2f328bd6`.
Captures began before the implementation commit was made, so their recorded
git SHA still names the starting commit; `editor_sha256.txt` identifies each
measured executable. It already contains the static-buffer and
VT-pacing work that followed task 1, so task 1 is historical context rather
than a matched before measurement. All captures use the native MSVC
RelWithDebInfo editor, RTX 4090/610.74, 1920x1080, default StreamMountain camera,
visible immediate presentation, a 20-second sample and verified POM-off state.
Raw JSON, traces and logs remain under `C:/tmp/clear-ridge-7-*`.

The unchanged-binary 300-second capture (`before-w300`) sampled 316 frames:
GPU median/p99/max **61.39/83.90/92.94 ms**, G-buffer median **32.78 ms**,
7 frame intervals over 100 ms and none over 1 s. There were 9 static uploads
in the sample and peak whole-GPU memory was 15,611 MiB. The viewer-object
instrumentation build overlapped part of warmup, not sampling. Editing the
capture script during this first run broke its summary footer; the completed
perf JSON and trace were retained, and all three summaries were regenerated
from those artifacts. Subsequent captures ran with the script unchanged.

`MATTER_GBUFFER_WORKLOAD=1` adds optional, fenced Vulkan pipeline-statistics
queries around the G-buffer. The capture script passes it with
`GBUFFER_WORKLOAD=1`. Queries count submitted primitives, vertex invocations,
primitives after clipping and fragment invocations; helper fragments mean the
last count is not an exact visible-surface overdraw measure. Pools are absent
by default. Unsupported devices warn and continue.

The early matched workload captures use 45 seconds of warmup:

| Sample median | Before (`workload-before`) | Cache order (`cache-after-w45`) |
|---|---:|---:|
| Submitted triangles | 11,548,963 | 11,548,963 |
| Primitives after clipping | 9,739,931 | 9,739,931 |
| Vertex shader invocations | 19,640,358 | 13,718,366 |
| Fragment shader invocations | 3,955,749 | 3,959,266.5 |
| G-buffer median / p99, ms | 18.75 / 21.35 | 18.11 / 20.49 |
| GPU total median / p99 / max, ms | 51.22 / 103.62 / 105.85 | 36.52 / 40.53 / 48.54 |
| Sampled frames | 330 | 542 |
| Frame intervals >100 ms / >1 s | 23 / 0 | 1 / 0 |
| Static uploads in sample | 5 | 2 |
| Peak whole-GPU memory, MiB | 12,699 | 12,573 |

Vertex invocations fall **30.2%** at identical submitted/clipped triangle
counts; fragments differ by 0.09%. G-buffer median falls only **3.4%**.
Do not attribute the much larger total-frame reduction to cache ordering:
VT median independently falls from 14.34 to 1.18 ms between these launches.
The stream is still evolving, and the small G-buffer timing delta needs that
run-variation qualification even though the vertex-work reduction is direct.

## Final 300-second capture and historical comparison

The final uninstrumented `cache-after-w300` capture completed normally with
POM off and zero validation errors. Its 325 frames give:

| Metric | Current unchanged baseline | Cache order |
|---|---:|---:|
| GPU median / p99 / max, ms | 61.39 / 83.90 / 92.94 | 60.68 / 74.79 / 89.84 |
| G-buffer median / p99, ms | 32.78 / 35.67 | 30.77 / 33.40 |
| Interval median / p99 / max, ms | 61.79 / 129.23 / 198.88 | 60.55 / 124.80 / 162.54 |
| Intervals >100 ms / >1 s | 7/316 / 0/316 | 5/325 / 0/325 |
| Static uploads | 9 | 6 |
| Peak whole-GPU memory, MiB | 15,611 | 15,451 |
| LOD instances scanned, mean / p95 | 316 / 320 | 310 / 315 |

The late G-buffer median is 6.1% lower, but the approximately 2% smaller
instance population and differing uploads prevent attributing all of it to
ordering. The early identical-triangle query comparison is the stronger
workload evidence; these single late runs establish modest timing movement,
not a statistically isolated speedup.

For the explicitly requested task-1 comparison, the original three 45-second
runs had GPU medians 227.5–248.3 ms and p99/max 394.1–594.3 ms, with pooled
intervals >100 ms **161/172** and >1 s **6/172**. The new early run is
**36.52/40.53/48.54 ms**, with **1/542** and **0/542** intervals respectively.
The original three 300-second runs had GPU medians **402.1–454.2 ms** and
p99/max **433.5–811.4 ms**, with pooled intervals >100 ms **138/141** and >1 s
**1/141**. The new late run is **60.68/74.79/89.84 ms**, with **5/325** and
**0/325** intervals respectively. These historical gains include intervening
static-buffer/VT changes and evolving residency, not just this task's ordering.
See the [task-1 attribution](streammountain-frame-attribution-2026-09-27.md)
for the original run populations and limitations.

## Change and invariants

`render/vertex_cache_order.h` implements a deterministic, linear adjacency-fan
traversal based on section 3 of [Sander, Nehab and Barczak's triangle-locality
algorithm](https://gfx.cs.princeton.edu/gfx/pubs/Sander_2007_%3ETR/tipsy.pdf).
It accepts an order only if a 32-entry FIFO simulation predicts fewer misses.
Already-local meshes, tiny meshes and invalid inputs retain their order.
`build_vulkan_part` applies it to each loaded mesh before rebasing that mesh's
indices into the GPU part. Streamed parts do this during worker prebuild.

Every triangle retains its three indices in the same order, including its
winding and provoking vertex. Vertex attributes, draw spans, cluster bounds,
LOD thresholds and CPU bake/chart meshes are unchanged. Ordinary RT BLAS
construction and closest-hit decoding both consume the same GPU index buffer.
The independently cached paged-geometry adapter is not changed, so its BLAS
cache identities do not need invalidation. This is triangle ordering, not
triangle removal; coincident surfaces still follow the renderer's depth-tie
semantics rather than an order-independent transparency guarantee.

An earlier experiment replaced 15 per-vertex instance-constant transform
components with a flat transform index. Its G-buffer median was
18.73 ms versus 18.75 ms before at identical triangle counts: no resolved
benefit. It was fully reverted. Its raw capture is `workload-after`; it is
**not** the shipped cache-ordering implementation.

## Reproduction

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
GBUFFER_WORKLOAD=1 VARIANTS=pom_off WARMUP=45 \
  tools/streammountain_attribution.sh C:/tmp/<workload-output>
VARIANTS=pom_off WARMUP=300 \
  tools/streammountain_attribution.sh C:/tmp/<late-output>
```

For workload medians, select `[gbuffer-workload]` log rows between
`perf: sampling` and `perf: wrote`. The logged frame serial names the readback
frame, so queries refer to its previously completed frame-slot use.
The reference instrumented executable is retained locally as
`MatterEditor/build/windows-msvc/editor-cr7-workload-before.exe`; select it
with the script's `EDITOR_NAME` override.

## Verification

The canonical MSVC `vertex_cache_order_tests` passes. It checks deterministic
output and exact triangle multisets (including corner order) on a shuffled
grid, 100 randomized inputs with duplicate/degenerate triangles, disconnected
triangle soup, unused vertices and invalid inputs. The grid's modeled misses
fall from 24,413 to 4,599. Both completed early perf captures report zero
Vulkan validation errors.

MSVC `vulkan_smoke_tests` built successfully. The cull mode ran with
`MATTER_GBUFFER_WORKLOAD=1` to exercise query-pool frame reuse/recreation and
passed with zero validation errors. RT mode also passed with zero validation
errors. Tests ran serially, separately from GPU captures.

The optional `vt-normal-frame` mode **failed 32 assertions** (eight each for
applied VT, raster oracle, RT oracle and raster/RT parity), with zero validation
errors. This is an unresolved separate finding, not a passing check. Its
hand-built fixture bypasses `build_vulkan_part`, where ordering is applied.
The current `raster.vert`, `gbuffer.frag`, `vt_composite.comp` and
`rt_surface.rchit` SPIR-V byte sequences all occur identically in the saved
unchanged reference editor. The fixture only renders 12 frames per rotation;
insufficient settling after the earlier VT slicing change is a hypothesis,
not a proven diagnosis. No assertion or tolerance was weakened.

Exact focused commands (logs are `/tmp/clear-ridge-7-*.log` in WSL):

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo vertex_cache_order_tests
./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vertex_cache_order_tests.exe
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
WSLENV=MATTER_VK_SMOKE_MODE:MATTER_GBUFFER_WORKLOAD:TMP:TEMP \
  MATTER_VK_SMOKE_MODE=cull MATTER_GBUFFER_WORKLOAD=1 TMP=C:/tmp TEMP=C:/tmp \
  ./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
# Run rt and vt-normal-frame separately; the latter failed as recorded above.
WSLENV=MATTER_VK_SMOKE_MODE:TMP:TEMP MATTER_VK_SMOKE_MODE=rt \
  TMP=C:/tmp TEMP=C:/tmp \
  ./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

## Next fix

Reduce the remaining submitted microtriangle workload with meshlet-level
frustum/backface-cone culling, preserving conservative coverage and the existing
LOD policy. First attribute triangles to parts/rungs at the default camera.
Cache ordering removes redundant vertex work but leaves 11.55 million triangles
and primitive/raster work in this early view; the much smaller timing response
than vertex-count response shows why this alone is not a high-framerate cure.
The VT-normal fixture also needs a separate readiness investigation.
