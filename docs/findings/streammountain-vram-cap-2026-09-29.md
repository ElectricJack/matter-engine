# StreamMountain paged VRAM cap — 2026-09-29

Task `clear-ridge.4`, the VRAM-budget first part of queue row 1.6. The two
captures below used `tools/streammountain_attribution.sh`, `PAGED_TERRAIN=1`,
`VARIANTS=pom_off`, one run each, 1920×1080, immediate presentation, and a
20 s sample after 300 s warmup. Both perf JSON files confirm POM was disabled.
Raw logs, traces, perf JSON, and `nvidia-smi` samples are in
`C:/tmp/clear-ridge-4-{before,after}-paged-300/`.

## Budgets and observations

The before editor was built from `635d38b4` (SHA-256 `6d021404…e4`). It used
the former 4,096 MiB VT budget, which allocated 25,600 slots and 4,064 MiB of
physical images, and the paged tool's former 1,024 MiB geometry cap. The after
editor was built from the changed source later committed as `0cb883ee`
(SHA-256 `d8912b11…0f5`): the
default VT budget is 2,048 MiB (12,800 slots, 2,032 MiB allocated), and the
terrain geometry page reservation is capped at 3,072 MiB by default. The
capture and cache-audit tools use those same defaults. Explicit
`MATTER_VT_POOL_MB`, `MATTER_GEOMETRY_GPU_MB`, `PAGED_VT_MB`, and `PAGED_GPU_MB`
overrides remain available.

| 300 s warmup, POM off | Before | After |
|---|---:|---:|
| Sampled frames | 73 | 151 |
| GPU total median / p99 / max, ms | 162.74 / 206.08 / 206.08 | 115.01 / 128.01 / 128.32 |
| Frame interval median / p99 / max, ms | 259.76 / 451.01 / 451.01 | 132.19 / 231.49 / 240.95 |
| Frame intervals over 100 ms | 73 / 73 | 128 / 151 |
| Frame intervals over 1 s | 0 | 0 |
| Static uploads during 20 s sample | 2 | 146 |
| Peak whole-GPU use (`nvidia-smi`), MiB | 12,456 | 10,259 |
| Peak geometry page reservation, MiB | 1,023.99 / 1,024 cap | 1,274.89 / 3,072 cap |
| Last global source fallbacks | 29 | 71 |
| Device-memory exhaustion / static-capacity overflow | 0 / 0 | 0 / 0 |

This is a memory-safety comparison, not a frame-time speedup claim. The
terrain cache was still filling and the sampled scenes differed: the CPU
trace counted 49,643 RT-scanned instances per frame before and 13,681 after.
The after
run's 146 static uploads and rising source fallbacks show it had not reached a
settled paged scene. It survived the 300 s window without OOM, but the
steady-state acceptance still needs cache preparation and a cache-only load.

A separate `terrain_cache_audit.py prepare` pass ran for its 5,400 s deadline
with the new budgets. The editor exited normally after its active cooks
finished (5,564 s elapsed), with no paging failure or Vulkan validation error.
It compiled and persisted 444 missing geometry assets and hit 236 cached
assets. At the deadline all 335 desired visible sectors were ready, the
geometry coverage counters were `(0, 0, 0)`, and VT had no queued pages or
rejected variants. Twelve geometry cooks remained in flight, so the audit
correctly marked this pass incomplete. A second, warm preparation pass was
stopped cleanly after 2,886 s to replace its short deadline with a longer
one. It recorded 528 cache hits, no new compilations, no coverage gaps or
failures, and 1,852 resident sectors with 12 cooks still in flight. Neither
pass qualifies as steady-state acceptance; both retained their durable
geometry cache entries for the longer continuation.

Task 1's shipped, non-paged POM-off baseline had GPU total medians of
402.1–454.2 ms and p99/max of 433.5–811.4 ms at 300 s; its pooled frame
interval counts were 138/141 over 100 ms and 1/141 over 1 s. The paged
captures above render a different mix of resident geometry and source
fallbacks, so the lower medians cannot be credited to this budget change.

## Per-consumer memory

The after editor logs a `vram` line every 120 frames when geometry profiling
is enabled. The peak reported values during the capture were:

| Consumer or ledger | MiB | Meaning |
|---|---:|---|
| Tracked device-local allocation | 6,914.28 | Process-wide Vulkan allocation tracker; includes the consumers below |
| VT physical pool | 2,032.03 | Five fixed physical page images, 12,800 slots |
| VT indirection capacity | 64.00 | Fixed GPU buffer |
| VT occlusion pages | 50.80 | Current GPU allocation |
| Geometry pages | 1,274.89 | Reservation ledger, bounded by 3,072 MiB; not raw `vkAllocateMemory` bytes |
| Static scene buffers | 800.00 host-visible, 0 device-local | Allocated cluster, vertex, and index buffers on this RTX 4090 |
| Tracked host-visible allocation | 1,364.84 | Process-wide tracker; includes the static buffers |
| Whole GPU (`nvidia-smi`) | 10,259 | Driver-wide peak, including allocations outside this process |

The before binary did not have the per-consumer line. Its log still identifies
the 4,064 MiB VT pool and 1,024 MiB geometry cap, and the paging census reached
1,073,736,140 bytes reserved against that cap. The after numbers are not
additive: geometry is a logical reservation, and the process-wide tracker
already includes VT and other Vulkan allocations. The static buffers use a
host-visible memory type on this driver; their size should not be labeled as
device-local VRAM even though allocation pressure can still affect Vulkan.

At the end of the after sample, VT used 1,584 of 12,800 slots, with 132 pinned
tails. It recorded zero evictions and zero calls to the O(pool) `pick_lru`
victim scan. The scan did not appear in this profile, so this task leaves its
algorithm in place and keeps the new cumulative scan count/time diagnostic for
future full-pool measurements.

## Verification

MSVC RelWithDebInfo `matter_editor` built successfully. The
`vt_residency_tests`, `geometry_hierarchy_tests`, and `props_tests` executables
each reported `ALL PASS`; `vulkan_smoke_tests` reported `ALL PASS` with zero
validation errors. The suites were built and run serially. `bash -n` on the
capture script, `python3 -m py_compile` on the audit tool, and
`git diff --check` also passed.
