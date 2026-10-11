# Headless terrain fields, meshes and CPU products

`matter_terrain` compiles the existing CPU field/noise interpreter directly from
`MatterEngine3/src/terrain_field.cpp`. The algorithms, integer noise hashes,
96-register limit, octave accumulation order and legacy FNV text identity have
one source of truth. Its production link dependency is `matter_contracts`;
private legacy includes are neutral field/overlay and hydrology data types.
The target also compiles the original `terrain_mesher.cpp` surface-nets and
contour implementation directly. The target links no engine archive, QuickJS,
graphics, windowing or ECS library.
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

The field API does not mesh or publish resources. The separate
`matter/terrain/meshing.h` API implements `TerrainService::generate` with the
same frozen field and the existing CPU mesher, as described below. Neither API
resolves imports, writes caches, or publishes to a runtime. Ordered integration owns existing host,
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

## Mesh profile and explicit sampling domain

`CpuTerrain(encoded_field, ChunkSettings{size})` copies the field and implements
the shared `TerrainService`. `generate_chunk(request, field, settings, control)`
is the equivalent synchronous free function. Requests must require
`matter.terrain.generate@1` and `matter.terrain.mesh.surface-nets@1`. Geometry
contains local float32 positions, native gradient normals, uint32 indices, native
material bucket order and a single placed instance. Four neutral PBR slot IDs
(`terrain.material.0` through `.3`) preserve the native grass/dirt/rock/snow
classification; the consuming material adapter supplies appearance. There are
no texture, renderer, collision-solver or resident-sector pointers.

The initial bounded contour profile has these exact rules:

* The output tile is `[0,size]^3` relative to `origin_metres`. Size is a power
  of two from 8 through 64 metres. Each origin coordinate is an integer multiple
  of size, with the world field precision checks from the field stage retained.
* Spacing is 2, 4 or 8 metres (native rungs 0, -1, -2), with 2–32 output cells
  per axis. Finer spacing would out-resolve the native canonical 2m contour and
  is explicitly unsupported. `detail_level` is an opaque coordinator identity;
  actual sampling is set by the domain and cell counts.
* The request's **sample domain includes the halo**, and is exactly
  `[-3*spacing,size+3*spacing]^3`, with `cell_counts = size/spacing+6` on each
  axis. For size 16 and spacing 2, pass `[-6,22]^3` and `{14,14,14}`.
  Geometry bounds cover the output tile, not this sample domain. The explicit
  halo covers native ghost cells, central-difference normals, canonical contour
  probes and material slope probes. A missing halo is an error, never an
  implicit read beyond the declared domain. Sampling fields with a wider range
  remains available through the separate field API.
* The complete request is validated first. A private field-stage request then
  receives only field capabilities and no mesh boundary requests. The product
  echoes the original job, region, key, origin, field digest, detail and full
  sampling cell counts. Unknown required capabilities or versions fail.
* The native process contour mode must be enabled. Its default is enabled;
  `MATTER_CONTOUR_SEAMS=0` returns `unsupported_capability`. This adapter never
  changes process-global bake mode or silently switches seam algorithms.

The original mesher is compiled from its source of truth, not copied or
reimplemented. Live-world/central manifest migration remains the ordered
integration task's work. The old column/overlap-band rollback API remains
available to its existing consumers; it is not exposed as this contour profile.

## Mesh boundaries and transition guarantees

Each requested neighbor uses `matter.terrain.boundary.contour-2m@1`, also listed
in `required_capabilities`. Local spacing must match the sample lattice.
Neighbor spacing must be 2/4/8m at equal or 2:1 resolution. Other ratios and
field-only boundary policies fail explicitly. The coordinator remains responsible
for matching field/dependency identities, origins and tangential domains;
neighbor keys in the shared contract express logical adjacency, not an implicit
origin computation. No neighboring mesh or live sector is queried.

The contour algorithm terminates each tile on a world-aligned 2m contour.
The selected six-face oblique-plane fixtures compare shared contour segments
**exactly** and require one incident triangle from each side, at equal spacing
and 2:1 for 2/4m and 4/8m. The existing native noisy cave 2x2x2-block fixture
also runs unchanged, covering a split octree child, face/edge/corner handover,
no see-through seam holes and its existing fold allowances. These are preserved
native guarantees, not a claim that every arbitrary field produces a globally
manifold surface: surface-nets pinches and topology below coarse sampling remain
limitations. Native contour tracing can omit components that no coarse seed
reaches; this extraction does not invent a stronger guarantee.

