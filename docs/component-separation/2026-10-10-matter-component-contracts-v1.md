---
title: Matter component contracts version 1
kind: design
status: implementation_baseline
project: matter-engine-cpp
date: 2026-10-10
---

# Matter component contracts version 1

## Scope and authority

Implementation specification for the [component separation design](2026-10-10-matter-engine-component-separation-design.md). The user authorized contract preparation and creation of the full implementation graph on October 10, with the project kept paused and each parallel track represented by an epic. This document settles the initial interface decisions; the contract epic must implement and validate them before the six parallel extraction tracks start. This status does not claim compiled contracts or completed implementation.

Native C++ generation, browser JavaScript plus WASM generation, standalone graphics, and headless terrain are first-class consumers. QuickJS is a replaceable native host adapter. Keep one implementation for shared algorithms. A change that contradicts the graphics/generation separation requires a design decision, not an adapter shortcut.

## Module ownership

Proposed target and directory names are fixed for task ownership, subject to a single documented adjustment by the baseline task if current repository conventions require it. The contract gate publishes the definitive ownership map before parallel work.

| Module | Suggested owned location | Allowed dependencies |
| --- | --- | --- |
| `matter_contracts` | `libs/MatterContracts/` | Standard library and neutral MathLib types only |
| `matter_geometry` | `libs/ProceduralGeometry/` | Contracts and audited CPU algorithm libraries |
| `matter_authoring` | `packages/matter-authoring/` | JavaScript standard language and injected host interfaces |
| `matter_host_quickjs` | `MatterEngine3/src/script/portable_host/` | SDK adapter, QuickJS and contracts |
| `matter_build` | `libs/ProceduralBuild/` | Contracts and injected authoring, geometry, resolver and cache interfaces |
| `matter_graphics` | `libs/MatterGraphics/` | Contracts, graphics platform/backend and shader libraries |
| `matter_terrain` | `libs/TerrainGeneration/` | Contracts, geometry and audited field algorithms |
| `matter_asset_pipeline` | `libs/AssetPipeline/` | Contracts, geometry and explicitly selected processing modules |
| `matter_geometry_c` | `bindings/geometry-c/` | Geometry and contracts |
| Browser integration | `integrations/web-procedural/` | SDK, WASM wrapper and independent renderer adapter |
| Matter coordination adapters | `MatterEngine3/src/component_adapters/` | Public interfaces of the modules being connected |

Each module owns its local build file. Central CMake files, source manifests, `script_host.cpp`, `dsl_state.*`, and renderer/world entrypoint wiring belong to designated integration tasks. Track workers prepare adapters in owned paths. A compatibility edit to a shared file must be assigned and ordered explicitly; parallel authors must not independently refactor the same entrypoint.

## Common values and errors

- Units: metres, seconds and radians. Coordinate frame: right-handed, Y up. Positions are local to an explicitly named part or region.
- Matrices: row-major 16 float values, column-vector multiplication, translation at indices 3, 7 and 11. `parent * local` composes a placement. This matches the inspected MathLib contract; legacy raymath multiplication must be converted explicitly.
- Quaternions: x, y, z, w. Triangle winding: counterclockwise viewed from the front. Normals and tangent handedness must be corrected for mirrored and nonuniform transforms.
- Geometry storage: finite float32 positions and attributes, uint32 indices in v1. Large worlds use explicit region origins; do not silently lose world precision by baking huge coordinates into vertex buffers.
- Identifiers: stable semantic strings for authored objects and instances; separate opaque content digests for exact cache identity. A content digest never replaces an instance identifier.
- Diagnostics: severity, stable code, message, optional source URI/line/column, recipe node ID and dependency path. Codes include invalid input, unsupported capability/version, missing dependency, cycle, resource limit, cancelled, stale result and backend failure.
- A failed or cancelled evaluation publishes no valid partial asset. Explicit progress/previews are separate, noncommittable outputs.

All data entering a public boundary is validated. Reject nonfinite values, invalid indices, overflowing lengths, missing references and exceeded budgets before expensive evaluation where possible.

## Recipe contract

