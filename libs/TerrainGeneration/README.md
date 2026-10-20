# Headless terrain fields, meshes and CPU products

`matter_terrain` owns the existing CPU field/noise interpreter,
`src/terrain_field.cpp`, which moved here from `MatterEngine3/src`; the engine
compiles the same file from this directory. The algorithms, integer noise hashes,
96-register limit, octave accumulation order and legacy FNV text identity have
one source of truth. Its production link dependency is `matter_contracts`;
private legacy includes are the engine's river-overlay, hydrology, bake-mode
and logging headers. The target also owns the original `src/terrain_mesher.cpp`
surface-nets and contour implementation; `src/seam_weld.cpp` lives beside it and
is compiled by the engine. The old `MatterEngine3/src` headers forward here. The target links no engine archive, QuickJS,
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
world and central build entrypoints. The module still reads those engine
headers privately, so it currently builds within the monorepo.

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

## Headless example package and independent consumer

Two example programs exercise the products end to end with rendering disabled.
Neither links a renderer, window, GPU, QuickJS, ECS or Matter world:

* `matter_terrain_example_tool <out_dir> [--fault <name>]` links
  `matter_terrain`. It generates three fixed 16 m contour tiles from one
  sloped-fbm field tape (`tests/examples/terrain_example_tiles.h`) and writes a
  self-describing package.
* `matter_terrain_example_consumer <package_dir> [--expect <code>]` links
  **only** `matter_contracts`. It reloads that package without the generator
  or field evaluator. It exits 0 on acceptance with no failed checks, 1 on a
  failed check and 2 on any other rejection. With `--expect <code>` it exits 0
  only when the package is rejected with exactly that contract error code.

| Region | Key | World origin (m) | Spacing / detail | Requested boundaries |
| --- | --- | --- | --- | --- |
| `terrain.example.west` | (-2,0,-1) | (-32,-16,-16) | 2 m / 0 | +X to east, equal detail |
| `terrain.example.east` | (-1,0,-1) | (-16,-16,-16) | 2 m / 0 | -X to west; +Z to north at 2:1 |
| `terrain.example.north` | (-1,0,0) | (-16,-16,0) | 4 m / 1 | -Z to east at 2:1 |

The seams lie on world x = -16 (equal detail) and z = 0 (2:1). Every tile has a
negative origin and requires `matter.terrain.collision.mesh@1` and
`matter.terrain.boundary.contour-2m@1`.

Package layout:

| Path | Content |
| --- | --- |
| `manifest.json` | Canonical JSON (`matter.terrain.example-package.v1`). It holds the contract profile, the required capabilities, the field identity (`terrain.example.field`, revision `source.1`, SHA-256 of the tape bytes), regions with key/origin/spacing/detail and the asset file plus semantic content digests, payload digests and lengths, both seams, tolerances, and ten field-height probes |
| `assets/<region>.json` | Encoded `AssetDocument`: mesh, material slots, typed `terrain` and `collisions` products, payload resource descriptors, the generator's required extensions and `matter.asset.terrain`/`collision` capabilities |
| `payloads/<sha256>.bin` | Collision positions/indices and seam records, named by content |

The consumer verifies the following, in order:
* the schema, profile and every required capability; unknown capabilities fail
  with `unsupported_capability`;
* every payload's SHA-256 and length, and every asset's file SHA-256 and
  semantic content digest (`invalid_input`);
* that provenance matches the package's field revision and content
  (`stale_result`);
* that placement matches the manifest.

It then rebuilds collision hulls from the typed buffers alone.

Exact standalone commands (Linux, from the repository root; the contour profile
needs `MATTER_CONTOUR_SEAMS=1`, and CTest sets it per test):

```bash
cmake -S libs/TerrainGeneration -B /tmp/matter-terrain-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMATTER_TERRAIN_BUILD_TESTS=ON
cmake --build /tmp/matter-terrain-build --parallel 2
ctest --test-dir /tmp/matter-terrain-build --output-on-failure -j 1
ctest --test-dir /tmp/matter-terrain-build -R '^terrain\.example\.' --output-on-failure -j 1
```

Production-only (no test targets), then generate and reload by hand:

```bash
cmake -S libs/TerrainGeneration -B /tmp/matter-terrain-prod -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DMATTER_TERRAIN_BUILD_TESTS=OFF
cmake --build /tmp/matter-terrain-prod --parallel 2
MATTER_CONTOUR_SEAMS=1 /tmp/matter-terrain-prod/matter_terrain_example_tool /tmp/terrain-pkg
/tmp/matter-terrain-prod/matter_terrain_example_consumer /tmp/terrain-pkg
# Each fault package must be refused with its exact code:
MATTER_CONTOUR_SEAMS=1 /tmp/matter-terrain-prod/matter_terrain_example_tool /tmp/terrain-stale --fault stale-source
/tmp/matter-terrain-prod/matter_terrain_example_consumer /tmp/terrain-stale --expect stale_result
```

