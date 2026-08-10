# Task 14 — GPU timing/status instrumentation and final acceptance

## Status

`DONE_WITH_CONCERNS`.  The instrumentation, source/CPU contracts, focused
Vulkan smoke coverage, and one full Engine/Editor link are green.  Final CLI
coverage was deliberately scoped: it round-trips every property and captures
the representative scene cases instead of repeating the previously-proven
25-option screenshot matrix.

## Delivered contract

- GPU zones 0 through 11 retain their ABI.  The append-only lanes are
  `Atmosphere=12`, `CloudShadows=13`, `VolDensity=14`, `VolScatter=15`, and
  `VolIntegrate=16`; `Count=17`.  The existing combined Volumetrics zone 9 is
  still recorded.
- `VkVolumetrics` exposes a typed optional `VolumetricPassBoundary` and calls
  it around the actual density, scatter, and integration passes.  Atmosphere
  timestamps are gated by `generation_pending`, and cloud-shadow timestamps
  cover the real reproject/density/prefix record only.
- `FrameStats`, `ViewerStats`, Lighting/Performance UI, PERF JSON, issue JSON,
  and positional `STATS` receive all five timing lanes.  Froxel bytes are read
  from the active persistent bundle; cloud-shadow bytes come from the actual
  persistent allocation.
- The stable `STATS` prefix remains unchanged through
  `vol_resource_generation`.  Its exact final suffix is
  `gpu_atmosphere_ms,gpu_cloud_shadows_ms,gpu_vol_density_ms,`
  `gpu_vol_scatter_ms,gpu_vol_integrate_ms,cloud_shadow_memory_MiB`.
- FIFO command documentation now includes `set`, `get`, `cam`, `stats`,
  `shot`, and `quit` examples without changing the parser.

## TDD and build evidence

The new source/property contract and Vulkan smoke contract were added first.
They produced a clean RED against `8453e385` for missing FrameStats/STATS
lanes, zones 12--16, and the typed callback.  After implementation:

- `make -C MatterEngine3/tests run-property-editor WIN_CXX=/ucrt64/bin/g++.exe` — `ALL PASS`.
- `make -C MatterEditor build/windows/vulkan_smoke_tests.exe ...` — green;
  `MATTER_VK_SMOKE_MODE=atmosphere MatterEditor/build/windows/vulkan_smoke_tests.exe`
  — `ALL PASS`, RTX 4090, Vulkan validation errors `0`.
- `make -j4 -C MatterEngine3 ...` and `make -j4 -C MatterEditor windows ...`
  — both exit `0` (only existing compiler warnings).
- `bash -n MatterEngine3/tools/atmosphere_cloud_shots.sh` — green.

## Scoped final CLI evidence

The first `final` launch completed the 25 froxel `set/get` round-trips and
captured noon/sunset/twilight, current/improved/high/ultra/custom presets,
orders 1/2/4, self/cross layer receivers, disabled/enabled ground shadow,
low-fog shadow, and translated/boundary near-far cases.  Its representative
boundary STATS row was:

```text
STATS,boundary,11.07,0.03,1.41,4.39,2471,211,276506,2259,0,2470,0,32768,73.5,1024.0,120,68,96,25.40,12,0.000,0.185,0.053,0.165,0.009,60.00
```

This shows a steady atmosphere lane of `0.000 ms`, cloud shadows `0.185 ms`,
density/scatter/integrate `0.053/0.165/0.009 ms`, 25.40 MiB froxels, and
60.00 MiB cloud-shadow memory.  The harness then timed out only on the final
redundant moving-frame capture because the Editor's 120-second perf timer
exited.  The one allowed rerun used a 240-second timer, but that timer moved
the Editor before command transport readiness; it was stopped before any
capture, with no third run.  Harness cleanup removed the first-run PNGs before
that stopped rerun, so the retained visual evidence is Task 13's inspected
receiver captures:

- `MatterEditor/build/validation/atmosphere-clouds/receivers/receivers_ground_object_enabled.png`
- `MatterEditor/build/validation/atmosphere-clouds/receivers/receivers_cross_layer_enabled.png`
- `MatterEditor/build/validation/atmosphere-clouds/receivers/receivers_low_fog_enabled.png`

