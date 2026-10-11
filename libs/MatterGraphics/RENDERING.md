# Material, lighting and effects extraction

Task `clear-crest-48.4.3` adds `VulkanComposition`: a graphics-owned offscreen
lighting and display pass using the existing production shaders. It executes
sun diffuse/metal GGX, point/spot BRDFs, emission, native subsurface and glass
fallback, sky/SH, shadow/GI/reflection/transmission product sampling, integrated
volume composition, and ACES/exposure/sRGB/dither. It consumes evaluated G-buffer
and lighting/environment products through `CompositionInputs`. It does not
rasterize meshes, trace rays, generate atmosphere/cloud density, or evaluate
procedural surface recipes. Task `clear-crest-48.4.5` supplies the mesh, impostor,
GPU selection and sparse representation producers described below.
`raster_inputs(SceneFrame, ...)` converts retained scene snapshots;
`use_gbuffer(...)` connects their typed output to this composition pass.

`composite.vert`, `composite.frag`, `display_transform.frag` are compiled directly
from `MatterEngine3/shaders_vk/`, including their six production GLSL helpers.
No shader implementation is copied. `src/vulkan_composition.cpp` extracts the
composite/display descriptor ABI, pipeline construction and packing from
`MatterEngine3/src/render/vk_scene_renderer.cpp`, replacing device/world globals
with an injected `VulkanDeviceBinding` and typed inputs. Source relocation and
replacement of original callers belong to the ordered integration tasks.

## Input and resource contract

- All colors/products are linear radiance before display. Pixels are row-major,
  row zero at `v=0`; volume indexing is `((z*height)+y)*width+x`.
- Albedo, normal, ORM, depth and identity have identical dimensions. Normal.xyz
  is world space; zero denotes sky. Normal.w is `log2(1+emission_strength)` for
  legacy products. Optional `emission` is an independently evaluated linear RGB
  product, black by default. New mesh raster evaluates textured emission there
  and writes normal.w=0, so emission is applied once.
  ORM is roughness/metallic/AO/horizon. Depth.r is reversed-Z with finite near/far.
  Identity.x is the material index with optional impostor bit 31; identity.y is
  the producer's surface identity used to prevent GI reconstruction leaking.
- `ShadingMaterial` preserves the nine native table lanes, without adding native
  state to portable `MaterialDescription`. G-buffer producers evaluate the base
  color/roughness/metallic/texture lanes; this pass reads emission RGB,
  transmission/IOR/absorption and subsurface lanes. Only thin-walled flag bit 0
  is accepted; unsupported flags fail. `shading_material()` converts opaque
  scalar PBR metadata and rejects textures/alpha that need a producer. Merely
  converting this table does not evaluate textures or populate a G-buffer.
- Full-resolution diffuse is surface-colored radiance. Reduced-resolution
  diffuse is incident light reconstructed using identity, depth and normals;
  destination albedo/metallic/AO is applied once. Visibility is RGB [0,1].
  Transmission.a is coverage. Local direct explicitly chooses raster BRDF or
  an already-weighted traced product; the two are never added together.
- Point/spot inputs contain position, normalized-on-pack direction, linear color,
  finite range, source radius and cone cosines. All lights use the native
  oversized-list path. Sparse-grid/tile acceleration is pending extraction;
  this pass makes no large-light-count performance claim.
- Environment inputs contain a physical sky-view map, nine **radiance** SH
  coefficients, direct-sun/sky/irradiance scales, four near/far ping cloud optical
  depth volumes, transforms and filter parameters. `cloud_state` is enabled,
  near ping, far ping, cloud-top metres; `cloud_filter` is near/far voxel metres,
  sun angular tangent, radius multiplier. Public matrices are row-major;
  packing transposes once for GLSL. Volume RGB is integrated scattering and A
  is transmittance. These products do not transfer generation ownership.
- `render()` is synchronous and thread confined. It borrows CPU input only
  through the call, owns images/descriptors/pipelines/buffers, and publishes
  output only after completion/readback. Submission retention includes opaque
  frame resources as well as buffers, including failure paths. Every queue user
  must share the injected queue mutex; the binding lifetime retains the device.