Native Windows (PowerShell, from the repository root). The wrapper configures
with tests on; CTest supplies `MATTER_CONTOUR_SEAMS` to each test. Set
`$env:MATTER_CONTOUR_SEAMS = "1"` before running the tool by hand.

```powershell
libs/TerrainGeneration/build-windows.ps1 -Config RelWithDebInfo -Test
$env:MATTER_CONTOUR_SEAMS = "1"
libs/TerrainGeneration/build/windows-msvc/relwithdebinfo/matter_terrain_example_tool.exe $env:TEMP\terrain-pkg
libs/TerrainGeneration/build/windows-msvc/relwithdebinfo/matter_terrain_example_consumer.exe $env:TEMP\terrain-pkg
```

`MATTER_TERRAIN_BUILD_EXAMPLES` defaults on only when this directory is the
top-level CMake project. A parent project that adds it with `add_subdirectory`
gets `matter_terrain` alone unless it opts in. The examples' sources live under
`tests/examples/`, with the other fixtures, so the component-separation probe
sees no extra targets or unbuilt production sources.

### Fixture IDs

Each program prints `fixture <id>` before its checks; each CTest name is the
fixture ID it runs.

| Fixture ID | Program | Evidence |
| --- | --- | --- |
| `terrain.example.generate.v1` | tool | Three tiles validate against the product contract, encode with declared capabilities, and write content-addressed payloads and the manifest |
| `terrain.example.reload.v1` | consumer | Contracts-only decode; payload, file and semantic identities; provenance and placement |
| `terrain.example.collision_consumption.v1` | consumer | Typed collision buffers reproduce each render mesh exactly; the solver side needs no mesh or graphics owner |
| `terrain.example.seam_equal_detail.v1` | consumer | west\|east at x=-16: canonical seam densities equal; 9 shared contour edges, one incident triangle per side |
| `terrain.example.seam_two_to_one.v1` | consumer | east\|north at z=0, 2 m vs 4 m: densities equal; 10 shared contour edges |
| `terrain.example.mirrored_placement.v1` | consumer | Mirrored, nonuniformly scaled placement `T(16,0,0)·S(-1,1.5,1)` is a valid winding-reversing transform: every face flips relative to its inverse-transpose normal, and reversed winding restores it |
| `terrain.example.collision_probes.v1` | consumer | Collision ray heights versus independently sampled field heights at 10 columns (6 on seams); seam neighbours agree |
| `terrain.example.reproducible.v1` | CMake script | A second clean generation matches all 14 package files byte for byte |
| `terrain.example.fault_asset-capability.v1` | consumer `--expect unsupported_capability` | north's asset requires unknown `matter.terrain.example.future@1` |
| `terrain.example.fault_package-capability.v1` | consumer `--expect unsupported_capability` | The manifest requires the unknown capability |
| `terrain.example.fault_stale-source.v1` | consumer `--expect stale_result` | north was generated from an older field edit (`source.0`) |
| `terrain.example.fault_tampered-payload.v1` | consumer `--expect invalid_input` | One byte of a seam payload flipped after generation |
| `terrain.example.coordinator_adjacent.v1` | coordinator tests | Three tiles generated concurrently and completed in reverse order through `TerrainCoordinator`; published products keep source identity, separate collision leases and exact seams |
| `terrain.example.coordinator_late_completion.v1` | coordinator tests | Source edit, eviction and demand withdrawal while work runs: late results are `stale_result`, nothing is published, credits return, one regeneration follows; a held published owner survives later edits |
| `terrain.example.coordinator_resolution_change.v1` | coordinator tests | north refined 4 m to 2 m and back with east's boundary updated; the restore is a cache hit with byte-identical geometry rebound to the current job; a 4:1 neighbour is refused at completion with `unsupported_capability` |
| `terrain.example.coordinator_faults.v1` | coordinator tests | Unknown required capability at define; unknown required product extension; graphics demand without a service; throwing backend then retry; tampered seam bytes; cross-region receipt; cancelled before running; operations budget below the sample count (`resource_limit` at define); elapsed deadline (`resource_limit`, nothing published) |

