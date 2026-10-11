# Production native implementation

The ray tracing, lighting, GI and sparse-shadow relocation is documented in
[RAY_TRACING.md](RAY_TRACING.md). It uses the same private-fragment boundary
described below for raster.

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

## VT residency and POM relocation

Task `clear-crest-48.4.6` moves 25 existing C++ files into `vt/` and twelve
VT shaders into `../../shaders/`, from reference
`6490dbab806876631465259cdf5b397f258c5837`. The engine paths forward to these
single implementations. Only include paths and the two compute shaders'
version-directive placement change: their legacy entrypoint wrappers supply
`#version 460` and enable includes. Native CMake depfiles track the forwarding
chain; the rollback Make graph explicitly depends on the moved shader files.
The original `vt_residency.cpp` and `vt_stub_filler.cpp` manifest entries
compile the relocated implementations once. They are not additional sources
of the independent `matter_graphics` or `matter_graphics_vulkan` targets.

- `vt_residency.*`, `vt_feedback*`, `vt_work_budget.h`, `vt_density.h` retain
  page resources, feedback, queue admission, eviction, generation checks,
  frame-slot recycling and the existing production budgets.
- `vt_types.h`, `vt_snapshot.h`, `vt_chart_gpu.h`, `vt_canonical_page.h`,
  `vt_receiver_material.h`, `vt_material_read.h`, `vt_material_pages.h`,
  `vt_occlusion_pages.h`, `vt_surface_boundary.h`, `vt_surface_connections.h`
  and `vt_world_receivers.h` retain native layouts, ownership and receiver math.
- `vt_encoded_*` retain cache identity, asynchronous storage, staging,
  capture/upload and cancellation. `vt_stub_filler.cpp` retains the flat
  fallback. The cache adapter still delegates misses to its injected producer.
- Moved shaders retain feedback compaction, BC encoding, page sampling,
  chart/triangle traversal, normal frames and connected-surface POM.

This is source separation with retained host coupling. Native device/resources,
profiling, chart/finite-surface types and property budgets remain dependencies.
The compatibility receiver-height helper still parses legacy surface metadata;
periodic-material preparation and surface-tape helpers remain in the engine.
The legacy host still injects its compositor/enricher through the existing
filler interfaces. No procedural compositor, evaluator or world/ScriptHost
implementation was moved into graphics, and no new generation call was added.

P3 `matter::asset::SurfaceProduct` owns evaluated CPU channels and height in
metres; it does not supply the legacy BC pages, receiver/chart maps, GPU layout
or lifetime bindings. Ordered integration `.9.4` owns that conversion and
producer coordination; `.9.1` owns live renderer wiring. This move introduces
no conversion algorithm or new standalone VT capability. The existing graphics
facade dependency audit does not establish independence of this native path.
Per Jack's directive, VT/VT-RT runtime qualification is deferred to
`nimble-rapids-30`; build and non-VT checks remain required. Relocation and
native evidence are recorded in `vt/relocation-evidence.md`.