- Input length/dimensions, byte budgets, identity references, finite values,
  modes and capabilities are checked before submission. Actual padded image
  allocations and buffer/staging bytes count toward `max_gpu_bytes` per frame;
  device image/descriptor limits are also checked. Driver command/descriptor
  bookkeeping is outside that payload budget. Output is unchanged on failure.
- The headless output is RGBA32F for both HDR and encoded display, sampled with
  nearest G-buffer/product and linear volume/environment filters. The live
  renderer's attachment precision, swapchain and temporal histories are separate
  integration concerns; bitwise full-frame parity is not claimed.

All advertised capabilities are version 1. The list is available from
`composition_capabilities()`; unknown names and versions fail explicitly.

| Capability | Concrete inputs and implemented behavior | Evidence |
| --- | --- | --- |
| `matter.graphics.composition` | Evaluated G-buffer, camera, material table, sun; native diffuse/metal/emission shading | dielectric/metallic shadow, emission fixtures |
| `matter.graphics.local-light-shading` | Typed point/spot array and direct owner; native attenuation and GGX/Lambert BRDF | point, spot, translated camera, single owner |
| `matter.graphics.shadow-product` | Evaluated RGB visibility; no shadow producer | dielectric/metallic colored visibility |
| `matter.graphics.gi-product` | Full/reduced diffuse, specular and traced local direct products | product combination, odd-extent reduced diffuse |
| `matter.graphics.environment-product` | Sky map, radiance SH, cloud optical depth/transforms/pings | sky, SH, cloud fixtures |
| `matter.graphics.volume-product` | Integrated scattering/transmittance volume | integrated-volume fixture |
| `matter.graphics.native-subsurface` | Scattering/thin-wall material plus impostor identity | subsurface/impostor masking |
| `matter.graphics.native-transmission` | Traced transmission/coverage or native environment glass fallback | product combination and glass fallback |
| `matter.graphics.display-transform` | Exposure, debug view, HDR result | every display pixel plus debug passthrough |

## Geometry and representation producer contract

All producer capabilities are version 1. `raster_capabilities()` advertises
`matter.graphics.gbuffer-producer`, `matter.graphics.impostor-draw` and
`matter.graphics.raster-alpha-mask`. `scene_draw_capabilities()` advertises
`matter.graphics.gpu-scene-draw`. `sparse_surface_capabilities()` advertises
`matter.graphics.sparse-surface-draw`, adding `matter.graphics.sparse-surface-query`
only when the device owner explicitly enables `VulkanSparseFeatures::ray_query_enabled`.
Unknown names/versions and unsupported representation requests fail explicitly.

- `RasterGeometry` holds shared indexed position/normal/tangent/UV geometry.
  `raster_inputs` shares each packed mesh/base-UV variant among snapshot instances,
  resolves material/texture identities, checks consumed bytes against their
  resource hashes, and preserves instance/assembly-path identities. Portable v1
  materials are opaque; the module API additionally supports alpha mask and
  double-sided raster. Blended transparency belongs to its separate producer.
- Texture inputs are resolved single-level RGBA8 with explicit sRGB/linear,
  nearest/linear and repeat/clamp/mirror sampling. Base color uses its selected UV
  set; metallic/roughness, normal and emissive use UV0. Roughness/metallic are G/B.
  Normals use inverse transpose, winding includes reflection parity, and tangent
  handedness follows that parity. Missing normals/tangents use geometric/UV
  derivatives; a degenerate tangent gets a finite orthogonal frame.
- Prepared impostor atlases have two RGBA8 layers (shade oct-normal/depth/coverage,
  then tint RGB/blend) in the native 8x8 grid. The 48 used views comprise 16
  azimuths and three elevation rings. `impostor_raster.glsl` and
  `impostor_sample.glsl` are shared directly with the legacy path; runtime mips use
  its original per-view coverage-preserving downsampler. Placement supports
  rotation and positive orthogonal scale, with the native near-boundary shrink,
  parallax, depth and alpha behavior. Reflected/sheared impostors fail explicitly.
- `GbufferOutput` contains RGBA32F albedo, world normal, ORM, depth and evaluated
  emission, RG32_UINT material/token identity, the material table and semantic
  identity map. Depth comes from D32 reversed-Z raster. Token zero is clear;
  material bit 31 identifies impostors. `use_gbuffer` transfers these products
  while retaining the caller's lighting/environment settings. The compositor's
  extra emission descriptor is compiled only under `MATTER_EXPLICIT_EMISSION_PRODUCT`;
  the original legacy shader ABI is unchanged when the macro is absent.