`tests/coordinator_integration_tests.cpp` drives the contracts-only adapter
`MatterEngine3/src/component_adapters/terrain_coordinator` over the CPU service.
It is compiled from its source directory, without the adapter's own tests. It
needs no Matter world, renderer or script host.

### Emitted artifact identities

These are the identities from `terrain.example.generate.v1` and the
production-only Release build above. All 14 package files are byte-identical
across GCC 13.3 Debug and Release (x86-64 Linux) and MSVC 19.44 RelWithDebInfo
(x64 Windows, through `build-windows.ps1`), each generated from a clean
directory. Cross-target noise bytes are not promised identical in general (see
above), so another compiler or target must check reproducibility itself and
record its own identities.

| Artifact | SHA-256 | Size / count |
| --- | --- | --- |
| `manifest.json` | `7e7cc5c5d36ca6d9f80404465c97bf0689295034fb1b17bb806ad5d839d01935` | 14 package files |
| `assets/terrain.example.west.json` | `d18e83b2f6a2e8f4b61517995de8dbb99aee0c3b1eb9f3e1c8890e89c3d60b6a` | content `5713c6ea59f5fd1dd305f1fbe1569d6076794e251f38814d3e89c12cbb58ab33`, 198 triangles |
| `assets/terrain.example.east.json` | `a2131935f3ea7ae7e4017f3f8146012ca98801fb770431ee5fa2c09e84dfde26` | content `b3165174d117fd84bbffb0dbcb1ca08258ff1d86024a31d4204021c31ccafddd`, 204 triangles |
| `assets/terrain.example.north.json` | `15a06b858525149a71bce0d1a42f072f26575848b1a73615c09922ccd4483a06` | content `74543a2fb89a763fe91ff68bc59e136a8927de2446f34281dd0cdb98d65f916b`, 76 triangles |
| west `terrain.collision.positions` | `971c7d832e603950032bbce7634f9d72afeebc7d0bb4ed3a1353d90b51cb7980` | 7128 B |
| west `terrain.collision.indices` | `8c233ed01a52148a9b56872edff6f81109b460a1c6efab27e6035f0279e4f180` | 2376 B |
| west `terrain.face.1` | `d2b8f0dff3f3acb30fdedb4eb01457da059fe1258b9816faee6f72259206511b` | 2069 B |
| east `terrain.collision.positions` | `3a4eb92d22e5195e1f08e0b53c9c36175a5ee2697574e5fb5f34e8241af9c3e3` | 7344 B |
| east `terrain.collision.indices` | `aa5c6d8787dfa12d5181436f0e58da96c817ced66ae13a8972e58c22ee8365a4` | 2448 B |
| east `terrain.face.0` | `ad62d2bad49547973cae90b4bdbc278d98e3807b95831564c07275f7fb3fb6be` | 2069 B |
| east `terrain.face.5` | `b9e7effe9e61dad95eb2f509eec7ac00d2e73098aa1e0bc0dcc01ef5553dd44a` | 2032 B |
| north `terrain.collision.positions` | `4a36570f49d60b2a4d3cbd460f77334c4d6a1dba9018761b7db5b977b1964d0a` | 2736 B |
| north `terrain.collision.indices` | `3bb184617389890e24fb8e79be17fe6373934442fdd75bf94ec91318425ae533` | 912 B |
| north `terrain.face.4` | `04ad0adfe80ddaa49b6db0496b41b095e478ea3faf02819dba45e5c4feac3fdc` | 2031 B |

Recorded tolerances (in the manifest, never loosened to pass):

* Collision height versus field height along a vertical ray:
  `0.1 × sample spacing`. The measured worst case is 0.156 m on the 4 m tile
  (0.039 × spacing). The largest ratio is 0.058 × spacing, on east at 2 m.
* Collision height between seam neighbours: `0.0001 m`. They agree exactly.
* Seam densities and shared contour edges: exact equality.

### Integration notes

* The field sampler accepts only field capabilities. A height probe therefore
  uses a field-stage request (`matter.terrain.generate@1`, no boundaries), not
  the meshing request.
* `validate(TerrainRequest)` refuses an operations budget below the sample
  count with `resource_limit` when the coordinator defines the region. A budget
  exhausted during execution surfaces through the deadline/budget checkpoint at
  completion.
* An unsupported neighbour ratio (4:1) passes `define`. The service then
  refuses it at completion with `unsupported_capability`, and nothing is
  published. A live world should keep neighbours within 2:1 when it schedules
  detail.
* When rebuilding a `TerrainResult` from a product, capture the envelope first:
  `success(product.job, std::move(product))` has unspecified argument
  evaluation order and can read a moved-from job.
