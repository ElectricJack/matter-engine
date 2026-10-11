# Optional physics-assisted generation

`matter_collision` owns version-1 collision descriptions and their strict JSON
codec. It links only `matter_contracts`. `matter_physics_generation` supplies
`settle`; its Box3d backend is opt-in (`MATTER_PHYSICS_BOX3D=ON`). The default
build returns `unsupported_capability` from generation while retaining baked
product loading. No public header contains solver, ECS, renderer, or host types.

The implementation compiles the existing `MatterEngine3/src/tileset_settle.cpp`
and `bake_trace.cpp` directly. It neither copies the algorithm nor links the
engine archive. The delivered contracts gate `0056e1576` is an ancestor of this
work. Legacy source authority, central manifests, ScriptHost/DslState, live
terrain installation, and the Settle Lab remain with the ordered integration
owners described in `docs/component-separation/ownership.md`.

## Contracts and capabilities

All products own their arrays. Units are metres, seconds, and kg/m³; poses use
RH Y-up coordinates and unit xyzw quaternions. Shape poses are body-local;
body poses are in the named generation region, without implicit parenting.
Semantic body IDs are distinct from shared shape IDs. The decoder validates
versions, required capabilities, references, finite numbers, meshes, poses,
materials, joints and resource limits before replacing its destination.

| Product or operation | v1 behavior |
| --- | --- |
| `matter.collision.product@1` | Sphere, capsule, oriented box, convex hull point cloud, indexed triangle mesh; static/kinematic/dynamic body descriptions |
| Capsule | Local +X core segment from `-half_segment` to `+half_segment`, plus radius; zero segment is a sphere |
| Joint descriptions | Distance and weld: two body-local frames, body IDs, collision flag, distance in metres; descriptions do not assert solver equivalence |
| `matter.physics.settle.box3d@1` | Native dynamic sphere/capsule/box/hull placement on a full toroidal heightfield; ordered layers, portal sync and optional final micro-relaxation |
| Unsupported generation | Triangle-mesh dynamics, joints, nonzero restitution, rotated portal occurrence frames, hulls over 64 source points, unavailable backend, unknown required capabilities |
| Invalid hull | Box3d preflight rejection; the legacy sphere fallback is explicitly forbidden at this boundary |
| Solver-state conversion | Always an explicit `unsupported_capability`, including requests naming the same solver; products carry descriptions, never solver snapshots |
| Browser | Solver-free descriptions/baked results are portable C++; the Box3d backend is native-only and WASM enablement fails at configure time; no browser conformance claim here |

The settling wrapper preserves the legacy timestep and tick/spawn order. It
uses bounded settings: torus 0.01–10000 m, dt 0.00001–0.1 s, 1–64 substeps,
per-layer duration up to 600 s, scale 0.01–100, and up to 1024 final relaxation
steps. Terrain must cover the torus exactly within a relative 0.0001 tolerance.
Body positions begin inside its X/Z domain. Inputs outside the documented
operating envelope fail instead of being clamped. See `src/settling.cpp` for
additional size/count/precision ceilings and diagnostics.

Calls check cancellation and resource budgets at simulation ticks and before
publishing. Final micro-relaxation is a bounded indivisible legacy operation;
cancellation is cooperative, not a hard wall-clock timeout. The working-memory
charge is an admission estimate, not an allocator-enforced hard cap. Calls in
this module serialize Box3d world creation/use/destruction. The application must
coordinate this with any concurrently invoked legacy Box3d entrypoints until
integration unifies their ownership. Progress callbacks run after that lock is
released. Failure or cancellation returns no partial result. Nonconvergence
at the time limit is reported as a warning and per-layer result, matching the
legacy behavior.

Each baked collision product records solver name, runtime version, source
SHA-256, compiler/build profile, backend version, canonical request SHA-256,
and all timestep/settings inputs (including gravity). The input digest includes
the collision definitions, ground samples, spawn velocities, layer ordering,
and sync frames. There is no promise of cross-platform, cross-compiler, or
cross-solver determinism. Distribute baked products when exact outputs matter.