An owned `RecipeDocument` contains `schema_version`, `required_capabilities`, source identity, canonical parameters, an explicit seed, resource references, an ordered graph of nodes, output node IDs and source mapping. Each node has a unique semantic ID, operation version, typed inputs, parameters and material references. References must form an acyclic graph.

Initial operations cover primitive meshes, polygon profile/extrusion, ordered solid CSG, explicit mesh input, affine placement and assembly references. Geometry modifiers extend the operation set through declared capabilities. CSG ordering and transforms are explicit; do not reinterpret a flat ordered legacy brush list as commutative operations. Degenerate/singular transforms and unsupported CSG transform classes receive documented diagnostics.

The contract is data, without JavaScript closures, C++ pointers, graphics handles or filesystem paths with implicit resolution rules. Geometry evaluation receives all required resources through a supplied resolved input set. Imports and downloads are resolved by the build coordinator before submission. The first implementation may use an inspectable JSON recipe encoding plus typed buffer resources; the in-memory contract is not bound to a GPU ABI or final package format.

Parameter normalization specifies sorted keys, numeric representation, seed semantics and dependency versions. Reuse the established seeded generator algorithm where compatibility permits; publish known-answer random sequences. Hash the canonical recipe with a collision-resistant digest for new portable artifacts. Keep legacy Matter source hashes explicitly namespaced and translated by adapters; do not invalidate existing caches by relabeling them.

Unknown required capabilities fail. Optional metadata must not alter geometry semantics. Additive changes carry feature/version declarations; incompatible meanings require a major contract version.

## Asset contract

An immutable owned `AssetDocument` contains a version, identity/provenance, bounds, mesh prototypes, material definitions, texture references, instances and optional typed extension products.

`MeshPrototype` contains positions, indices, optional normals/tangents/UV sets, submesh material bindings and bounds. An `Instance` references a prototype or assembly, has its own semantic ID, optional parent ID and local transform. Validate parent graphs and distinguish instance identity from shared prototype identity. A repeated assembly must retain sharing.

The material baseline is opaque metallic/roughness PBR: base color, metallic, roughness, normal and emissive inputs with explicit texture color spaces and sampler conventions. Transparency or advanced Matter surface representations use optional declared capabilities. An adapter must supply a tested conversion/fallback or a clear unsupported diagnostic; silent visual loss is unacceptable.

Attachments, collision descriptions, LOD representations/error metrics, rigs, skin weights, animation clips and terrain boundary data are versioned optional products. Their contracts have no solver, ECS or graphics resource types. Preserve current supported native features through optional modules; initial browser support can be narrower and must expose that capability matrix.

BLAS/TLAS structures, device buffers, renderer policy, cache-write state and live world state are derived consumer data outside this contract.

## Geometry and job interfaces

Conceptual interfaces, with exact declarations delivered by the contracts epic:

```text
evaluate(recipe, resolved_resources, options, control) -> AssetResult
query(asset, query_batch, options, control) -> QueryResult
process(asset, processing_request, control) -> AssetResult
```

`options` identifies algorithm versions and finite resource ceilings: operation count, recursion/dependency depth, vertices, indices, output bytes, working memory and execution budget. `control` supplies cooperative cancellation and bounded progress reporting. Native synchronous evaluation has no dependency on a scheduler; the host runs it in its chosen worker/process. Browser work runs in a worker and may be terminated as a last resort, after which all handles for that worker are invalid.

Staged generation is supported: evaluate geometry, query batched surface positions/normals, then construct another recipe. Do not require a callback into JavaScript per vertex or sample. Field evaluation uses a portable field program or an explicitly selected native backend, with clear unsupported-target behavior.

The coordinator stamps requests with an opaque request ID, source revision and generation epoch. Results echo this envelope; admission compares all three before publishing. Algorithms do not decide whether a world revision is still current.

## Memory and C binding

Native C++ APIs return owned immutable results. Borrowed input spans live through the synchronous call. Async host APIs retain/copy inputs until completion and release them on all exits. Ownership is explicit for every buffer.

The C ABI uses versioned structures with size fields, fixed-width scalar values, opaque handles, stable status codes and explicit release functions. No STL type, C++ exception, QuickJS value or renderer object crosses the ABI. v1 uses library-owned result storage released by the library; custom allocators are an optional later capability.

