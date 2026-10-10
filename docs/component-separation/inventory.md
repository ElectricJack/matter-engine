# Source and native capability inventory

Inspected source: `98af23851dd3424ddbfc62624cd2afb850e9d09d`.
Paths below identify existing implementation authority, not permission for a
parallel worker to edit a central file. Follow [ownership](ownership.md).
Compile shared algorithms from their source of truth; do not copy or symlink
sibling implementations into a new module.

## Props and geometry

| Current source | Behavior and dependencies | Extraction destination |
| --- | --- | --- |
| `MatterEngine3/src/triangle_emit.hpp`, `src/dsl_state.cpp`, `src/dsl_bindings.cpp` | Direct triangles, primitive meshes, polygon triangulation and profile extrusion. The emitter uses SpatialQueryLib `Tri`/`TriEx`; state/bindings also bring QuickJS, terrain, material and animation state. | CPU algorithms → `matter_geometry`; portable recipe construction → `matter_authoring`; legacy calls → host/coordinator adapters. |
| `MatterEngine3/src/csg_lowering.cpp`, `src/dsl_state.h` (`BuildBuffer`); `libs/MatterSurfaceLib/include/fat_primitive.h`, `include/csg_stages.h`, `src/surface.c`, `src/cell.cpp`, `src/cluster.cpp`, `src/mesh_worker_pool.cpp` | Ordered SDF union/difference/intersection, typed primitives, marching cubes and cell work. Surface C sources depend on MemoryLib/SpatialQueryLib, MathLib C mirrors, spatial hashing and marching-cubes tables. Cell/worker wrappers include legacy geometry/resource types. | Audited CPU lowering/meshing → `matter_geometry`; scheduling and publication stay above it. |
| `MatterEngine3/src/modifier_apply.cpp`, `src/retopo_blacklist.cpp`; `libs/MatterSurfaceLib/src/mesh_simplifier.cpp`, `src/mesh_smooth.cpp`, `src/mesh_transform.cpp`, `src/mesh_retopo.cpp` | Ordered simplification, smoothing and optional remeshing; TriEx reprojection. Retopo uses `third_party/autoremesher_core`, its Eigen/geogram dependencies and TBB shim, serialized native state, and a blacklist journal. | CPU modifiers → geometry; blacklist persistence is injected; optional remeshing needs its own target audit. |
| `libs/SpatialQueryLib/src/bvh.cpp`, `src/bvh_analyzer.cpp`, `src/spatial_hash.c`; `MatterEngine3/src/world_tracer.cpp` | CPU acceleration/spatial queries; the world tracer also consumes Matter assets. Distinguish CPU structures from MatterSurfaceLib GPU BLAS/TLAS managers. | Asset-local CPU queries → geometry; world queries → coordination adapter. |
| `MatterEngine3/src/script_host.cpp`, `src/solid_source_js.h`; `include/matter/solid_sdf_meshing.h` | QuickJS evaluation already has owned solid-source output and cancellation; GPU ABI is a backend lowering, not the neutral recipe. ScriptHost also owns meshing, LODs, files, animation and renderer publication. | Replaceable `matter_host_quickjs` plus explicit geometry/processing adapters. |

Real prop inputs are `objects/castle/materials/CastlePavingSlab.js`,
`CastleStone.js` and `objects/vegetation/alpine/AlpineFlower.js` under
`projects/world_demo/`. Their transitive authoring libraries include project
`shared-lib/castle_paving.js`, `castle_primitives.js`, `vegetation.js` and
the fallback `MatterEngine3/shared-lib/rng.js`.
The existing export goldens provide actual native artifact expectations; stone
is a fine-SDF stress fixture, not the minimal smoke prop.

## Assemblies and build coordination

