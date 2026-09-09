# Baked Particle Water Mesh Animation

**Date:** 2026-08-26

**Status:** Implemented and accepted on `codex/dualsphysics-fluid-spike`

> **Historical rendering policy:** The per-frame animated-water BLAS/TLAS
> policy in section 7.3 was superseded on 2026-08-28. Animated water is now
> raster-only by design; see the current
> [render-eligibility design](../../../superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md). The
> capture, artifact, meshing, and raster-playback sections remain the record
> of the landed animation system.

**Scope:** PhysX section capture, weighted loop meshing, animation artifacts,
Vulkan raster playback, cached native-RT geometry, RiverFloatLab acceptance

## 1. Purpose

MatterEngine's accepted river simulation and gameplay field remain baked and
static at runtime, but the visible surface should move with shapes taken from
the actual PhysX particle flow. Each river section records one second of
particle positions at 30 frames per second. Two copies of that recorded period,
offset by half a second and blended with complementary weights, produce a
seamless periodic density field. The existing GPU isosurface mesher extracts
thirty independent visual meshes from that field during the build.

This design supersedes only the "water mesh remains static at runtime" visual
restriction in the now-deprecated
[river-presentation design](../../../deprecated/superpowers/specs/2026-08-24-real-time-river-presentation-floating-bodies-design.md).
The accepted simulation result, gameplay/presentation fields, CPU collision
mesh, terrain collision, and buoyancy inputs remain static. A later follow-up
replaced the static-only ray-tracing proxy with the matching animated frame.

## 2. Authored contract

Animation is opt-in on the imperative river-network builder:

```js
network.meshAnimation({
  framesPerSecond: 30,
  duration: 1.0,
  phaseOffset: 0.5,
});
```

The first implementation accepts exactly these three values. This is an
intentional format contract, not a hidden default: accepting arbitrary sample
rates would require resampling and a second temporal-quality policy. The
builder validates that the PBD fixed step is exactly compatible with the
requested rate. RiverFloatLab's `1/120` fixed step captures every fourth
simulation step.

The canonical network text and hash include the animation settings. Changing
only animation settings invalidates animation and visual products, but not the
accepted gameplay or coarse collision products. A network without
`meshAnimation()` follows the current static path byte-for-byte.

The engine representation is:

```cpp
struct HydrologyMeshAnimationSettings {
    bool enabled = false;
    std::uint32_t frames_per_second = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t sample_step_stride = 0;
    std::uint32_t phase_offset_frames = 0;
};
```

For the authored values this canonicalizes to `{true, 30, 30, 4, 15}`.

## 3. Capture semantics

### 3.1 Rolling device capture

The PhysX adapter keeps a thirty-slot rolling device ring while it simulates.
After every fourth successful `fetchResults`, it copies the active prefix of
`PxParticleBuffer::getPositionInvMasses()` device-to-device into the next ring
slot and records the active count and simulation step. Slots grow to the
needed active count and are reused; the adapter never allocates thirty times
`max_particles`.

Only position data is captured. Particle radius is a section constant;
velocity, phase, and stable id are not required by visual isosurface
extraction. Stable ids are append-only prefixes in the current adapter. At
finalization, the final quarantine flags remove the matching array indices
from every captured frame before publication.

The accepted capture is the last thirty chronological samples at or before
the fill sensor's completion batch. It must have:

- exactly thirty samples separated by four simulation steps;
- strictly increasing sample steps;
- finite positions inside the accepted diagnostic policy;
- no capture-frame count above the physical bake's `max_particles`; and
- the accepted final snapshot as or after its newest sample.

The current batch sensor discovers completion after a batch readback, so the
newest capture may be later than `completion_step`, but never later than the
existing accepted terminal snapshot. This preserves the current simulation
stop and acceptance semantics.

### 3.2 Backend seam

`FluidBakeOutput` gains `FluidParticleAnimationCapture`, containing thirty
host position frames and their step/count metadata. The backend still owns all
PhysX/CUDA objects and returns ordinary engine values. Test backends can supply
the same capture without CUDA. Progress callbacks remain count-only and do not
expose private particle state.