The browser wrapper initially copies/transfers results into JavaScript-owned typed arrays at the worker boundary. Borrowed WASM heap views may not survive another allocation, memory growth or result release. Multiple library instances do not share handles. Repeated evaluation, failure, cancellation and disposal must be exercised for leaks and use-after-free errors.

## Host and build coordinator

The SDK owns recipe construction helpers and explicit RNG state. Host adapters provide source/module resolution and execution control. No browser DOM, Node filesystem or QuickJS global is part of the shared SDK API. An authoring invocation consumes a frozen source/dependency snapshot; async module discovery happens before that invocation or through a documented host stage.

The build coordinator evaluates dependencies children-first, deduplicates prototypes and schedules bounded jobs. It receives resolver and cache interfaces. Cache keys include transitive sources, parameters, seed, contract/algorithm versions and processing settings. Renderer resource caches are separate. File export and cache persistence are explicit consumers after successful evaluation.

No graph edge or task depends on its containing epic completing. Cross-epic prerequisites refer to delivered leaf work so independently progressing tracks do not deadlock on container settlement.

## Graphics interface

Graphics accepts immutable asset uploads, instance/scene updates, camera/light state and resource releases. Upload completion and resource lifetime are explicit; deleting a scene instance does not necessarily delete its shared prototype. Graphics is responsible for device synchronization, culling, render passes and renderer resources.

Graphics may report representation demand, projected error or memory pressure. Those observations go to the world coordinator; graphics never calls generation or fetches source modules. Imported and synthetic assets exercise the same interface. A graphics-only executable must link with both procedural and terrain targets disabled.

Keep shader compilation, renderer material implementations, virtual-texture residency, ray-tracing structures and effects inside graphics/backend modules. Optional compute generation may share neutral device infrastructure but never the renderer scene or frame loop.

## Terrain interface

`TerrainRequest` specifies a frozen field/dependency identity, region key, local origin, sample domain, resolution/detail, boundary policy and job envelope. The terrain generator returns `TerrainProduct`: portable geometry, bounds, optional collision/field products, boundary descriptors and the request identity.

Boundary descriptors identify neighboring region coordinates, sample spacing and seam/transition data without containing resident sector pointers. The terrain epic defines algorithm-specific seam guarantees using the contract extension mechanism before implementing transitions.

World coordination owns demand, region lifetimes, cancellation, stale-result rejection and cache admission. Graphics owns residency of uploaded resources. Collision and navigation are separate consumers. Evicting graphics resources must not discard field definitions or independently needed collision products.

## Compatibility and acceptance fixtures

Mandatory initial fixtures: one primitive/extruded prop, ordered CSG subtraction, one repeated-child assembly, material assignment, mirrored/nonuniform placement, deterministic seeded variation, malformed recipe, resource exhaustion and cancellation. Terrain adds adjacent chunks, resolution changes and late completion after invalidation. Optional modules add their own representative native fixtures.

Recipe/metadata equality is exact for a pinned contract profile. Geometry is compared using topology rules and recorded absolute/relative tolerances with units; establish these before accepting cross-target results. Do not weaken tolerances to pass a regression. Graphics validation includes matched views and material/instance checks.

The contract gate must compile native consumers with only public headers, exercise validation and ownership with fake services, and publish the fixed file-ownership map, capability table, fixture IDs and exact test commands. Runtime stubs in this gate do not count as extraction or performance evidence. Downstream tasks require the gate's delivered commit, not an unmerged worktree.

## Integration and retirement

Parallel tracks own their component implementations. Integration leaves own changes to central manifests and live Matter wiring at ordered milestones. Tests use the repository's current supported Windows build wrappers for Matter and documented separate WASM tooling; do not copy Agent Queue's Python testing rules into this C++ project.

Compatibility expansion precedes removal. Retire duplicate legacy implementation only after all required native consumers and the portability matrix pass. Preserve a revertable commit boundary and fixture artifacts. Public package publishing, changes to AQ rollout policy, a new scripting engine, and commercial service work are outside this graph.
