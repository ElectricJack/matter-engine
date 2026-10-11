# Headless terrain fields and frozen region inputs

`matter_terrain` compiles the existing CPU field/noise interpreter directly from
`MatterEngine3/src/terrain_field.cpp`. The algorithms, integer noise hashes,
96-register limit, octave accumulation order and legacy FNV text identity have
one source of truth. Its production link dependency is `matter_contracts`;
private legacy includes are neutral field/overlay and hydrology data types.
The target links no engine archive, QuickJS, graphics, windowing or ECS library.
The original field fixture additionally compiles the CPU river overlay/geometry
sources; those concrete algorithms are test dependencies only.

The public header is `matter/terrain/field.h`. `freeze_region(request, encoded)`
validates the shared `TerrainRequest` envelope, compiles a field, and deep-copies
resolved bytes and assets into owned immutable storage. Caller mutations and
release cannot change a completed input. `sample_local` returns one field sample;
`sample_region` returns a closed lattice and retains its `FrozenRegion`, including
the request identity, field and dependencies. Both use the supplied job controls
and echo request ID, source revision and generation epoch. Admission remains the
coordinator's responsibility (`services::admit` rejects stale envelopes).

This is the field stage for later chunk generation. It does not implement
`TerrainService::generate`, produce geometry, resolve imports, write caches, or
publish collision/graphics resources. Ordered integration owns existing host,
world and central build entrypoints. Relocation of the legacy source also belongs
to integration, so the module currently builds within the monorepo.

## Encoding and capability boundaries

| Accepted capability at version 1 | Meaning |
| --- | --- |
| `matter.terrain.generate` | Required shared terrain request envelope; mesh generation is a later stage |
| `matter.terrain.field.text` | Exact legacy field tape bytes, supplied explicitly, with SHA-256 matching `request.field.content` |
| `matter.terrain.field.world-f32` | CPU field evaluation in checked float32 world coordinates; double origins remain separately owned |
| `matter.terrain.field.sample` | Height, positive-solid density, moisture and relief |
| `matter.terrain.boundary.shared-samples` | Identical samples at coincident world coordinates for matching spacing |
| `matter.terrain.boundary.aligned-samples` | Identical samples at coincident coordinates for power-of-two spacing ratios |

Every requested boundary policy must appear in `required_capabilities`, as the
shared contract requires. Unknown capabilities, operation names, encoding
versions, GPU targets and script callback targets fail explicitly. CPU is the
portable C++ path; a compiled WASM backend is not qualified by these native tests.
Native overlay callbacks, surface/habitat tapes, external field-resource opcodes,
and GPU tapes are not encodings of this public field interface. Unresolved asset
dependencies fail with `missing_dependency`. Resolved inputs are validated and
retained as frozen dependency identities; this tape has no opcode that reads an
asset or resource. A future reader must declare a new capability/encoding.

The text encoding supports `const`, `input wx/wy/wz`, `noise2/ridge2`,
`noise3/ridge3`, `warp2`, arithmetic, clamp/blend/smoothstep/abs/oneminus/pow and
the existing height/density/moisture/relief/seaLevel/biome directives. It preserves
the legacy optional implicit `height - world_y` density and heightfield
recognizer. Unlike the permissive legacy parser, the public boundary rejects
trailing tokens/numeric prefixes, duplicate directives and nonfinite literals.
Noise seeds are uint32, octave counts are integers in [1,16], frequency is
nonnegative, gain is in [0,1], and lacunarity is positive. Clamp bounds are ordered
and smoothstep bounds strictly increase. Nonfinite evaluated outputs fail the
whole result, including arithmetic overflow.

`field.content` identifies exact text bytes with SHA-256. Transitive dependency
digests, algorithms, resolution and boundary settings stay explicit in the
request and belong in coordinator cache keys. `legacy_text_hash()` preserves the
separate FNV value for compatibility adapters; it is never a portable digest.

## Sample coordinates and algorithm boundary extensions

Positions and spacing are metres, right-handed Y-up. `sample_domain` is local
to `origin_metres`. Conversion adds/subtracts the origin in double precision;
invalid/nonfinite/overflowing conversions leave the destination untouched.
Height is world altitude; density evaluates world y. Geometry producers must
subtract the region origin when producing local vertices.

Each axis has `cell_counts + 1` samples, including both endpoints. Interior
positions are `lo + (hi-lo)*index/cell_count` in double, with the last sample
assigned exactly `hi`. X varies fastest, then Y, then Z. Adjacent regions both
include their shared face. Region keys describe adjacency; callers supply origins
and domains consistently. Keys are not multiplied into origins implicitly.

These two boundary capabilities extend the fixed `TerrainBoundaryEnvelope`:

* `shared-samples@1` requires the neighbor's encoded spacing to match exactly.
  For the same frozen field/dependencies, a pair of consistently placed domains
  evaluates every coincident face sample identically within a single CPU target.