Every requested boundary receives an owned UTF-8 canonical JSON resource at
the caller's `seam_resource` ID, or `terrain.face.N` by default (contract face
order: -X,+X,-Y,+Y,-Z,+Z). Resource IDs must be unique and cannot collide with
collision buffer IDs. The `contour-2m@1` payload contains:

| Field | Meaning |
| --- | --- |
| `version`, `face`, `origin_metres`, `tile_size_metres` | Version 1, contract face and explicit coordinate frame |
| `canonical_spacing_metres`, `sample_side`, `densities` | 2m lattice including both endpoints; tangential a fastest, then b. X face uses (Y,Z), Y uses (X,Z), Z uses (X,Y). Positive density is solid. |
| `sample_spacing_metres`, `neighbor_spacing_metres`, `neighbor_key` | Requested sampling/transition metadata |
| `native_plane`, `native_cell_layer`, `native_cells` | Original sparse surface-nets record; global cell indices a/b, world-double position, normal, native material ID and four plane-corner sign bits |

Native sparse cell records describe the underlying dual mesh, not the canonical
contour itself. Final shared contour segments are edges of the portable mesh;
the density lattice and metadata are available to independent seam consumers.
Contour mode has no overlap band or separately drawn skirt to publish.

## Collision ownership and controls

Requiring `matter.terrain.collision.mesh@1` adds a required extension with
`positions` and `indices` resource IDs, local bounds, frame and CCW winding.
`terrain.collision.positions` contains little-endian float32 XYZ triples;
`terrain.collision.indices` contains little-endian uint32 triangle indices.
They exactly reproduce the portable render mesh's geometry. Each payload has
its own immutable `BytesOwner`, SHA-256 and typed resource descriptor. A physics
consumer can retain these owners plus product origin/bounds/identity and release
all graphics/mesh owners independently. No solver object or acceleration
structure is created. Without this capability no collision bytes are allocated.
All-air/all-solid regions succeed with an empty asset and, when requested, empty
typed collision buffers. No degenerate placeholder triangle is emitted.

One invocation-wide `JobMonitor` covers preparation, meshing, conversion,
boundary sampling, validation and final publication. Conservative reservations
check queries, operation cost and working storage before the native call. The
reservation uses `v=(tile_cells+3)^3`, `f=6*(size/2+1)^2`, at most
`q=64*v+256*f` charged queries, `q*(field_sample_cost+64)` operations and
`2048*v+16384*f` working bytes plus the retained input. These deliberately
conservative accounting ceilings include native maps, triangle soup, output
buffers and JSON conversion; they are not measured peak process memory.
For a 32m/2m tile a caller can use 1e9 operations, 1e7 queries and 256 MiB
working storage, retaining its own output/vertex/index/time limits. Actual
vertices/indices and encoded output resources are checked before publication.

The unchanged native mesher has no internal cancellation hook. Its capped
2–32-cell, at-most-64m invocation is one bounded cooperative work unit, with
cancellation/deadline checkpoints immediately before and after it. Conversion
and boundary sampling have additional checkpoints. This does not promise
preemptive interruption or a hard wall-clock deadline inside the native call.
Progress reports `terrain.mesh.begin`, `.convert`, `.complete`, up to the
caller's event budget; callback cancellation/exception cannot publish a prefix.
All failures return no product and preserve the request envelope. The
coordinator's `admit_terrain` separately rejects stale epochs, revisions and
region/detail identity.

Additional fixtures in the same native wrapper/CTest suite:

| Fixture ID / CTest | Evidence |
| --- | --- |
| `terrain.mesh.render_collision.v1` | Public-only consumer; render/collision equality, separate ownership, stale-result rejection |
| `terrain.mesh.six_faces_and_lod.v1` | Signed faces at negative origins, exact matching samples and contour edges at 2/4/8m, selected 2:1 transitions |
| `terrain.mesh.controls_and_limits.v1` | Stage cancellation, callback exceptions, zero progress, budgets, domains, capabilities, precision, resource identity and empty output |
| `terrain.mesh_native_parity.v1` | Direct native versus portable noisy heightfield: exact positions, normals, triangle order, material IDs at all three supported spacings |
| `terrain.mesh_rollback_rejected.v1` | Separate process with native contour mode disabled; explicit unsupported result |
| `terrain.dependencies.v1` | Exact production CPU source set, contracts-only linkage, public includes and archive symbol audit |
| `terrain.legacy_terrain_mesher`, `terrain.legacy_contour_engine` | Original native mesher/noisy cave and transition suites, unchanged expectations |