- `VulkanSceneDraw` runs the original culling/LOD, geometry-cut and visible-ID
  shaders. Bounds, ladders and hierarchy are evaluated inputs, with explicit
  animation generations and an explicit-skin flag preventing duplicate draws.
  Occlusion accepts same-frame identity or renders the coarse list itself;
  masked and impostor draws remain exempt. It returns selected placements,
  command counts/statistics and missing-node requests. Every draw preserves its
  selected VT slot, prior transform/history and water slot/generation for B/E.
  Capacity overflow fails atomically. `pick` resolves integer IDs to semantic
  identity; selection lines use the original overlay shaders, without depth,
  for caller composition after display.
- `VulkanSparseSurface` packs evaluated brick masks, fitted cell distributions,
  planes/support, triangles, dense or 8x8 sparse texture mip pages, LOD ladders and
  prototype hierarchies into the original shader ABI. It performs native flat
  selection or bounded hierarchy refinement, then GPU-generated indirect voxel
  and surface draws. Surface, directional-surface and voxel levels share the
  native distance rule. Flat placements accept affine transforms; hierarchy
  roots/edges require similarities. The caller supplies conservative bounds and
  a complete coarse cover. It returns G-buffer, velocity/reactivity and hierarchy
  status. Fitting, baking, hierarchy construction and temporal reconstruction
  remain external.
- Optional sparse query consumes a completed forest AS and retained inner AS
  addresses on the injected device. Forest instance IDs index `placements`,
  custom indices index `objects`; object-local rays traverse that object's inner
  AS. Inner instance IDs plus `first_instance` index `local_instances`, custom
  indices name global levels, and primitive IDs index triangles within each
  level. The RT owner supplies this exact indexing ABI, completes builds/updates
  before handoff, and retains every AS through `lifetime`. Opaque AS contents
  cannot be inspected by graphics. The consumer validates explicit metadata and
  device identity; it never builds acceleration or invokes a generator.

The same synchronous, thread-confined, device-owner/queue-mutex and atomic-output
rules as composition apply. CPU payload budgets are checked before adapter
packing; actual image/buffer/staging allocations count against the GPU budget.
Limits bound dimensions, draws, materials, textures and input/GPU bytes. Sparse
node/primitive budgets are explicit, graph depth/cycles are checked, and native
fallback preserves the coarse cover. Invalid or unavailable features produce a
typed failure and leave the previous output intact. The native passes require
their reported color-attachment/descriptor/image limits; query additionally
requires enabled Vulkan buffer-device-address, acceleration-structure and
ray-query features/extensions. They do not create a device or enable its features.

## Native feature inventory and remaining producers

Paths in this table are relative to `MatterEngine3/`. A **pending** capability
below is a proposed boundary name, never advertised by this pass. Existing
native paths remain intact. Absence of a producer must cause explicit capability
failure or use an approved, tested compatibility adapter during migration.
Central wiring alone cannot substitute for missing module extraction.

The A–E labels below identify extraction ownership. A is delivered by
`clear-crest-48.4.5`; the B–E rows describe their separate producer boundaries,
not their current queue status. Live integration remains downstream. Existing
P3/P4/P6 own generation and asset preparation; graphics never calls these
producers itself. Supervisor instruction `msg-16f7afe1eebe45e5b727272c1432fffa`
authorizes the shared impostor helper extraction while preserving legacy ABI
and behavior. New imported raster uses inverse-transpose normals; legacy
`raster.vert` retains its original model-normal semantics.

| Key | Producer responsibility |
| --- | --- |
| A | Extract graphics G-buffer raster materials and impostor producers |
| B | Extract graphics VT residency feedback and POM receivers |
| C | Extract graphics ray tracing acceleration lighting and GI filters |
| D | Extract physical atmosphere cloud and froxel producers |
| E | Extract forward water temporal and DLSS graphics adapters |

