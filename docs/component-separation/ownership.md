# Component and central-file ownership

Contracts v1 names and directories are adopted without adjustment. These are
extraction destinations; they do not all exist at the pinned commit. Each track
owns its new directory, public interface, implementation, focused tests and
local build file. Delivered neutral contracts and the public-consumer gate
precede parallel extraction. See the copied specification for ABI and schema
details; this map does not relax them.

## Fixed modules

| Target / integration | Owned directory | Allowed direct dependencies / responsibility |
| --- | --- | --- |
| `matter_contracts` | `libs/MatterContracts/` | Standard library and neutral MathLib types. Versioned recipes, assets, transforms, materials, diagnostics, budgets, capabilities, terrain extensions and immutable ownership. No QuickJS, Flecs, renderer, filesystem or window types. |
| `matter_geometry` | `libs/ProceduralGeometry/` | Contracts and audited CPU algorithm libraries. Primitive/explicit meshes, extrusion, ordered CSG, transforms and selected modifiers; no renderer, host or world services. |
| `matter_authoring` | `packages/matter-authoring/` | JS standard language and injected host interfaces. Portable recipe construction, validation and deterministic authoring; shared SDK for browser and native adapter. |
| `matter_host_quickjs` | `MatterEngine3/src/script/portable_host/` | SDK adapter, QuickJS and contracts. Replaceable native evaluation host; never a dependency of geometry/contracts/graphics. |
| `matter_build` | `libs/ProceduralBuild/` | Contracts and injected authoring, geometry, resolver and cache interfaces. Assembly DAG, job/cancellation coordination and cache/source identity. |
| `matter_graphics` | `libs/MatterGraphics/` | Contracts, platform/backend and shader libraries. Uploads, resource handles, drawing, culling and graphics-derived acceleration/residency. It consumes assets; it does not invoke generators. |
| `matter_terrain` | `libs/TerrainGeneration/` | Contracts, geometry and audited field algorithms. Fields, chunk sampling, meshing, boundary metadata and transitions; independent of streaming/rendering. |
| `matter_asset_pipeline` | `libs/AssetPipeline/` | Contracts, geometry and explicitly selected processing modules. LOD/chart/export and optional products; processing never becomes an implicit basic-prop dependency. |
| `matter_geometry_c` | `bindings/geometry-c/` | Geometry and contracts. Opaque handles, version/size-tagged C structures, status and explicit release; no STL/exception/QuickJS ABI exposure. |
| Browser integration | `integrations/web-procedural/` | SDK, C/WASM wrapper and independent renderer adapter. Copies/transfers own typed arrays; WASM heap views cannot outlive growth/release. |
| Matter coordination adapters | `MatterEngine3/src/component_adapters/` | Public interfaces of connected modules. Native conversion, service wiring, request/revision/epoch admission and world/renderer lifetime policy. |

Native optional animation, collision/physics, hydrology, GPU compute, GI,
surface/material processing, remeshing and export retain their existing source
authority until individually extracted. The optional-processing track defines
versioned interfaces and local targets in its assigned modules; integration
task `clear-crest-48.9.4` owns registration and compatibility entrypoints.
This baseline reserves no invented directory or target for an unimplemented
optional component. The full preservation inventory is in [inventory.md](inventory.md).

First-party MathLib, spatial-query algorithms, MeshChartingLib, MemoryLib and
ProfileLib are separately audited dependencies. AssetStoreLib/storage, Flecs
world state, Box3d, ozz and platform/device services remain behind appropriate
native adapters. A transitive dependency still counts: linking a legacy Matter
archive into a new component does not satisfy independence.

## Central files: one assigned writer per milestone

The task IDs below come from the supervisor's live graph on 2026-10-10
(message provenance in `baseline.json`). This assigns existing shared files
to ordered integration work, not to parallel track workers. A milestone edits
only its relevant slice and uses delivered prerequisite commits. No worker
may independently append to a shared source list while another owns it.