If animation is not authored, no ring exists and no additional device copy or
host readback occurs.

## 4. Seamless weighted density loop

For output frame `i` in `[0, 30)`, the mesher consumes capture frame `i` and
capture frame `(i + 15) % 30`. Let:

```text
t  = i / 30
wA = 0.5 - 0.5 * cos(2*pi*t)
wB = 1 - wA
```

Phase A has zero influence at its wrap (`i = 0`) and full influence when phase
B wraps (`i = 15`). Both sets remain present in the mesher input, but their
weights always sum to one. The half-phase does not duplicate stored capture
frames.

The GPU mesher adds a weighted two-span contract to `ParticleJob`:

```cpp
struct ParticlePhaseBlend {
    std::uint32_t split_index = 0;
    float primary_weight = 1.0f;
    float secondary_weight = 0.0f;
};
```

Indices below `split_index` use `primary_weight`; remaining indices use
`secondary_weight`. Static jobs set `split_index == particle_count` and retain
the existing field exactly.

For positive visual blend width, the existing smooth-min field changes from:

```text
sum(exp(-(distance - minimum) / blendWidth))
```

to:

```text
sum(phaseWeight * exp(-(distance - minimum) / blendWidth))
```

Zero-weight particles are skipped. The CPU reference evaluator and GPU field
and normal-gradient shaders implement the same formula. A weighted animation
job with zero blend width is rejected because a hard minimum cannot represent
a density crossfade. Existing unweighted zero-blend jobs remain valid.

This is a density blend, not a transparent mesh blend. It avoids doubled water
volume, doubled optical depth, and refraction artifacts.

## 5. Per-frame meshing and section ownership

After the section simulation is accepted, the product phase builds thirty
visual meshes through the same renderer-thread GPU mesher used by the static
snapshot. Each frame uses the same visual voxel, blend width, bounds, material,
limits, cancellation, and generation checks as the accepted static product.

Animated section and spillway ownership follows the current handoff clipping
rules. For every frame index, upstream, handoff patch, and downstream pieces
are clipped and welded with the same authored cut distances used by the static
network product. A frame with an open ownership cut, invalid topology,
non-finite value, or output-capacity overflow fails the animation product; it
never publishes a cracked partial loop.

The static visual mesh remains the authoritative RT proxy and fallback. The
animated frames do not alter gameplay-field or presentation-field extraction.

## 6. Animation artifact

Animation is stored separately from `.mhyd` so static section cache hits do not
load hundreds of megabytes and legacy static tools retain their bounded
artifact. Each section writes:

```text
animations/<section-id>-<animation-key>.mhwa
```

and each handoff writes the corresponding animation artifact under
`animations/handoffs/`. The network manifest records path, semantic key,
source static payload digest, frame count, frame rate, and payload digest.

The version-one `MHYDWAN1` payload contains:

- section/handoff identity and source digests;
- frame rate, frame count, phase offset, and loop duration;
- one finite world-space quantization AABB shared by every frame;
- thirty frame directory records with byte offsets, vertex/index counts,
  bounds, and per-frame digest;
- packed vertex and uint32 index payloads; and
- an overall payload digest.

Each packed vertex occupies twelve bytes:

```cpp
struct PackedWaterAnimationVertex {
    std::uint32_t position_xy_unorm16;
    std::uint32_t position_z_unorm16_normal_x_snorm16;
    std::uint32_t normal_y_snorm16_reserved;
};
```

Positions decode from the shared AABB. Normals use two-component octahedral
SNORM16 encoding. The reserved upper sixteen bits must be zero in version one.
Material identity is stored once per artifact. Indices remain uint32. The
serializer rejects a quantization error above `visual_voxel_m / 16`, a decoded
normal outside the finite unit-vector tolerance, out-of-range indices,
zero-area triangles introduced by quantization, truncation, or any payload
larger than 1 GiB.