| Native feature / exact legacy implementation | Typed input; resource/producer ownership | Extraction vs. wiring owner | Acceptance still owed |
| --- | --- | --- | --- |
| Mesh/material raster, textures, normal maps and alpha: `src/render/vk_scene_renderer.*`, `raster_mesh.*`; `shaders_vk/raster.vert`, `gbuffer.frag`, `material_common.glsl`, `scene_instance.glsl` | `SceneFrame` geometry/transforms plus resolved texture bytes/PBR materials produce the composition G-buffer; GPU images/pipelines owned by graphics. Implemented `matter.graphics.gbuffer-producer` | A implements `VulkanRaster` and `raster_inputs`. R4 `clear-crest-48.4.4` uses it for imported/synthetic assets; `.9.1` wires legacy scene entrypoints | Native imported/synthetic, shared/mirrored, texture/normal/alpha and deferred fixtures pass; live scene/swapchain qualification remains with integration |
| Sun/local light/shadow receiver shading: `composite.frag`, `local_lighting.glsl`, `local_light_types.glsl`, `src/render/vk_lighting_controls.*` | Implemented typed sun/local array and evaluated RGB visibility; compositor owns resources | This task extracts receiver shading. `.9.1` maps live controls; C extracts acceleration/visibility production | Selected receiver fixtures pass here; live lighting-garden/legacy exposure settings still require `.9.1` matched views |
| Sparse local light index and primary tile culling: `src/render/primary_light_culling.h`; `shaders_vk/primary_light_cull.comp`; renderer list/grid packing | Typed light/camera/scene records; graphics owns index/masks. Pending `matter.graphics.local-light-acceleration` | C extracts sparse/tile production; `.9.1` wires updates | `local_light_index_tests.cpp`, `primary_light_culling_tests.cpp`, raster/RT differential visibility and light-count performance |
| RT primary, shadows, GI, reflections, refraction/absorption/clearcoat: renderer RT setup, `src/render/vk_blas_cache.h`, `blas_disk_cache.h`; `shaders_vk/rt_primary*.rgen`, `rt_shadow.*`, `rt_lighting*.rgen`, `rt_lighting_impl.glsl`, `rt_surface.*`, `rt_surface_common.glsl`, `rt_visibility.*`, `rt_radiance.rmiss` | Explicit geometry/material snapshots, scene transforms, camera/light/environment; graphics owns BLAS/TLAS, SBT, pipelines, histories and radiance products. Pending `matter.graphics.ray-tracing-producer` | C extracts RT production/resource lifecycle; `.9.1` converts current calls | Existing RT surface test shader, primary fixed/adaptive/culled parity, thin glass and opaque shadow cases, acceleration updates and device-fence retirement |
| GI filtering/reconstruction: `src/render/vk_gi_math.*`, `vk_gi_contract.h`; `shaders_vk/gi_temporal.comp`, `gi_atrous.comp`, `gi_common.glsl` | Explicit prior/current depth/normal/identity/lighting with history epoch; graphics owns histories. Receiver reconstruction implemented; pending `matter.graphics.gi-filter-producer` | C extracts temporal/atrous filters; `.9.1` wires invalidation | Disocclusion, history reset, full/reduced/odd resolution energy and edge preservation; current fixture only proves receiver reconstruction |
| VT residency, feedback, formats/compression and POM: `src/render/vt_residency.*`, `vt_feedback*.h`, `vt_encoded_*.h`, `vt_material_read.h`, `vt_receiver_material.h`; `shaders_vk/vt_feedback.comp`, `vt_common.glsl`, `vt_parallax.glsl`, `vt_draw_inputs.glsl`, `vt_chart_*.glsl`, `vt_bc_encode.comp` | Graphics receives evaluated canonical/encoded pages, chart/receiver maps and resource identities; owns GPU page tables, residency, sampling, feedback and BC upload. Pending `matter.graphics.virtual-texture-residency` | B extracts graphics runtime; `.9.1` wires residency; `.9.4` wires P3 products | `vt_residency_tests.cpp`, `vt_encoded_pages_tests.cpp`, `vt_material_read_tests.h`, seam/feedback/eviction/POM cases and reference images |
| Procedural surface evaluation, AO/height normals, finite stamps, Wang/tileset baking: `src/render/vt_compositor.*`, `vt_enrich.*`, `tileset_bake_vk.*`, `shared_surface_compile.h`; `shaders_vk/vt_composite.comp`, `vt_surface_tape.glsl`, `vt_surface_walk.glsl`, `vt_seed_walk.glsl`, `vt_noise.glsl`, `vt_enrich_ao.comp`, `vt_height_normal.comp`, `finite_surface_stamp.glsl`, `tileset_bake_*.comp`, `wang_common.glsl` | Evaluator belongs to AssetPipeline/optional compute, outputs finite evaluated pages/maps/meshes. Graphics receives products or emits demand to coordinator, never receives a generator callback | Existing P3 `clear-crest-48.8.3` extracts evaluation; B consumes products; `.9.4` supplies adapter | P3 surface parity, finite/periodic identity, chart seams and AO/normal determinism; no evaluator symbols in graphics dependency closure |
| Impostors/billboards and atlas mip sampling: renderer paths, `src/render/impostor_mips.h`, `shaders_vk/impostor_common.glsl`, `gbuffer.frag` | Prepared atlas/mesh/identity data; asset preparation makes atlas, graphics owns draw/residency. Implemented `matter.graphics.impostor-draw`, preserving identity emission/subsurface semantics | A implements shared placement/sampling and view-isolated mip upload; asset preparation supplies data; `.9.1` wires | Native view/rotation/scale/alpha/mip and mesh-to-impostor selection/material fixtures pass; full legacy vegetation silhouette/color qualification remains with integration |
| Atmosphere: `src/render/vk_atmosphere.*`; `shaders_vk/atmosphere_{transmittance,multiscatter,sky_view,irradiance}.comp`, `atmosphere_common.glsl` | Typed atmosphere parameters, sun and camera altitude; graphics owns LUTs/SH and production. Pending `matter.graphics.atmosphere-producer`; composition consumes sky/SH | D extracts LUT producer; `.9.1` maps world-authored values without world headers | `atmosphere_tests.cpp`, `environment_sampling_test.comp`, LUT/SH energy and matched horizon/sun views |
| Cloud shadows: `src/render/vk_cloud_shadows.*`; `shaders_vk/cloud_shadow_{density,integrate,reproject}.comp`, `cloud_density.glsl`, `cloud_shadow_common.glsl` | Typed density/weather parameters, time/wind/seed, camera clipmaps; graphics owns tau/reprojection histories. Pending `matter.graphics.cloud-shadow-producer` | D extracts density/integrate/reproject; `.9.1` injects parameters | `cloud_shadow_tests.cpp`, `cloud_layer_tests.cpp`, `cloud_image_metrics_tests.py`; near/far/ping/edge/movement parity (constant tau consumer tested here) |
| Volumetrics/emitters: `src/render/vk_volumetrics.*`, `vk_emitter_gather.*`; `shaders_vk/vol_{density,scatter,integrate}.comp`, `vol_common.glsl` | Coordinator gathers typed world emitters; graphics receives explicit bounded emitter/medium/light arrays and owns froxel volumes/history. Pending `matter.graphics.volumetric-producer` | D extracts froxel producer; `.9.1` keeps world/ECS gathering outside graphics | `volumetric_quality_tests.cpp`, height layer, fog/cloud silhouette, temporal reset and emissive injection matched views |
| Water forward/field/mesh playback: `src/render/water_field_vk.*`, `water_field_vk_resources.*`, `water_animation_gpu.*`, `water_mesh_animation_playback.*`, `water_forward_reference.*`, `water_surface_reference.*`; `shaders_vk/water_forward.frag`, `water_surface.glsl`, `water_screen_space.glsl` | Prepared water mesh/field, phase/time/material/medium/environment, opaque depth/color, animation product. Graphics owns field textures and forward draw; generation owns hydrology/mesh evaluation. Pending `matter.graphics.water-forward` | E extracts surface/resources/playback; P4 `.8.4` and P6 `.8.6` supply animation/compute products; `.9.4` and `.9.1` wire | `gpu_water_render_tests.cpp`, `gpu_water_animation_render_tests.cpp`, field/surface/forward-reference tests; refraction/foam/underwater/transparency and animated boundary parity |
| Temporal reconstruction/DLSS: `src/render/vk_temporal.*`, `streamline_bridge.*`, renderer motion/jitter paths | Typed color/depth/motion/exposure, camera and explicit reset epoch; graphics owns temporal/SDK resources. Pending `matter.graphics.temporal-upscale` | E extracts adapters/settings from world_session includes; `.9.1` wires device/swapchain, no SDK callback into generation | History reset/camera cuts, motion vectors, resize, SDK available/unavailable behavior and matched static/moving native views |
| Sparse voxel/surface proxy rendering and shadows: `src/render/vk_sparse_voxel.*`, `vk_sparse_placement_update.inl`; `shaders_vk/sparse_voxel*.{vert,frag,comp,glsl}`, `sparse_hierarchy.comp`, `surface_proxy.*`, `surface_query.*`, `surface_detail.glsl`, `surface_filter.glsl` | Prepared sparse hierarchy/surface data, transforms/camera/materials; graphics owns traversal/residency/draw, no field generation. Implemented `matter.graphics.sparse-surface-draw` and optional `matter.graphics.sparse-surface-query` | A implements native flat/hierarchy draws and completed-AS query consumption; C owns shadow/AS construction; P6 supplies geometry compute products; `.9.1/.9.4` wire | Native raster/proxy/planar-voxel parity, surface/voxel LOD transitions, page filtering, history products, hierarchy fallback and nested query pass; shadow production/live temporal qualification belongs to C/E/integration |
| GPU culling, draw selection, picking/selection overlay: renderer paths, `indirect_draw_runs.h`, `geometry_raster_adapter.h`; `shaders_vk/cull.comp`, `geometry_select.comp`, `visibility_id.*`, `visible_ids.comp`, `selection_line.*`, `geometry_cut.glsl` | Explicit draw bounds, transforms, selected IDs and cut planes; graphics owns indirect/ID resources, editor owns interaction. Implemented `matter.graphics.gpu-scene-draw` | A implements `VulkanSceneDraw`, `pick` and `overlay`; `.9.1` wires legacy entrypoint/editor | Native frustum/occlusion/LOD, hierarchy missing-child/fallback, animation-bound generation, reflected picking and overlay fixtures pass; live indirect integration remains |
| Skinning/rigid animation and bounds: `src/render/vk_animation_skinning.*`, `vk_animation_bounds.*`, `animation_{skin,rigid}_bridge.*`; `shaders_vk/animation_skin.comp` | Prepared skin/rig/clip/pose products; graphics owns GPU skin/bounds resources; animation evaluation and ECS remain external | Existing P4 `clear-crest-48.8.4` extracts typed bridges; A consumes resulting draw buffers; `.9.4` wires | `vk_animation_skin_compute_tests.cpp`, `vk_animation_bounds_tests.cpp`, rigid/skin bridge and phase B/C acceptance |
| GPU geometry/material generation: `src/render/gpu_meshing/*`, `geometry_world_runtime.*`; `shaders_vk/gpu_mesh_*.comp`, `solid_*.comp`, `face_material.comp` | Optional compute generator owns field/meshing/material evaluation and outputs geometry products. Graphics has no generation entrypoint | Existing P6 `clear-crest-48.8.6`; `.9.4` wires service separation | Compute field/meshing fixture parity and renderer-only linkage; not advertised as a graphics capability |