| Existing central surface | Assigned writer and handoff |
| --- | --- |
| `MatterEngine3/src/script_host.cpp`, `script_host.h`, `dsl_state.*`, `dsl_bindings.*`, native Part/World host registration and bake entrypoints | `clear-crest-48.9.1`: native prop/CSG/assembly adapters. Retain uncovered legacy verbs and native features. Terrain changes later belong to `.9.3`, optional capability wiring to `.9.4`, limited to their delivered milestones. |
| Native prop renderer entrypoint `MatterEngine3/src/render/vk_scene_renderer.*`, renderer asset conversion/PartStore entrypoints and `MatterEditor/src/main.cpp` | `.9.1` connects public graphics/geometry interfaces. `.9.3` later owns terrain admission/eviction; `.9.4` optional products. Graphics track prepares new local implementation and adapters without concurrent entrypoint edits. |
| `MatterEngine3/src/matter_engine.cpp`, `MatterEngine3/include/matter/world_session.h`, `src/sector_streamer.*`, `src/streaming/sector_streaming_coordinator.*`, `src/provider/local_provider.*`, `src/provider/resolvers.cpp`, terrain collision/render publication | `clear-crest-48.9.3`, after native/browser integration prerequisites (I1/I2). Separate cache, collision and graphics consumers; own revision/epoch admission and eviction lifetimes. |
| Optional registration, host/export/CLI bindings, animation/physics/hydrology/compute compatibility glue and native capability declarations | `clear-crest-48.9.4`. Complete every native inventory row with tested evidence or an explicit user-approved scope change. Unapproved gaps block completion. |
| Root `CMakeLists.txt`, `cmake/MatterEngine.cmake`, `MatterViewer.cmake`, `MatterEditor.cmake`, `MatterTargets.cmake`, `MatterThirdParty.cmake`, `MatterShaders.cmake`, and `cmake/manifests/*.sources`; existing root `platform.mk` and per-project Makefile glue | Milestone `.9.1` owns initial native source-list/registration changes; `.9.3` owns subsequent terrain changes; `.9.4` owns optional changes. Parallel modules own only their local build files. These central slices must be serialized rather than independently edited across tracks. |
| `CMakePresets.json`, compiler/toolchain/packaging and optional SDK CMake modules; `tools/build-windows*`, `tools/windows/`, `tools/ci-windows.ps1`, source-manifest checker, `.github/workflows/native-windows.yml` | `clear-crest-48.9.5` owns component matrix/dependency enforcement and final central build integration after module delivery. Coordinate with the separate promotion CI task before editing shared workflows. Do not change branch protections, AQ rollout or project configuration. Earlier native integration must preserve supported wrappers and package behavior. |
| Existing tests' central CTest/Make registration | Relevant integration milestone (`.9.1`, `.9.3`, `.9.4`), then `.9.5` for the full matrix. Local module tests/registration may be prepared by their track. Existing GPU tests stay serial. |
| Superseded algorithm paths and their final obsolete central links | `clear-crest-48.9.7`, only after qualification. Map each removed path to its single replacement, retain supported forwarding APIs and history, and re-run native packaging plus the component matrix. |
| `docs/component-separation/` and this baseline ownership map | `clear-crest-48.1.1` owns this initial record; later gate/qualification tasks append evidence without rewriting the pinned baseline or source provenance. |

Source paths within each row that belong to the current monolith remain shared
until an integration handoff assigns their final implementation authority.
The existence of a destination above does not authorize moving/deleting its
legacy counterpart ahead of the delivered gate. If two tasks need the same
central file concurrently, their supervisor orders or explicitly reassigns
the slice; this map cannot grant an unheld task.

## Gate handoffs

1. The contract gate implements immutable recipe/asset schemas and validation,
   capability diagnostics, coordinate conventions, ownership/release and
   bounded/cancelled evaluation. It compiles public headers alone and runs a
   native consumer using fake resolver/cache/authoring/geometry services.
2. It publishes the source/fixture map, definitive dependency rules and
   native/WASM conformance fixtures with stated tolerances. Delivery of this
   documentation alone is insufficient to release parallel extraction.
3. Tracks deliver isolated libraries and local fixtures. Native integration
   preserves all inventory rows through explicit adapters and uses the
   selected small prop/CSG/shared-assembly corpus first.
4. Terrain and optional integration follow their live dependency gates. CI
   verifies contracts-only, geometry-only, graphics-only, terrain-only, full
   native and WASM with disabled components absent from the link graph.
5. Qualification records correctness and performance separately; retirement
   occurs only after compatibility, independent builds and packaging pass.

Namespaced legacy FNV cache identity stays distinct from the specified portable
digest. No cache relabeling, tolerance loosening, shader fixes, promotion changes
or source moves are part of this baseline task.

## Service scaffold handoff

Task `clear-crest-48.1.3` adds the [machine-readable local target manifest](../../libs/MatterContracts/component-ownership.json)
and [public service/ABI ownership rules](../../libs/MatterContracts/SERVICES.md).
These append to the baseline without changing its pinned provenance or central
writers. Native module targets are explicitly marked interface-only scaffolds;
their configure/consumer checks do not establish extraction or performance
acceptance. The delivered shared contract gate remains prerequisite to tracks.
