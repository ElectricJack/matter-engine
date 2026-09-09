# Baked Water Mesh Animation Acceptance

**Date:** 2026-08-26

**World:** `RiverFloatLab`

**Result:** Pass

## Acceptance result

RiverFloat now bakes a one-second, 30-frame water-surface animation from the
accepted PhysX particle flow. Each output frame meshes two copies of the
captured particle cycle with a half-cycle offset and complementary weights.
The editor validates and loads the resulting `.mhwa` artifacts
transactionally, advances all visible sections from one common 30 Hz clock,
draws the packed animation directly from GPU-visible buffers, and restores the
accepted static water surface if activation fails.

The animated surface changes visuals only. The accepted gameplay field,
buoyancy input, collision products, and ray-tracing proxy remain static.

## Test system

- GPU: NVIDIA GeForce RTX 4090
- Driver: NVIDIA 610.74 (`0x98928000`)
- Vulkan API: 1.4.341
- Compiler: Visual Studio 2022 17.14.7 / MSVC RelWithDebInfo
- PhysX: 5.6.1 checkout `5ca9f472105a90d70d957c243cb0ef36fe251a9f`
- CUDA: 12.8.61

## Artifact results

| Product | Bytes | MiB | Payload digest |
|---|---:|---:|---|
| `upper-e056b8b76ea9b081.mhwa` | 287,652,441 | 274.327 | `6253746c0e7f176a` |
| `lower-c5df9175634adfdd.mhwa` | 249,148,665 | 237.607 | `50fa8ad1228997ab` |
| `handoffs/pool-one-ed1aa942125527ca.mhwa` | 1,181,796 | 1.127 | recorded by manifest |
| **Loaded total** | **537,982,902** | **513.060** | — |

Both section artifacts remain below the 300 MiB per-section limit. The loaded
compressed set remains below the 700 MiB CPU limit. Runtime activation reported
`compressed=513.1 MiB slot=17.6 MiB`, so the per-visible-set GPU slot remains
below 96 MiB.

The accepted upper section contains 382,067 particles at step 2,560. Its 30
frames contain 267,065–275,464 vertices and 528,322–544,016 triangles. The
accepted lower section contains 343,860 particles at step 2,304. Its 30 frames
contain 229,538–250,081 vertices and 450,619–463,385 triangles.

The recorded manifest-ready replay took 103.002 seconds: 1.739 seconds for the
handoff mesh, 89.530 seconds for the handoff animation mesh, and 7.131 seconds
for serialization. The two section artifacts were immutable cache hits in this
trace, so this number is deliberately not presented as a single wholly uncached
two-section bake measurement. The valid section artifacts were produced earlier
in the same diagnostic cycle.

Source: `MatterEditor/build/qa/water-animation-2026-08-26/fresh/traces/timings.json`.

## Runtime performance

The final hidden, borderless 2560x1440 acceptance run collected 13,691 frames
over 20 seconds after static geometry had remained upload-stable for 30 frames.

| Measurement | Result | Limit |
|---|---:|---:|
| Animated-water GPU median | 0.069632 ms | 1.5 ms |
| Animated-water GPU p95 | 0.072704 ms | 3.0 ms |
| Overall frame median | 1.137400 ms | informational |
| Overall frame p95 | 4.181800 ms | informational |
| Water-frame uploads | 1,200 | 30 Hz transitions only |
| Unchanged rendered frames without upload | 12,491 | expected |
| Decode dispatches | 0 | direct packed draw path |
| Steady-state allocations | 0 | required |
| Static vertex/cluster/instance uploads | 0 / 0 / 0 | required after warm-up |
| Immediate submits | 0 | required |
| Vulkan validation errors | 0 | required |

The result file is
`MatterEditor/build/qa/water-animation-2026-08-26/perf-2560x1440-validated-span-final.json`
with SHA-256
`0C859387693D02C32E746B67C03917F152895B8B12820B372D146196D6E68D43`.
Ray tracing was available but disabled for this raster playback measurement;
the immutable static water proxy remains the ray-tracing representation.

During performance diagnosis, a 1.5-second frame stall was traced to validating
and hashing the complete 513 MiB artifact set on every frame selection. Artifact
spans are now fully validated once during transactional activation and cached
for the common playback clock. A regression test verifies that steady-state
selection performs no validation pass.

## Visual evidence

The acceptance timeline is
`MatterEngine3/tools/river_water_animation_acceptance.timeline`. Its captures
are under `MatterEditor/build/qa/water-animation-2026-08-26/fresh/`:

- `overview.png`
- `upper-boulder-rapids.png`
- `curved-ravine.png`
- `waterfall-side.png`
- `plunge-pool.png`
- `spillway-transition.png`
- `lower-rapids.png`
- `floating-crates-rafts.png`
- `second-pool.png`

Live timeline inspection covered the half-cycle overlap and the frame-29 to
frame-0 boundary. No gross geometry explosion, section crack, or exposed-terrain
pulse was observed. The stills show the upstream channel, boulder rapids, curved
ravine, waterfall and plunge pool, pool spillway, lower section, and floating
crates/rafts.

The forced-failure timeline is
`MatterEngine3/tools/river_water_animation_fallback.timeline`. With the animation
artifact deliberately unavailable, the editor logged:

```text
[hydrology] water mesh animation fell back to the accepted static surface: water animation artifact is missing
```

The usable fallback frame is
`MatterEditor/build/qa/water-animation-2026-08-26/fallback/static-artifact-load-fallback.png`.

## Verification

The final MSVC build command completed with exit code 0:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo
```

The complete CTest suite ran from a fresh short Windows temporary directory to
avoid path-length and stale-cache interference:

```powershell
$env:TEMP = 'C:\ct\w2'
$env:TMP = $env:TEMP
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo `
  -C RelWithDebInfo --output-on-failure
```

Result: **84/84 tests passed** in 363.97 seconds. This includes the dedicated
water capture/artifact/playback/render tests, hydrology and PhysX contracts,
Vulkan smoke tests on the RTX 4090, package tests, and compiler-policy tests.

## Known limitations

- A single wholly uncached end-to-end timing sample was not retained; the
  recorded 103.002-second trace reuses the accepted upper/lower artifacts and
  rebuilds the handoff animation.
- The animated mesh is raster-only. Ray tracing continues to use the accepted
  static proxy by design.
- The current acceptance format is intentionally fixed at 30 frames over one
  second with a 15-frame phase offset.