Artifacts are written atomically and validated by reopening before the network
manifest can become Ready. Corruption or a stale source digest preserves the
last valid installed animation.

## 7. Runtime publication and playback

### 7.1 CPU publication

The immutable authored-fluid render publication gains animation metadata and
validated compressed frame payloads. Version one may retain compressed
artifacts in CPU memory after validation; it must not expand all frames to
general renderer vertices. A later file-mapped streamer can replace this
storage without changing the renderer contract.

All sections in one network share a network loop clock. Section-specific
deterministic phase seeds are deliberately not used because they would break
handoff continuity. Playback frame is:

```text
floor((animationTimeSeconds mod 1.0) * 30) mod 30
```

The existing shader animation time uses the same clock.

### 7.2 Vulkan raster lane

Animated water uses a dedicated direct-draw raster lane. It does not enter the
immutable static vertex arena or cull command table.
Per Vulkan frame slot, the renderer owns bounded water-animation vertex and
index buffers. When the selected 30 Hz frame changes, it writes that frame's
packed vertex and index payloads into fence-owned, persistently mapped
GPU-visible buffers and draws them directly in the ordinary G-buffer pass. The
water vertex specialization decodes the 12-byte packed position/normal ABI.
This preserves GPU decode while avoiding both a 28-byte-per-vertex compute
expansion and a redundant staging-to-device full-frame copy.

The CPU decode oracle is water-specific:

```cpp
struct VkWaterAnimationVertex {
    matter::Float3 position;
    matter::Float3 normal;
    std::uint32_t material_index;
};
```

Runtime keeps this expanded record out of device memory. Per-draw quantization
bounds and material identity travel in graphics push constants; the water
raster vertex specialization decodes each indexed vertex, supplies constant
tint and surface channels, and reads transform, water-field binding,
generation, and temporal identity from the same draw-transform record as
static water. It feeds the existing `gbuffer.frag` and shared
`water_surface.glsl`; there is no second water material implementation.

Writes occur only when the 30 Hz frame changes, not every presentation frame.
Every mapped buffer is owned by one Vulkan frame slot and retained by the
normal frame-serial lifetime system. Bounds
and counts are validated before command recording. A rejected
frame records no animated draw and leaves the static fallback visible.

### 7.3 Cached animated ray-tracing geometry

While animated playback is healthy, `rt_proxy_only` suppresses the accepted
static water proxy in both raster and RT selection. A compute pass expands the
same packed frame used by raster into a fence-owned 28-byte RT vertex buffer;
the combined index buffer is rebased once during frame preparation. The BLAS
therefore traces the exact visible animation frame and keeps reflections,
shadows, refraction, and fog-depth intersections coherent with the G-buffer.

The one-second loop has only thirty stable frame identities. The renderer
builds a BLAS on the first visit to each identity, caches that acceleration
structure by animation frame index, and reuses it on every later loop. Decoded
vertex/index storage remains bounded to frames-in-flight; the cache retains
only acceleration structures. The default LRU budget is 2048 MiB and can be
overridden with `MATTER_WATER_BLAS_CACHE_MB`. In-flight submissions retain
shared ownership, so eviction cannot destroy referenced geometry.

If playback, compute decode, or BLAS preparation fails, the renderer clears
the dynamic generation and restores the accepted static proxy in both raster
and RT lanes. It never mixes an animated raster frame with stale RT geometry or
a stale water-field generation.

## 8. Memory and performance budgets

The accepted RiverFloatLab baseline is:

| Section | Particles | Vertices | Triangles |
|---|---:|---:|---:|
| upper | 382,067 | 271,800 | 541,468 |
| lower | 343,860 | 247,829 | 486,076 |

Version-one budgets are:

- no more than 300 MiB compressed animation artifact per section;
- no more than 700 MiB compressed CPU animation payload for the complete
  two-section RiverFloatLab network;
- per-slot packed and decoded animation buffers are bounded by the published
  frame capacity and the renderer's frames-in-flight count;