| Current source | Behavior and dependencies | Extraction destination |
| --- | --- | --- |
| `MatterEngine3/src/part_graph.h`, `src/part_graph.cpp`, `src/part_graph_snapshot.cpp`, `src/module_resolver.cpp` | Resolver/baker injection, children-first DAG, prototype deduplication, cycle diagnostics, transitive shared-lib folding and legacy content hashing. Discovery/cache operations currently use filesystem and Matter schemas. | `matter_build` with injected source, cache and authoring services. |
| `MatterEngine3/src/world_flatten.cpp`, `src/part_flatten.cpp`, `src/part_cluster.cpp`, `src/lod_bake.cpp` | Hierarchy placement, transforms, cluster partitioning, LOD/error ladders and attributes; flattening currently writes Matter artifacts and builds legacy BLAS data. MeshChartingLib handles charts/atlas packing. | Neutral assembly/processing → build/asset pipeline; Matter artifact and renderer conversion → adapters. |
| `MatterEngine3/src/async_bake.cpp`, `src/resolve_cache.cpp`, `src/authored_world_cache.cpp` | Bounded jobs, cache identity, cancellation and publication; tied to Matter world/session and provider state. | Service-injected build jobs; stale admission/world lifetimes remain coordination-owned. |
| `projects/world_demo/shared-lib/castle_plan.js`, `castle_structure.js`, `castle_structure_fixture.js`; `tests/fixtures/castle_plan_two_room_two_level.js` | Deterministic small two-level structure and repeated pieces; emitted Part verbs/child placements rather than portable recipes today. | Portable SDK compatibility slice with exact source/parameter identity. |

`part_graph_tests` has Root/P1/P2/Bolt sharing (four unique artifacts) and a
cycle that bakes nothing. This is a smaller native assembly oracle than a full
castle scene. Current fake resolver/baker tests do not yet prove the new public
contracts compile independently.

## Graphics

| Current source | Behavior and dependencies | Extraction destination |
| --- | --- | --- |
| `MatterEngine3/src/render/vk_scene_renderer.cpp`, `vk_context.cpp`, `vk_resources.cpp`, `raster_mesh.cpp`, `vk_instance_cache.cpp` | Native Vulkan allocation/upload/frame orchestration; renderer entrypoints also consume PartStore/provider/world data. | `matter_graphics` internals; live entrypoints changed by graphics integration only. |
| `MatterEngine3/src/render/part_store.*`, `raster_cull.h`, `vk_blas_cache.h`, `indexed_part_geometry.*`, `vertex_cache_order.h`; `libs/MatterSurfaceLib/src/blas_manager.cpp`, `tlas_manager.cpp` | Geometry conversion, static/indexed buffers, culling, acceleration structures and instance lifetime. `BakedGeometry` is renderer/resource data, not a portable result. | Graphics-owned derived resources; portable upload conversion in adapters. |
| `src/render/vt_*`, `src/render/gpu_meshing/*`, `MatterEngine3/shaders_vk/` | VT material production/residency/feedback, POM, finite/periodic surfaces, GPU generation, raster/RT/GI/water/sparse-voxel effects. GPU generation must get a separate compute interface if extracted. | Graphics material/backend modules; selected optional compute backend below scene/frame policy. |
| `MatterEditor/src/main.cpp`, `viewport_capture.*`, `asset_root.*` | GLFW/editor, ImGui/ImGuizmo, asset loading and calibrated identity/depth/normal captures. | Application/editor; preserve new capture and working-directory asset-root behavior. |

Dependencies today: Vulkan SDK/headers/loader and `glslc`, GLFW sources from
raylib's vendored external tree, ImGui/ImGuizmo for the editor, `bc7enc` for
compressed material products, CPU spatial/math/profile libraries. Raylib POD
headers remain in the Matter build; **no OpenGL renderer/windowing runtime is
the current product path**. Streamline/DLSS is an optional external SDK backend.
The root CMake graph generates embedded SPIR-V and text shader tables even for
the existing headless archive. That archive still links QuickJS, Flecs, Box3d,
ozz and BC7: its name is not evidence of geometry independence.

## Terrain and optional native domains