Probe/test shaders (`*_probe.comp`, `*_test.*`) belong to their corresponding
feature's validation. Device/context/swapchain, scene visibility/buffer resources,
frame transforms, statistics and profiling are covered by the R2 migration map
in [INTEGRATION.md](INTEGRATION.md) and `.9.1`; this leaf does not change them.

## Reproduction and evidence

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test
```

The supported native wrapper resolves pinned MSVC/Vulkan SDK, configures Ninja,
compiles production GLSL to embedded SPIR-V, and runs all five CTests. The optional
Vulkan target requires glslc and Python 3.9+ even when tests are disabled; the core
`matter_graphics` target has no Vulkan/shader-tool dependency. GLSL depfiles are
normalized for paths containing spaces. The independence test audits the actual
CMake graph, links, compiler-discovered dependencies and the exact shader closure
enumerated in `tests/independence.py`. No broad legacy directory allowance is used.

Recorded on A task baseline `ff3f3e5bc` plus this extraction: MSVC 19.44.35211,
Vulkan SDK 1.4.357, NVIDIA GeForce RTX 4090, Khronos validation and synchronization
validation enabled. Five of five CTests passed. The Vulkan loader reported two
pre-existing missing Epic overlay manifests; there were **zero Vulkan validation
errors**, including teardown. These loader warnings are not shader/device errors.

`tests/composition_native.h` renders fourteen 65x33 matched views (2,145 pixels
each) with production shaders and an independent double-precision CPU plane/BRDF
reference. Each HDR channel must meet `3e-4 + 3e-4*abs(reference)` and each encoded
display channel `3e-4`; alpha is exactly 1. Largest observed HDR error was
`4.02282e-6` (metallic GGX), largest display error `2.32458e-6`. Fixture IDs:

- `dielectric-colored-shadow`, `metallic-ggx-shadow`
- `point-light-brdf`, `spot-light-cone`, `translated-camera-light`
- `rt-products-emission-single-owner`, `integrated-volume`
- `environment-irradiance`, `reduced-diffuse-odd-extent`, `physical-sky-product`
- `subsurface-impostor-mask`, `cloud-optical-depth`, `native-glass-fallback`
- `debug-display-passthrough`

Matched actual/reference PPMs are generated in the build directory's
`composition-evidence/`; per-fixture errors appear in
`Testing/Temporary/LastTest.log`. A visual contact sheet is committed at
[tests/evidence/composition-matched.png](tests/evidence/composition-matched.png).
The CPU boundary tests cover malformed payloads/dimensions, capabilities,
material/identity, finite camera/light inputs and budget rejection. Native tests
also prove output remains unchanged on unsupported producer and GPU-budget
failure and that images/pipelines/descriptors' shared frame owner remains alive
behind a host-gated GPU submission until its fence completes.

A adds four more 65x33 deferred matched fixtures using the same error bounds:
`raster-geometry-deferred`, `raster-textured-emission-once`,
`raster-imported-shared`, and `raster-imported-textured-shared`. The imported
fixture decodes the checked-in MatterContracts repeated-assembly JSON and checks
analytic triangle coverage, mirrored/shared placement, color, normal and depth;
it does not link an image or glTF decoder. Off-boundary texture-mask pixels have
explicit expected coverage. Other native producer assertions are recorded below.
The four raster fixtures have maximum HDR error `1.63913e-8` and display error
`1.19209e-7`; their actual/reference contact sheet is
[tests/evidence/raster-matched.png](tests/evidence/raster-matched.png).

| Native fixture source | Coverage |
| --- | --- |
| `tests/raster_native.h` | Synthetic geometry into deferred lighting, all four PBR textures, sRGB, sampler wrap, tangent and derivative normal frames, mirrored/nonuniform transforms, emission once, alpha mask, empty/failure rollback, impostor views/rotation/scale/mip isolation |
| `tests/imported_raster_native.h` | Portable JSON geometry, analytic coverage/depth/normal/material, shared packed geometry, mirrored placement, UV1, replacement/removal, caller release and retained old snapshots, preallocation budget |
| `tests/scene_draw_native.h` | Production GPU frustum/LOD/occlusion, mesh/impostor transitions, hierarchy requests/refinement/bounded fallback, animation bounds/generation and explicit skin ownership, VT/water/history metadata, integer picking and selection overlay |
| `tests/sparse_native.h` | All-pixel surface/planar-voxel/raster parity, surface-to-voxel LOD transition, dense/sparse mip-page filtering, alpha coverage, history velocity/reactivity, hierarchy refinement/cover fallback, invalid graph and empty cleanup |
| `tests/sparse_query_native.h` | Completed external nested AS, proxy parity, alpha/texture changes, retained lifetime, foreign device and disabled-feature rejection; AS construction exists only in the test fixture |

The core also passes four CTests with GNU 13.3/CMake 3.28.3 on WSL using
`cmake -S libs/MatterGraphics -B /tmp/matter-graphics-a-core -G Ninja -DCMAKE_BUILD_TYPE=Debug`,
`cmake --build /tmp/matter-graphics-a-core --parallel 2` and
`ctest --test-dir /tmp/matter-graphics-a-core --output-on-failure -j1`.
The supported native fixture device enables timeline semaphores, buffer device
address, acceleration structures and ray query; no software fake replaces these
checks. Original `raster.vert`, `gbuffer.frag` and `composite.frag` additionally
compile with Vulkan 1.2 glslc and the new emission macro absent.

These fixtures establish selected native raster, representation and shading
behavior. Full live-scene precision/silhouette/temporal qualification, RT lighting
production, water production and performance remain with the inventory owners.
The synchronous module path repacks/uploads and reads back per call; it makes no
zero-copy or live-frame performance claim.
