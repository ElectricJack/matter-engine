# Increased staging reservations

User explicitly requested larger staging after the moving test exceeded 1GiB.
Terrain cache audit and interactive review launcher now reserve 2GiB CPU vertex
staging, 256MiB CPU index staging, and 2GiB Vulkan vertex storage up front.
Vulkan index reserve remains 256MiB. These are separate allocations, not one
shared 2GiB budget. Generic renderer defaults are unchanged; configured performance
and review launches use these values. No executable change required for sizes.

Repeated movement run: C:/tmp/matter-blas-mountain/moving-profile-2gb-v1.
Same 240-position/four-frame cadence and build as moving-profile-v1. Caches are
warmer after the first pass, so whole-run speed difference cannot be attributed
solely to larger reservations. Direct allocation/growth events and their timing
are the relevant verification. Results follow below.

Completed exit 0.

```json
{
  "growth_after_init": [],
  "frames": 959,
  "median_ms": 42.682300000000396,
  "p95_ms": 58.392100000004575,
  "max_ms": 179.9835000000021,
  "static_max_ms": 0.2364
}
```

No CPU staging or Vulkan static capacity growth after initialization. The
large capacity-rewrite stall is absent in this run. Remaining movement frame
intervals still miss the sub-10ms target. Warm-cache differences prevent a pure
A/B attribution of general frame improvement. End-to-end goal remains active.