| Current source | Behavior and dependencies | Extraction destination |
| --- | --- | --- |
| `MatterEngine3/src/terrain_field.*`, `terrain_river_overlay.*` | Canonical field/surface/habitat tapes, deterministic integer-hash noise, heights/densities/material/ecology lanes, height caches and optional river overlays. CPU surface tape has a GPU twin in `shaders_vk/vt_surface_tape.glsl`. | `matter_terrain`; GPU material twin remains explicitly graphics/backend-owned. |
| `MatterEngine3/src/terrain_mesher.*`, `seam_weld.*`, `seam_boundary.h`, `sector_grid.*` | Surface nets, tiled volumes, rung/region sampling, boundary geometry and seam correction. Mesher currently reads shared bake-mode policy; contract extraction must make policy explicit. `contour_seam_tests.cpp` is an independent analytic foundation harness, not a production mesher. | Terrain algorithms and versioned boundary extension; live sector ownership remains coordination. |
| `MatterEngine3/src/provider/local_provider.*`, `provider/resolvers.cpp`, `sector_streamer.*`, `streaming/sector_streaming_coordinator.*`, `matter_engine.cpp`; `MatterEngine3/include/matter/world_session.h` | Demand, streaming, source fallback, job epochs, result admission, caches and renderer publication. WorldSession implementation is in `matter_engine.cpp`. | Matter coordination adapters/integration. |
| `MatterEngine3/src/geometry/`, `render/geometry_world_runtime.*` | Hierarchy compilation/page caches/residency versus renderer world admission. VG stays opt-in (`MATTER_GEOMETRY_PAGES=1`); retain complete-parent/source fallback and the inert disabled path. | CPU hierarchy processing/geometry; store and renderer adapters separately. |
| `MatterEngine3/src/export/`, `src/gi_bake.cpp`, `gi_bake_scene.cpp`, `gi_bake_image.cpp`, `cli/` | Existing static GLB/image and OBJ/MTL/PBR export; CPU GI lightmaps and CLI. | Asset pipeline/export adapters; GI is a separate processing capability. |
| `MatterEngine3/src/animation/`, `src/hydrology/`, `src/terrain_collision/`, `src/ecs/`; `integrations/physx_adapter/`, `libs/ParticleFlowLib/` | Native rigs/skinning/clips via ozz, particle flow, Box3d/ECS, collision, hydrology and optional PhysX/CUDA bake backend. | Versioned optional products/services; never mandatory dependencies of a basic prop. |
| `libs/AssetStoreLib/`, `libs/MemoryLib/`, `libs/ProfileLib/`, `libs/MathLib/`, `libs/MeshChartingLib/` | Binary pages and ref tables, allocators, instrumentation, neutral math and charts. These are first-party libraries. | Injected storage, audited algorithm dependencies, neutral math and optional instrumentation. |

## Third-party and platform boundary audit

The current build definitions are `cmake/MatterThirdParty.cmake`,
`MatterTargets.cmake`, `MatterViewer.cmake`, `MatterPhysx.cmake` and
`MatterStreamline.cmake` (hashed in `baseline.json`). Keep the tracked vendor
versions/licenses with extracted consumers; audit actual headers and link
dependencies rather than treating an old umbrella archive as a CPU dependency.

| Current dependency / location | Current use and intended boundary |
| --- | --- |
| `third_party/quickjs-ng/` | Native JS execution/module loading; host adapter only. Browser SDK and native/WASM geometry do not require it. |
| `third_party/raylib/src/raylib.h`, its `external/glfw/` | Legacy POD/math-facing declarations plus native GLFW sources. Replace PODs at neutral interfaces; retain windowing below graphics/editor. |
| `third_party/Vulkan-Headers/`, pinned external Vulkan SDK `C:/VulkanSDK/1.4.357.0` | Vulkan headers/loader, shader compiler and validation tools; graphics or explicitly selected compute backend, never neutral contracts. |
| `third_party/imgui/`, `third_party/ImGuizmo/` | Editor UI/gizmo only. |
| `third_party/bc7enc/` | Texture compression in material/export processing; optional to a basic mesh result. |
| `third_party/flecs/`, `third_party/box3d/` | Native ECS/physics; world and collision/placement adapters, outside basic geometry/graphics-only interfaces. |
| `third_party/ozz-animation/` | Native animation/offline bake and skin/clip products; explicit optional animation capability. |
| `third_party/autoremesher_core/` | CPU remeshing with Eigen/geogram code and local TBB shim; native viewer enables it by default. Isolate global state/serialization and persistence; separately audit browser support. |
| External Streamline/DLSS and PhysX/CUDA SDKs | Opt-in via `MATTER_ENABLE_STREAMLINE` / `MATTER_ENABLE_PHYSX` and supported wrappers. Neither SDK is installed/published by this task; basic independent configurations exclude them. |