- animated BLAS cache memory is LRU-bounded to 2048 MiB by default;
- no more than 1.5 ms median and 3.0 ms p95 GPU cost for upload/decode/draw of
  the visible animated water set at 2560x1440 on the installed RTX 4090;
- no steady-state heap allocation after publication; and
- no frame upload when the selected 30 Hz frame has not changed.

The bake reports capture copy/readback time, thirty-frame meshing time,
quantization/serialization time, artifact bytes, maximum frame counts, runtime
decode time, uploaded bytes, and animated draw time separately.

## 9. Failure policy

- Invalid authored values fail world definition with a source-oriented
  `hydrology.meshAnimation` diagnostic.
- Capture allocation, CUDA copy, or readback failure fails only the authored
  animation product after preserving the accepted static water artifact.
- Fewer than thirty valid terminal samples produces a reported static fallback;
  frames are never duplicated to conceal the shortage.
- Meshing, seam stitching, quantization, serialization, digest, or capacity
  failure publishes no animation generation.
- Loading a corrupt/stale `.mhwa` keeps the previous valid animation when one
  exists; otherwise static water renders.
- Runtime upload/decode/range failure disables the complete network animation
  generation and restores static raster water. It does not disable a single
  section and expose a moving/static crack.
- Device loss follows the renderer's existing device-loss path.

## 10. Testing

### 10.1 CPU and artifact tests

- DSL canonicalizes exactly `{30, 30, 4, 15}` and rejects every incompatible
  value and incompatible fixed step.
- Static networks retain their previous canonical identity and outputs.
- Rolling capture orders wrapped samples and filters quarantined indices.
- Phase weights are complementary, hide each phase's wrap, and are bit-stable.
- CPU weighted-field reference matches analytic primary-only, secondary-only,
  equal-overlap, and zero-weight cases.
- `.mhwa` round-trips byte-identically and rejects corruption, truncation,
  stale source keys, oversized counts, invalid reserved bits, bad indices, and
  excessive quantization error without mutating the prior loaded artifact.

### 10.2 GPU and renderer tests

- Vulkan weighted field and emitted mesh match the CPU oracle within the
  existing mesher tolerances.
- Two half-phase fixtures do not double the measured surface thickness.
- Packed vertex GPU decode matches the CPU decoder within quantization bounds.
- Frame selection wraps from 29 to 0 with a shared network clock.
- A healthy animated draw suppresses the static proxy in raster and RT, decodes
  one matching RT vertex frame, and emits one dynamic RT geometry record.
- The first visit to a loop frame builds one BLAS; a repeated visit records a
  cache hit and performs no second BLAS build.
- A failed animated draw restores the static raster/RT proxy atomically.
- Repeated presentation frames within one 30 Hz interval upload zero bytes.
- Field-generation replacement is transactional across proxy and animation.

### 10.3 RiverFloatLab acceptance

The MSVC/PhysX build performs a cold RiverFloatLab bake, then a cache-hit run.
It records artifact/count/timing summaries and captures at least thirty
consecutive playback frames at each of:

1. the upper boulder rapids;
2. the curve before the waterfall;
3. the waterfall and plunge pool;
4. the first spillway handoff; and
5. the lower rapids.

Contact sheets and the live editor must show moving silhouettes, coherent foam
and shader motion, no loop pop, no doubled water level, no section crack, no
terrain disappearance, and unchanged floating-body/terrain collision. Static
fallback and animation-disabled comparison captures use the same cameras.

The canonical Windows gate is the MSVC `RelWithDebInfo` editor and tests. No
MinGW/GCC executable, runtime DLL, or acceptance result is valid for this
feature.

## 11. Implementation boundaries

This milestone does not add runtime fluid simulation, dynamic water collision,
particle rendering, arbitrary loop durations, temporal mesh interpolation,
portable serialized BLAS, or new isosurface extraction. The existing GPU
mesher and water material remain the only visual field and shading
implementations.
