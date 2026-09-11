# Local-light authoring and publication contract

This is the shared contract between World JavaScript, the provider/cache, CPU
reference tests, deferred raster lighting, and ray-traced hit lighting. The CPU
transport, spatial index, Vulkan publication, and deferred raster consumer are
implemented. Ray-traced local-light visibility remains a later lighting task.

## World JavaScript API

Both established forms remain valid:

```js
class Legacy extends World {
  static lights = [
    { position: [0, 2, 0], color: [1, 0.7, 0.4], intensity: 80, range: 9 },
  ];
}

class FurnishedRoom extends World {
  static lights = {
    sun: { dir: [-0.45, -0.8, -0.35], color: [2.2, 2.05, 1.8] },
    sky: { color: [0.08, 0.1, 0.16] },
    points: [
      { position: [4, 2.4, 7], color: [1, 0.55, 0.25], intensity: 90,
        range: 10, sourceRadius: 0.08, castsShadow: true },
    ],
    spots: [
      { position: [0, 6, 0], direction: [0, -1, 0], color: [1, 0.9, 0.7],
        intensity: 140, range: 18, sourceRadius: 0.12,
        castsShadow: true, inner: 18, outer: 32 },
    ],
  };
}
```

The top-level array is a point-light list. The object form resolves `points`
before `spots`, independent of JavaScript property insertion order. `pos` is an
alias for `position`; spots also accept `dir` for `direction`. Color defaults to
white, intensity to 1, range to 10 m, source radius to 0 m, and castsShadow to
false. Established spots may omit cone angles; both default to 180 degrees.

Position, color, direction, intensity, range, radius, and cone values must be
finite. Range is positive; RGB, intensity, and source radius are nonnegative;
castsShadow is a Boolean. A spot direction is nonzero and is normalized during
resolution. Cone values are half-angles in degrees with
`0 <= inner <= outer <= 180`. Invalid authoring fails the world load with the
list-entry property path. `castsShadow` requests visibility testing; it does not
promise raster shadowing. The baseline raster implementation may remain
unshadowed, while RT honors it.

Distances use world metres. Authored intensity is a candela-equivalent
scene-linear scalar. Resolution folds it into RGB radiant intensity:
`resolved.color = authored.color * authored.intensity`.

## Resolved record ABI

`world_lights::LocalLight` in `src/world_lights.h` is the only resolved record.
It is 64 bytes, aligned to 16 bytes, and maps to this std430 layout:

| Byte | C++ field | GLSL interpretation |
|---:|---|---|
| 0 | `position[3]`, `range` | `vec4 position_range` |
| 16 | `direction[3]`, `cos_outer` | `vec4 direction_cosOuter` |
| 32 | `color[3]`, `source_radius` | `vec4 color_sourceRadius` |
| 48 | `cos_inner`, `kind`, `flags`, `reserved` | `float, uint, uint, uint` |

Kinds are point=0 and spot=1. Flag bit 0 is castsShadow; other bits and the
reserved lane must be zero. Point direction is zero and its cone cosines are
-1. Spot direction points from the source toward the cone target. For a
receiver `P`, its cone cosine is
`dot(direction, normalize(P - position))`.

The resolve cache stores these fields individually in the same order at format
version 7. It does not serialize derived cells or revision. Cache load validates
the records and deterministically rebuilds both before returning a hit.

## World-space index ABI

`LocalLightPublication` owns ordered records, `LocalLightSpatialIndex`, and a
nonzero content revision over all record fields plus index configuration. The
default cell size is 8 m. Cell coordinates are signed and computed independently
on all axes as `floor(worldPosition / cellSize)`, so -0.01 m is cell -1 and an
exact +8 m boundary is cell +1.

Finite-range spheres are inserted into every intersecting cell. Each occupied
cell contributes its authored-order light indices to one compact uint32 list.
A light whose candidate AABB spans more than 4096 cells, or whose coordinates
cannot be represented as signed 32-bit cells, goes only into the explicit
authored-order `oversized_light_indices` list. Every lookup returns the selected
cell list followed by all oversized indices. It has no 64-light or other fixed
candidate cap.

