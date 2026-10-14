# Production native implementations

The ray tracing, lighting, GI and sparse-shadow relocation is documented in
[RAY_TRACING.md](RAY_TRACING.md). It uses the same private-fragment boundary
described below for raster.

## Atmosphere, cloud shadows and froxels

`clear-crest-48.4.8` moves `vk_atmosphere.*`, `vk_cloud_shadows.*` and
`vk_volumetrics.*` from the engine into this directory. The reference is
`6490dbab806876631465259cdf5b397f258c5837`. The old translation units and
headers forward here, so existing CMake/Make hosts compile exactly one copy.
Resource ownership, dispatches, layouts, histories, failure injection and
numerical algorithms are preserved. The three producers account for 5,526
original lines; parameter/reference headers and two extracted data blocks
bring the total to 6,892 relocated lines.

The four parameter headers (`atmosphere.h`, `cloud_layers.h`,
`cloud_shadow_settings.h`, `volumetric_quality.h`) are byte-identical moves.
`fog_settings.h` contains the unchanged 125-line FogSettings/cloud-helper
block from `world_definition.h`; `gpu_volume_emitter.h` contains the unchanged
16-line GPU record from `vk_emitter_gather.h`. The latter's part-aware gather
stays in the engine. No emitter-update behavior has been added.

The producer sources no longer include world definitions, WorldSession,
VkSceneRenderer or the part-aware emitter gatherer. `set_sun_direction(Float3)`
performs the original three assignments; the old `set_lighting` entrypoint
forwards the direction from its existing host translation unit. Consumers of
the module archive use `set_sun_direction`. All other public native signatures
and namespaces remain intact.

`matter_graphics_environment` is a separately compiled static archive, enabled
with `MATTER_GRAPHICS_VULKAN`. It still borrows the native `VulkanDevice`,
`vk_resources`, frame matrices and matrix packing support. A host must supply
their implementations and the existing descriptor/command-buffer lifecycle.
This is not a new standalone device or facade producer API. The dependency
audit enumerates the exact remaining platform/data headers and rejects world,
scene, ScriptHost and generation dependencies. The portable compositor still
rejects unadvertised producer capabilities.

The archive compiles the ten original atmosphere/cloud-shadow/volume compute
shaders from `MatterEngine3/shaders_vk` with Vulkan 1.3, matching the host.
Shaders remain at their existing source authority; none are copied or rewritten.
A ten-line adapter reuses the host SPIR-V generator with a separate namespace,
preventing its subset lookup registry from colliding with the host's full one.
The forwarding host continues using its full existing shader registry.

Validation and remaining qualification are recorded in
[RENDERING.md](../../RENDERING.md#environment-relocation-evidence).

## Raster implementation

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

`clear-crest-48.4.14` turns the G-buffer recorder and pipeline creation fragments
into compiled `mesh_raster.cpp` and `mesh_pipelines.cpp`. The recorder borrows
neutral image/buffer/pipeline/descriptor records, with explicit callbacks for VT
pre/post recording and sparse draws. Host ownership stays in `VkSceneRenderer`;
its call site forwards existing buffers, layout bookkeeping, timestamps, draw
order, skin/water records and hooks. Pipeline creation receives a shader loader
and profiling callback, preserving native variants and specialization controls.
The shared image barrier and destination-stage rules live in
`image_transitions.h`; the engine delegates to their single implementation.

The standalone public `VulkanRaster` entrypoint consumes the existing immutable
`SceneFrame` asset/scene facade. Its device binding owns native lifetimes and
queue synchronization. Uniform opaque PBR geometry is packed per visible
placement with inverse-transpose normals and winding parity, then the exact
production pipeline and recorder produce G-buffers. Prepared texture, VT, skin,
water and impostor products are not yet accepted by this facade adapter; they
remain supported by the host's unchanged extensions. Unsupported materials
fail before recording. There is no world/source/generator dependency and no
alternate raster shader. `tests/raster_native.h` is the minimal GFX-IMPORTED
caller and checks actual repeated/mirrored fixture pixels, identities, normals,
materials, depth, clear state and atomic resource/appearance failures.

Two `.inl` fragments still remain at their lexical host positions:

| Fragment | Host boundary and dependencies |
| --- | --- |
| `impostor_atlas.inl` | Existing upload/clear callbacks and atlas creation/descriptors; native resources, bake constants and tileset image allocation. |
| `adopt_part_impostors.inl` | Existing validation, mips, slot allocation, upload, vertex patching and diagnostics; retained host resources. |

These atlas consumers do not add standalone impostor support. The standalone
CMake graph separately compiles the raster pass and six original shader variants.
Its source, link, compiler-header and shader-dependency audit admits only the
exact platform/pure-data closure and the compile-out ProfileLib header.

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
that branch's `raster_native.h` expects inverse-transpose normals to differ from
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