The retained stopped-rerun telemetry records validation errors `0`, but is not
used as the configured final-scene timing row.  `atmosphere-presentation` was
not rerun after the final link because the bounded acceptance window was spent
on the required final rerun; this is the remaining acceptance concern.

## Scope deviation and hygiene

The final suite intentionally does not recapture the old full 25-option visual
matrix: every pair is still exercised through property round-trips, while the
representative captures cover the requested quality, sun, receiver, and
near/far behaviors.  No protected dirty path was staged or changed.

## Fix round 1/5 -- timing and acceptance hardening

### RED to GREEN evidence

- The new focused source contract was first run against the pre-fix source and
  failed six checks: there was no immediate-candidate timestamp/result path and
  no staged final acceptance, exact `set`/`get`, or ffmpeg comparison contract.
- The new RTX 4090 atmosphere gate was then run against that same production
  baseline.  It failed exactly `dirty atmosphere candidate publishes a positive
  GPU timestamp`, while reporting `validation errors: 0`.  The failure exposed
  that `VkAtmosphere::build_candidate()` submits its LUT work synchronously
  before the renderer frame query pool is reset, so zone 12 only bracketed the
  later steady no-op `record()` call.
- `VkAtmosphere` now owns a two-query timestamp pool for its immediate command
  buffer.  A successful candidate carries the measured dispatch duration until
  its atomic commit, and the renderer publishes it to zone 12's EMA.  A steady
  or failed transaction assigns exactly `0.0 ms`; no CPU duration is used.
- The final harness starts without `MATTER_PERF_*`, asserts every requested
  property value, verifies effective froxel STATS dimensions and memory, checks
  capture/done markers, requires hash plus ffmpeg SSIM differences for enabled
  state/order/receiver images, bounds moving-frame SSIM, scans renderer and
  validation errors, checks child exit status, and promotes only a fully
  accepted staging directory.

### Focused verification after the fix

- `make -C MatterEngine3/tests run-property-editor WIN_CXX=/ucrt64/bin/g++.exe`
  -- `ALL PASS`.
- Werror Vulkan smoke target rebuilt successfully with the UCRT64 toolchain.
  `MATTER_VK_SMOKE_MODE=atmosphere MatterEditor/build/windows/vulkan_smoke_tests.exe`
  -- `ALL PASS` on NVIDIA GeForce RTX 4090 (driver 610.74, Vulkan 1.4.341),
  including the dirty-positive and immediate-steady-zero zone-12 assertions;
  `validation errors: 0`.  The post-exit ReShade loader message is external to
  the test and appears after its all-pass result.
- `bash -n MatterEngine3/tools/atmosphere_cloud_shots.sh` -- pass; the Editor
  was relinked successfully with the UCRT64 compiler.

### One bounded final run and retained evidence

The only allowed post-fix `final` run launched without an automatic PERF
timer, wrote to a private staging directory, and never reached its first
capture: after the StreamMountain world bake completed in `23375 ms`, its
existing sector-publication readiness wait did not observe a published sector.
It stopped with `ERROR: StreamMountain sectors did not publish`; the enclosing
runner imposed exit `124` and cleanup sent `quit`.  Per the bounded-run rule,
there was no retry.  No canonical PNG, STATS, or telemetry was overwritten.
The failed stage logs are retained as:

- `MatterEditor/build/validation/atmosphere-clouds/final/final_failed_readiness_20260810_0415_viewer.log`
- `MatterEditor/build/validation/atmosphere-clouds/final/final_failed_readiness_20260810_0415_commands.log`

The temporary FIFO, generated shader cache, and now-redundant staging leaf
were removed after preserving those logs.  This leaves the final CLI evidence
as `DONE_WITH_CONCERNS`: the hardened script and focused GPU timing gate are
green, but the single permitted final visual run was blocked by readiness
before the new image/STATS assertions could execute.  Task 13 receiver PNGs
remain the retained inspected visual evidence.