Platform/device, allocator and storage services may have private implementations,
but their public boundary must preserve explicit ownership, diagnostics and
cancellation. A GPU compute handoff requires device compatibility and sync/
lifetime rules; it cannot be the only portable result path.

## Native compatibility and target matrix

Every row below is a native preservation obligation before legacy retirement.
"Required" means the consuming profile explicitly needs the capability; it
does not make the feature a transitive dependency of all targets. Browser
status is **planned/unverified** throughout this baseline.

| Capability | Native authority / focused fixture or test | Planned browser handling |
| --- | --- | --- |
| Mesh primitives, profile/extrusion, explicit meshes | `script_host_tests` extrusion/shape cases; `PROP-SLAB`, `PROP-FLOWER`, `GEOM-EXTRUDE` | Required first slice. |
| Ordered solid CSG, deterministic transforms | `script_host_tests`, `dsl_determinism_tests`; `GEOM-CSG`, `GEOM-TRANSFORM` | Required first slice; unsupported transform classes fail explicitly. |
| Shared assembly, material slots, seeded variation | `part_graph_tests`, `part_flatten_tests`; `BUILD-SHARED`, `PROP-FLOWER`, `GEOM-SEED` | Required first slice. |
| Simplify/smooth and attribute preservation | `modifier_apply_tests`, `mesh_simplifier_tests`, `staged_normals_tests` | Declare audited CPU subset; no silent modifier omission. |
| Optional autoremesher | `mesh_retopo.cpp` / native viewer default; tests without the flag only prove skip semantics | Unsupported until separately audited; skip semantics of the legacy path are not neutral API acceptance. |
| Spatial queries and staged evaluation | `world_tracer_tests`, `solid_source_evaluation_tests` | Bounded batched API required; no JS per-sample callbacks. |
| LODs, clusters, UV charts, export/PBR textures, GI bake | `part_flatten_tests`, `chart_atlas_tests`, `obj_export_golden_tests`, `asset_export_tests`, `gi_bake_tests` | Optional processing with declared capability and tested fallback/error. |
| Terrain fields, chunks, seams, surface/habitat classification | `terrain_field_tests`, `terrain_mesher_tests`, `contour_seam_tests`, `surface_field_tests`, `sector_bake_tests` | Required terrain slice after public contracts; transition tolerances set by terrain gate. |
| Raster, RT/GI, VT/finite/periodic materials, water, sparse foliage, optional DLSS | `vulkan_smoke_tests` modes and native scenes; `RENDER-NORMAL`, `RENDER-MOUNTAIN` | Independent renderer adapter; advanced material/representation conversions declared. |
| Animation/rig/socket/skin/clip products | `animation_dsl_rig_tests.cpp`, `animation/` and ozz targets | Optional versioned products; explicit unsupported result until implemented. |
| Collision, physics placement, particle flow, hydrology and water animation | collision/river/water tests registered in `cmake/MatterEngine.cmake`; optional `MATTER_ENABLE_PHYSX` | Optional native services, excluded from the initial portable prop. |
| Cancellation, finite budgets, diagnostics, stale rejection, ownership | `solid_source_evaluation_tests`, `async_bake_tests`, `geometry_hierarchy_tests` are existing analogs | New neutral public-boundary tests mandatory; see manifest gaps. |

The capability declarations and neutral contract tests must be implemented by
the contract gate, not inferred from legacy coverage. Unknown required features
must fail explicitly on either target. Cross-target geometry tolerances and
browser toolchain/device budgets remain gate deliverables; none were measured
or relaxed here.