* `aligned-samples@1` accepts power-of-two spacing ratios. When face origins and
  tangential lattices align, coincident coarse/fine points agree exactly. It
  provides no interpolation, transition triangles, skirts or watertight mesh
  guarantee; the meshing stage must declare its own extension semantics.

The boundary's local spacing must equal the encoded float32 lattice spacing.
This field stage requires exactly uniform domain-derived spacing in all axes.
An envelope does not contain a neighbor origin/domain, so the coordinator must
check domain alignment and matching field/dependency identities before relying
on these conditional guarantees. `seam_resource` requests fail explicitly here;
the field stage cannot supply mesh transition bytes.

Fields are pointwise and have no region-edge clamp or neighboring-sector lookup.
The field sample itself needs no halo. Algorithms that derive normals/slope or
curvature must request their probe halo explicitly: the legacy slope uses
central differences at +/-0.5m in X/Z; curvature uses its four-point ring at
`max(radius,0.25m)`. Include those probes in the sample domain before evaluating
them; `sample_local` rejects coordinates outside the declared domain. Both sides
must use the same probe distance and world-coordinate convention. Domain warp
evaluates its frozen source at displaced world coordinates, not a neighbor's
resident samples.

The legacy interpreter evaluates world coordinates as float32. Freeze rejects
domains whose double origin addition loses extent, float32 ULP exceeds spacing/8
(rounding error then exceeds the declared spacing/16 bound), or any noise octave
could exceed its safe int32 lattice. Warp displacement is included conservatively
in that check. Large origins remain representable by the conversion API even
when this particular field backend rejects their evaluation precision. Another
precision profile requires a declared algorithm/backend capability.

## Budgets, ownership and fixtures

Cancellation/deadline checkpoints surround preparation and each bounded sample,
and precede publication. Failure publishes no partial lattice. Operations count
tape instructions/noise octaves plus recursively evaluated warp prefixes, with a
conservative four full tapes per sample. A whole grid checks operation, query,
output and working ceilings before allocating. Warp depth is bounded separately.
Working accounting conservatively reserves the full register-operation capacity,
input metadata, text, payload bytes and encoded dependency assets. These are
portable accounting ceilings, not a measurement of allocator overhead or process
peak memory. A snapshot also checks its output ceiling. Grid results retain this
snapshot without recopying it and account their newly generated sample storage.
Point samples account one sample. Progress sends one completion event, before the
last cancellation check; a zero event budget disables it. Callback exceptions
produce `backend_failure`. Immutable inputs can be sampled concurrently; borrowed
factory arguments must not be mutated during the synchronous call.

`tests/region_tests.cpp` uses only public field/contracts interfaces and neutral
contract fixtures. Named fixtures are printed in its output:

| Fixture ID | Evidence |
| --- | --- |
| `terrain.coordinates.v1` | Double-origin round trip, local/world altitude, exact endpoints and array order |
| `terrain.adjacent_faces.v1` | All six signed faces, negative coordinates, 2D warp plus 3D density; exact same-target equality |
| `terrain.aligned_detail.v1` | Coincident 2:1 coarse/fine samples, exact same-target equality |
| `terrain.frozen_dependencies.v1` | Deep resource/asset ownership, source release, missing dependency failure |
| `terrain.rejections.v1` | Target/encoding/capability failures, malformed fields, unsafe coordinates and invalid boundaries |
| `terrain.control_and_ownership.v1` | Budget/cancellation failures, callback exceptions, final cancellation, concurrent reuse and stale-result rejection |
| `terrain.legacy_field_noise` | Unchanged `MatterEngine3/tests/terrain_field_tests.cpp`, including noise, column-cache and river overlay fixtures |
| `terrain.legacy_surface_noise` | Unchanged `MatterEngine3/tests/surface_field_tests.cpp`, including the shared noise core |

The original JS authoring `terrain_noise_tests.cpp` exercises a separate QuickJS
module and is not linked into this component. Its behavior is outside this field
interpreter's extraction. Existing native field/noise expectations and tolerances
are unchanged. Cross-target noise bytes are not promised identical.

Supported native Windows toolchain, from PowerShell:

```powershell
libs/TerrainGeneration/build-windows.ps1 -Config RelWithDebInfo -Test
```

From WSL, invoking the same repository toolchain resolver:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/libs/TerrainGeneration/build-windows.ps1")" \
  -Config RelWithDebInfo -Test
```

Independent Linux build (two compiler jobs, serial tests):

```bash
cmake -S libs/TerrainGeneration -B /tmp/matter-terrain-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-terrain-build --parallel 2
ctest --test-dir /tmp/matter-terrain-build --output-on-failure -j 1
```

Production-only consumers set `BUILD_TESTING=OFF` and
`MATTER_TERRAIN_BUILD_TESTS=OFF`, or link `matter_terrain` from a parent CMake
project. No central manifest or live-world edits are needed for these builds.