`LocalLightCell` is a 32-byte GPU bucket:

| Byte | Field |
|---:|---|
| 0 | signed `cell[3]`, uint `offset` |
| 16 | uint `count`, uint `reserved[3]` |

`count == 0` is the empty sentinel. Buckets use open addressing with a
power-of-two count and linear probing. The CPU/GLSL hash is:

```text
h = uint(x) * 0x8da6b343
h ^= uint(y) * 0xd8163841
h ^= uint(z) * 0xcb1ab31f
h ^= h >> 16; h *= 0x7feb352d
h ^= h >> 15; h *= 0x846ca68b
h ^= h >> 16
bucket = h & (bucketCount - 1)
```

The same lookup accepts any world position, so visible G-buffer pixels and
off-screen RT secondary hits share it. `LocalLightIndexStats` reports occupied
cells, buckets, compact-list entries, oversized lights, worst candidates per
cell, and index upload bytes. Build/query allocation failures return an error;
they never publish a truncated result.

## Reference attenuation

For squared source distance `d2`, range `R`, and source radius `r`:

```text
cutoff = max(1 - d2 / (R * R), 0)
distanceTerm = 1 / (d2 + max(r, 0.0001 m)^2)
cone = point ? 1 : smoothstep(cosOuter, cosInner, cosTheta)
irradianceRGB = resolved.color * cone * cutoff^2 * distanceTerm
```

The value is exactly zero at and beyond range. Equal spot cone angles use a
hard step. `local_light_attenuation` and `evaluate_local_light_irradiance` are
the CPU reference; raster and RT must match them before applying their common
energy-conscious diffuse/GGX surface BRDF.

## Renderer publication and reload

The authoritative handoff is `WorldManifest::lights.local`. It is separate from
the fixed-size `VkSceneLighting` push constants. The Vulkan consumer contract is
an atomic method with this shape:

```cpp
bool VkSceneRenderer::update_local_lights(
    const world_lights::LocalLightPublication& publication,
    std::string& error);
```

The renderer compares the content revision, replaces records/cells/compact and
oversized buffers plus descriptors as one transaction, and advances its own
publication generation only after success. A different accepted revision resets
all temporal lanes whose samples include local direct or bounced lighting. A
failed update retains the last valid buffers/revision and leaves any pending
history reset armed. Renderer reset invalidates its accepted-revision bit, so
even identical content republishes after device/scene reset. An empty
publication is an explicit clear and must replace a previous nonempty one.

Full and live world reloads already rebuild the manifest and reset Vulkan
temporal state. Light-only source edits change the light revision while leaving
part content hashes stable; camera movement changes neither and must not rebuild
the index or geometry. Local direct illumination is a separate contribution and
must remain enabled when the diffuse-GI multiplier is zero.

The deferred composite binds one immutable publication per retired frame slot
as descriptor set 2: records at binding 0, cells at 1, compact indices at 2,
oversized indices at 3, and a 32-byte metadata record at 4. The metadata carries
record/bucket/oversized counts, cell size and inverse, worst candidate count,
and the direct-light owner. `LocalLightCell` must compile to std430 array stride
32; its three reserved words are scalar `uint`s in GLSL because a `uvec3` there
would align to byte 32 and incorrectly produce stride 48.

Raster reconstructs each visible G-buffer pixel's world position, probes only
that cell plus the oversized list, and evaluates the shared finite-range/spot
attenuation and energy-conscious diffuse/GGX BRDF. The contribution is
independent of diffuse GI. Baseline raster local lights are intentionally
unshadowed; flag bit 0 is retained for the RT visibility task.

`LocalDirectOwner::Raster` is the current ownership state. The RT follow-up must
publish a separate RT local-direct lane and may switch the metadata owner to
`RayTraced` only when that lane is ready for the same light revision. Composite
must select exactly one owner; it must never sum raster and RT local direct.
Debug-view index 7 (`Local-light candidates`) visualizes the indexed candidate
count at reconstructed world positions without scanning the complete light
array. Frame telemetry reports light count, occupied cells, worst candidates,
oversized count, compact entries, and upload bytes.
