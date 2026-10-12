# Production raster implementation

The subsequent water/temporal/DLSS source move and its retained native host
boundaries are documented in [WATER_TEMPORAL.md](WATER_TEMPORAL.md).

These files were moved from the production engine by `bright-torrent-52`.
The reference is commit `58289c187`. They preserve the native renderer's
algorithms, shader ABI, draw order, specialization options and ownership.
They do not implement the separate `clear-crest-48.4.5` raster API.

`raster_mesh.cpp`, `raster_mesh.h` and `impostor_mips.h` retain their existing
namespaces and types. The engine locations forward here. The mesh header's
one changed include names the existing `IndexedPartGeometry` contract;
welding and procedural impostor baking remain engine responsibilities.

The four `.inl` files are **private implementation fragments**, included once
at their original lexical positions in `VkSceneRenderer`. They deliberately
share the host's existing resource ownership, helper functions, feature macros,
profiling scopes and fault-injection definitions. They are not compiled as
additional translation units or exposed as a standalone graphics API.

| Fragment | Host boundary and dependencies |
| --- | --- |
| `record_gbuffer.inl` | Inside `record_raster`, borrowing its `RasterRecord`. Uses existing image transitions, timestamps, draw types, indirect-run helper and profiling. VT pre/post hooks and sparse draws remain calls into their existing owners. Leaves the existing viewport/scissor and attachments in scope for composition. |
| `create_raster_pipelines.inl` | Inside `create_raster_pipelines`, borrowing the device, pipeline layout and output handles. Uses existing shader-module, pipeline-profile, error and statistics helpers. Retains static/skinned/water/wireframe/visibility variants and their shared pipeline state; composition continues afterward in the host. |
| `impostor_atlas.inl` | In namespace `viewer`. Contains the existing upload/clear callbacks and atlas creation/descriptor methods; uses native device/resources, bake-layout constants and the existing tileset-image allocator. |
| `adopt_part_impostors.inl` | In namespace `viewer`. Retains the existing part-input validation, mip preparation, slot allocation, upload, vertex patching and diagnostics. Uses the atlas fragment's upload callback and the renderer's retained resources. |

Expanding those four includes reconstructs the reference renderer source
byte for byte. No renderer algorithm is copied into a second path. Source
locations in compiler/profiler diagnostics naturally identify the moved files.
This source separation retains host coupling; it does not establish standalone
renderer independence. The independently built facade/compositor remains a
separate boundary, with its own dependency audit.

The complete `raster.vert`, `gbuffer.frag`, `material_common.glsl` and
`impostor_common.glsl` files live in `../../shaders/`, unchanged. Root CMake
maps physical entrypoints to their existing embedded SPIR-V names, including
the skin/water specializations. The Make graph uses the same moved sources.
Common GLSL includes at the engine location forward here for other producers.
Both build graphs track those dependencies; no symlinks are required.

The rejected `.4.5` branch was not an ancestor of this task's base. Its rewritten
`raster.cpp`, `vulkan_raster.cpp`, `mesh.vert`/`mesh.frag`, scene-draw and sparse
producer implementations are therefore not imported. Its fixtures requiring
those APIs cannot be treated as native behavior parity: for example,
`raster_native.h` explicitly expects inverse-transpose normals to differ from
the legacy renderer. Preserve that incompatibility in validation evidence;
do not change production semantics or weaken those fixtures to make them pass.