## Asset storage and integration

Use `extension(product)` to obtain a required `matter.collision.product@1`
extension; add that capability to the containing asset's
`required_capabilities` and the reader's explicit supported capabilities.
`decode(extension, destination)` performs typed validation beyond the shared
contract envelope. The existing asset archive stores the extension without
linking Box3d. Consumers that do not support the required collision capability
must report it; do not silently mark semantic data optional.

Settled body IDs and poses are the placement output. An application maps these
IDs to its own instances, preserving shared shape/prototype identity. This
module does not infer those mappings or install ECS bodies. The central
optional integration milestone owns live authoring/instance registration.

`MatterEngine3/src/component_adapters/collision/terrain.*` converts the existing
terrain candidate to an owned neutral `TerrainProduct` and back. Tile-local
vertices, indices, origins, sorted coordinates (signed 64-bit), empty tiles,
geometry/installation keys, tile digests and materials retain their exact
values. Derived cache/timing statistics are not portable; reverse conversion
recomputes triangle, vertex and empty-tile counts. The adapter never accesses
renderer residency or changes the current terrain collision consumer. Native
integration may link `matter_collision_terrain_adapter` and include `terrain.h`.
TerrainProduct is an in-memory adapter contract; the existing terrain artifact
format remains authoritative for terrain persistence.

Do not link the legacy settle implementation twice in an integrated binary:
use `matter_settle_native` as the single owner when central manifests switch
over. Existing `matter_box3d` is reused when supplied by the parent build.
These targets are registered solely by this local CMake project; no central
build or runtime registration has been changed by this extraction.

## Validation

Supported native build, from PowerShell at the repository root:

```powershell
libs/AssetPipeline/physics/build-windows.ps1 -Matrix
libs/ProceduralGeometry/build-windows.ps1 -Test
libs/MatterGraphics/build-windows.ps1 -Test
```

From WSL, run each with the same wrapper pattern:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD/libs/AssetPipeline/physics/build-windows.ps1")" -Matrix
```

`-Matrix` configures and tests solver-on and solver-off builds with the
repository's resolved MSVC toolchain. It then loads the actual solver-on
`settled.asset` from the solver-off executable and rejects forbidden solver,
ECS, host or renderer sources in that build graph. `-Solver -Test` runs the
backend fixtures alone; `-Test` defaults to solver-off. Compile parallelism is
two and tests are serial. Build products stay under the ignored local `build/`.

| Fixture ID | Acceptance |
| --- | --- |
| PHYS-PRODUCT | Exact canonical round trip of all supported descriptions and joints; shared IDs retained |
| PHYS-UNSUPPORTED | Invalid version, unknown capability/shape/joint, missing references, nonfinite values, invalid meshes and conversion diagnostics; atomic decode |
| PHYS-PARITY | 30 seeded boxes: exact pose-component equality against the same-build legacy batch API, same-build repeatability, ground heights 0.03–0.20 m |
| PHYS-PORTAL | Two layers with reordered body identities and finalization; exact same-build legacy parity and synced X coordinates within 0.00001 m |
| PHYS-SHAPES | Box, sphere, capsule and convex hull resting heights within 0.02 m of 0.05 m |
| PHYS-WRAP | Moving sphere crosses +X edge and appears inside the torus |
| PHYS-FAILURE | Invalid request, duplicate/bad sync binding, failed hull, joints, cancellation, exhausted operations and callback exceptions publish no product |
| PHYS-TERRAIN | Exact geometry/material/identity round trip including coordinates above 2^53 and an empty tile |
| PHYS-BAKED / PHYS-INDEPENDENCE | Actual generated archive loads in solver-off build; provenance retained; graph excludes solver dependencies |

Historical platform-specific FNV goldens are not rebaselined. New extraction
parity is exact against the unchanged implementation compiled with the same
MSVC toolchain. This is extraction evidence, not native editor wiring,
performance qualification, or a cross-platform determinism claim.
