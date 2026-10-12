# Material, lighting and effects extraction

Task `clear-crest-48.4.3` adds `VulkanComposition`: a graphics-owned offscreen
lighting and display pass using the existing production shaders. It executes
sun diffuse/metal GGX, point/spot BRDFs, emission, native subsurface and glass
fallback, sky/SH, shadow/GI/reflection/transmission product sampling, integrated
volume composition, and ACES/exposure/sRGB/dither. It consumes evaluated G-buffer
and lighting/environment products through `CompositionInputs`. It does not
rasterize meshes, trace rays, generate atmosphere/cloud density, or evaluate
procedural surface recipes. `SceneBackend` and this pass remain separate APIs
until the producer/integration work below connects their explicit resources.

`composite.vert`, `composite.frag`, `display_transform.frag` are compiled directly
from `MatterEngine3/shaders_vk/`, including their production GLSL helpers.
The engine's `impostor_common.glsl` forwards to `shaders/impostor_common.glsl`.
No shader implementation is copied. `src/vulkan_composition.cpp` extracts the
composite/display descriptor ABI, pipeline construction and packing from
`MatterEngine3/src/render/vk_scene_renderer.cpp`, replacing device/world globals
with an injected `VulkanDeviceBinding` and typed inputs. Source relocation and
replacement of original callers belong to the ordered integration tasks.

## Input and resource contract

- All colors/products are linear radiance before display. Pixels are row-major,
  row zero at `v=0`; volume indexing is `((z*height)+y)*width+x`.
- Albedo, normal, ORM, depth and identity have identical dimensions. Normal.xyz
  is world space; zero denotes sky. Normal.w is `log2(1+emission_strength)`.
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

## Native feature inventory and remaining producers

Paths in this table are relative to `MatterEngine3/`. A **pending** capability
below is a proposed boundary name, never advertised by this pass. Existing
native paths remain intact. Absence of a producer must cause explicit capability
failure or use an approved, tested compatibility adapter during migration.
Central wiring alone cannot substitute for missing module extraction.

`bright-torrent-52` relocates the production raster/material shaders and
mesh/impostor helpers to MatterGraphics, plus the G-buffer recording, raster
pipeline and atlas routines as private fragments included by the existing host.
The inventory below names their original engine locations; see
[the actual paths and retained host dependencies](src/native/README.md).
Standalone producer APIs and broader facade integration remain pending.

`clear-crest-48.4.6` similarly relocates the existing VT residency/feedback,
encoded-page upload/lifetime helpers and POM shaders. Legacy files forward to
the graphics-owned implementation; behavior and host wiring remain intact.
See [the VT boundary and deferred qualification](src/native/README.md#vt-residency-and-pom-relocation).
This does not advertise a standalone VT runtime or a P3 product adapter.

`clear-crest-48.4.7` relocates C's production BLAS/TLAS, RT dispatch, local-light
index/tile culling, GI filters and sparse-shadow interop under the same pure-move
directive. See [the moved paths, retained ownership and native qualification](src/native/RAY_TRACING.md).
The original inventory rows below still identify the legacy entrypoints. This
source move does not advertise any of the proposed standalone capabilities.

`clear-crest-48.4.8` also relocates the three native environment producers and
their pure parameter records into `src/native`, with engine forwarding paths.
See [environment relocation evidence](#environment-relocation-evidence) for
the retained platform boundary and the separately reported native test results.

Follow-up owners A–E are temporary task keys in supervisor proposal
`prop-9ef784b996c1`, vault file
`plans/2026-10-11-graphics-producer-followups.json`. The daemon validated the
atomic graph without cycles. Human approval gate `gate-60889b1ee37a` awaits a decision. It has no attached
waiters, so the supervisor explicitly **PAUSED** R4 (`.4.4`), `.9.1` and `.9.4`
pending that decision and installed dependencies (correction in message
`msg-b8daf5705593477e830f1f92873f4761`). The proposal has **not** been committed and
these five task keys are not yet live task IDs. A follows R3 and gates R4/`.9.1`;
B–E follow A and gate `.9.4`, B also consumes `.8.3`, E consumes `.8.4/.8.6`.
Supervisor message `msg-64feb6aa9eaa4a73ab90ac82821d7982` directs this temporary
reference and forbids treating the staged proposal as implemented coverage.
After approval the supervisor must replace these keys with assigned IDs. The
producers must be delivered or explicitly dispositioned before acceptance;
legacy compatibility remains required meanwhile. Existing P3/P4/P6 own
generation and asset preparation; graphics never calls these producers itself.

| Key | Proposed task title |
| --- | --- |
| A | Extract graphics G-buffer raster materials and impostor producers |
| B | Extract graphics VT residency feedback and POM receivers |
| C | Extract graphics ray tracing acceleration lighting and GI filters |
| D | Extract physical atmosphere cloud and froxel producers |
| E | Extract forward water temporal and DLSS graphics adapters |

| Native feature / exact legacy implementation | Typed input; resource/producer ownership | Extraction vs. wiring owner | Acceptance still owed |
| --- | --- | --- | --- |
| Mesh/material raster, textures, normal maps and alpha: `src/render/vk_scene_renderer.*`, `raster_mesh.*`; `shaders_vk/raster.vert`, `gbuffer.frag`, `material_common.glsl`, `scene_instance.glsl` | `SceneFrame` geometry/transforms plus resolved texture bytes/PBR materials produce the composition G-buffer; GPU images/pipelines owned by graphics. Pending `matter.graphics.gbuffer-producer` | A extracts the producer. R4 `clear-crest-48.4.4` uses it for imported/synthetic assets; `.9.1` wires legacy scene entrypoints | Imported textured/untextured meshes, mirrored transforms, alpha/normal/roughness and shared instances in matched images; no generation link |
| Sun/local light/shadow receiver shading: `composite.frag`, `local_lighting.glsl`, `local_light_types.glsl`, `src/render/vk_lighting_controls.*` | Implemented typed sun/local array and evaluated RGB visibility; compositor owns resources | This task extracts receiver shading. `.9.1` maps live controls; C extracts acceleration/visibility production | Selected receiver fixtures pass here; live lighting-garden/legacy exposure settings still require `.9.1` matched views |
| Sparse local light index and primary tile culling: `src/render/primary_light_culling.h`; `shaders_vk/primary_light_cull.comp`; renderer list/grid packing | Typed light/camera/scene records; graphics owns index/masks. Pending `matter.graphics.local-light-acceleration` | C extracts sparse/tile production; `.9.1` wires updates | `local_light_index_tests.cpp`, `primary_light_culling_tests.cpp`, raster/RT differential visibility and light-count performance |
| RT primary, shadows, GI, reflections, refraction/absorption/clearcoat: renderer RT setup, `src/render/vk_blas_cache.h`, `blas_disk_cache.h`; `shaders_vk/rt_primary*.rgen`, `rt_shadow.*`, `rt_lighting*.rgen`, `rt_lighting_impl.glsl`, `rt_surface.*`, `rt_surface_common.glsl`, `rt_visibility.*`, `rt_radiance.rmiss` | Explicit geometry/material snapshots, scene transforms, camera/light/environment; graphics owns BLAS/TLAS, SBT, pipelines, histories and radiance products. Pending `matter.graphics.ray-tracing-producer` | C extracts RT production/resource lifecycle; `.9.1` converts current calls | Existing RT surface test shader, primary fixed/adaptive/culled parity, thin glass and opaque shadow cases, acceleration updates and device-fence retirement |
| GI filtering/reconstruction: `src/render/vk_gi_math.*`, `vk_gi_contract.h`; `shaders_vk/gi_temporal.comp`, `gi_atrous.comp`, `gi_common.glsl` | Explicit prior/current depth/normal/identity/lighting with history epoch; graphics owns histories. Receiver reconstruction implemented; pending `matter.graphics.gi-filter-producer` | C extracts temporal/atrous filters; `.9.1` wires invalidation | Disocclusion, history reset, full/reduced/odd resolution energy and edge preservation; current fixture only proves receiver reconstruction |
| VT residency, feedback, formats/compression and POM: `src/render/vt_residency.*`, `vt_feedback*.h`, `vt_encoded_*.h`, `vt_material_read.h`, `vt_receiver_material.h`; `shaders_vk/vt_feedback.comp`, `vt_common.glsl`, `vt_parallax.glsl`, `vt_draw_inputs.glsl`, `vt_chart_*.glsl`, `vt_bc_encode.comp` | Graphics receives evaluated canonical/encoded pages, chart/receiver maps and resource identities; owns GPU page tables, residency, sampling, feedback and BC upload. Pending `matter.graphics.virtual-texture-residency` | B extracts graphics runtime; `.9.1` wires residency; `.9.4` wires P3 products | `vt_residency_tests.cpp`, `vt_encoded_pages_tests.cpp`, `vt_material_read_tests.h`, seam/feedback/eviction/POM cases and reference images |
| Procedural surface evaluation, AO/height normals, finite stamps, Wang/tileset baking: `src/render/vt_compositor.*`, `vt_enrich.*`, `tileset_bake_vk.*`, `shared_surface_compile.h`; `shaders_vk/vt_composite.comp`, `vt_surface_tape.glsl`, `vt_surface_walk.glsl`, `vt_seed_walk.glsl`, `vt_noise.glsl`, `vt_enrich_ao.comp`, `vt_height_normal.comp`, `finite_surface_stamp.glsl`, `tileset_bake_*.comp`, `wang_common.glsl` | Evaluator belongs to AssetPipeline/optional compute, outputs finite evaluated pages/maps/meshes. Graphics receives products or emits demand to coordinator, never receives a generator callback | Existing P3 `clear-crest-48.8.3` extracts evaluation; B consumes products; `.9.4` supplies adapter | P3 surface parity, finite/periodic identity, chart seams and AO/normal determinism; no evaluator symbols in graphics dependency closure |
| Impostors/billboards and atlas mip sampling: renderer paths, `src/render/impostor_mips.h`, `shaders_vk/impostor_common.glsl`, `gbuffer.frag` | Prepared atlas/mesh/identity data; asset preparation makes atlas, graphics owns draw/residency. This pass preserves identity emission/subsurface semantics. Pending `matter.graphics.impostor-draw` | A extracts draw/material path; existing asset preparation supplies data; `.9.1` wires | Legacy impostor/mesh distance and view parity, atlas alpha/mips, no green bark regression; current mask fixture covers compositor only |
| Atmosphere: `src/render/vk_atmosphere.*`; `shaders_vk/atmosphere_{transmittance,multiscatter,sky_view,irradiance}.comp`, `atmosphere_common.glsl` | Typed atmosphere parameters, sun and camera altitude; graphics owns LUTs/SH and production. Pending `matter.graphics.atmosphere-producer`; composition consumes sky/SH | D extracts LUT producer; `.9.1` maps world-authored values without world headers | `atmosphere_tests.cpp`, `environment_sampling_test.comp`, LUT/SH energy and matched horizon/sun views |
| Cloud shadows: `src/render/vk_cloud_shadows.*`; `shaders_vk/cloud_shadow_{density,integrate,reproject}.comp`, `cloud_density.glsl`, `cloud_shadow_common.glsl` | Typed density/weather parameters, time/wind/seed, camera clipmaps; graphics owns tau/reprojection histories. Pending `matter.graphics.cloud-shadow-producer` | D extracts density/integrate/reproject; `.9.1` injects parameters | `cloud_shadow_tests.cpp`, `cloud_layer_tests.cpp`, `cloud_image_metrics_tests.py`; near/far/ping/edge/movement parity (constant tau consumer tested here) |
| Volumetrics/emitters: `src/render/vk_volumetrics.*`, `vk_emitter_gather.*`; `shaders_vk/vol_{density,scatter,integrate}.comp`, `vol_common.glsl` | Coordinator gathers typed world emitters; graphics receives explicit bounded emitter/medium/light arrays and owns froxel volumes/history. Pending `matter.graphics.volumetric-producer` | D extracts froxel producer; `.9.1` keeps world/ECS gathering outside graphics | `volumetric_quality_tests.cpp`, height layer, fog/cloud silhouette, temporal reset and emissive injection matched views |
| Water forward/field/mesh playback: `src/render/water_field_vk.*`, `water_field_vk_resources.*`, `water_animation_gpu.*`, `water_mesh_animation_playback.*`, `water_forward_reference.*`, `water_surface_reference.*`; `shaders_vk/water_forward.frag`, `water_surface.glsl`, `water_screen_space.glsl` | Prepared water mesh/field, phase/time/material/medium/environment, opaque depth/color, animation product. Graphics owns field textures and forward draw; generation owns hydrology/mesh evaluation. Pending `matter.graphics.water-forward` | E extracts surface/resources/playback; P4 `.8.4` and P6 `.8.6` supply animation/compute products; `.9.4` and `.9.1` wire | `gpu_water_render_tests.cpp`, `gpu_water_animation_render_tests.cpp`, field/surface/forward-reference tests; refraction/foam/underwater/transparency and animated boundary parity |
| Temporal reconstruction/DLSS: `src/render/vk_temporal.*`, `streamline_bridge.*`, renderer motion/jitter paths | Typed color/depth/motion/exposure, camera and explicit reset epoch; graphics owns temporal/SDK resources. Pending `matter.graphics.temporal-upscale` | E extracts adapters/settings from world_session includes; `.9.1` wires device/swapchain, no SDK callback into generation | History reset/camera cuts, motion vectors, resize, SDK available/unavailable behavior and matched static/moving native views |
| Sparse voxel/surface proxy rendering and shadows: `src/render/vk_sparse_voxel.*`, `vk_sparse_placement_update.inl`; `shaders_vk/sparse_voxel*.{vert,frag,comp,glsl}`, `sparse_hierarchy.comp`, `surface_proxy.*`, `surface_query.*`, `surface_detail.glsl`, `surface_filter.glsl` | Prepared sparse hierarchy/surface data, transforms/camera/materials; graphics owns traversal/residency/draw, no field generation. Pending `matter.graphics.sparse-surface-draw` | A extracts draw/representation handling; C owns shadow/RT interop; P6 supplies geometry compute products; `.9.1/.9.4` wire | `sparse_voxel_vk_tests.h`, shadow/temporal tests, `surface_proxy_tests.cpp`, `sparse_surface_query_tests.h`; mesh/proxy transition and lighting parity |
| GPU culling, draw selection, picking/selection overlay: renderer paths, `indirect_draw_runs.h`, `geometry_raster_adapter.h`; `shaders_vk/cull.comp`, `geometry_select.comp`, `visibility_id.*`, `visible_ids.comp`, `selection_line.*`, `geometry_cut.glsl` | Explicit draw bounds, transforms, selected IDs and cut planes; graphics owns indirect/ID resources, editor owns interaction. Pending `matter.graphics.gpu-scene-draw` | A extracts these draw consumers; `.9.1` wires legacy entrypoint/editor | Existing culling/draw/selection checks plus hierarchy visibility, mirrored transforms and readback/picking correctness |
| Skinning/rigid animation and bounds: `src/render/vk_animation_skinning.*`, `vk_animation_bounds.*`, `animation_{skin,rigid}_bridge.*`; `shaders_vk/animation_skin.comp` | Prepared skin/rig/clip/pose products; graphics owns GPU skin/bounds resources; animation evaluation and ECS remain external | Existing P4 `clear-crest-48.8.4` extracts typed bridges; A consumes resulting draw buffers; `.9.4` wires | `vk_animation_skin_compute_tests.cpp`, `vk_animation_bounds_tests.cpp`, rigid/skin bridge and phase B/C acceptance |
| GPU geometry/material generation: `src/render/gpu_meshing/*`, `geometry_world_runtime.*`; `shaders_vk/gpu_mesh_*.comp`, `solid_*.comp`, `face_material.comp` | Optional compute generator owns field/meshing/material evaluation and outputs geometry products. Graphics has no generation entrypoint | Existing P6 `clear-crest-48.8.6`; `.9.4` wires service separation | Compute field/meshing fixture parity and renderer-only linkage; not advertised as a graphics capability |

Probe/test shaders (`*_probe.comp`, `*_test.*`) belong to their corresponding
feature's validation. Device/context/swapchain, scene visibility/buffer resources,
frame transforms, statistics and profiling are covered by the R2 migration map
in [INTEGRATION.md](INTEGRATION.md) and `.9.1`; this leaf does not change them.

## Reproduction and evidence

The later production source move for inventory E is documented in
[native/WATER_TEMPORAL.md](src/native/WATER_TEMPORAL.md). Water resources,
playback, shaders, temporal state and Streamline implementation now live in
MatterGraphics, with legacy forwarding paths and exact renderer fragments.
This establishes source ownership; the retained native host coupling does not
advertise the pending standalone water/temporal capabilities above.

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test
```

The supported native wrapper resolves pinned MSVC/Vulkan SDK, configures Ninja,
compiles production GLSL to embedded SPIR-V, and runs the CTests. The optional
Vulkan target requires glslc and Python 3.9+ even when tests are disabled; the core
`matter_graphics` target has no Vulkan/shader-tool dependency. GLSL depfiles are
normalized for paths containing spaces. The independence test audits the actual
CMake graph, links, compiled dependency closure and the exact shader allowlist.

Recorded on task baseline `bd498c5af` plus this module change: MSVC 19.44.35211,
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

These are selected production **shading** parity fixtures. They do not prove
mesh rasterization, RT generation, full-scene temporal quality, water production,
or performance. The inventory above records the additional acceptance owed.

## Environment relocation evidence

The `.4.8` move uses native producer sources from
`6490dbab806876631465259cdf5b397f258c5837`. After normalizing include paths,
the shader-registry namespace and the three-assignment sun-direction adapter,
all six producer files match that reference. The four parameter headers and
the FogSettings/GPU-emitter blocks match byte for byte. See the
[native boundary](src/native/README.md#atmosphere-cloud-shadows-and-froxels).

Commands from the repository root (Windows PowerShell, except the stated WSL
build wrapper):

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test
# WSL: ./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
$smoke = '.\MatterEditor\build\cmake\windows-msvc\relwithdebinfo\vulkan_smoke_tests.exe'
$env:MATTER_VK_SMOKE_MODE = 'atmosphere'; & $smoke
$env:MATTER_VK_SMOKE_MODE = 'froxel-resize'; & $smoke
$env:MATTER_VK_SMOKE_MODE = 'rt-empty-tlas'; & $smoke
```

Recorded with MSVC 19.44.35211, toolset 14.44.35207, Windows SDK 10.0.26100.0,
Vulkan SDK 1.4.357.0, NVIDIA GeForce RTX 4090 driver 610.74/API 1.4.341,
Khronos validation enabled. All three smoke modes passed with **zero validation
errors**, including teardown. Existing Epic overlay manifest and unused shader
interface warnings appear separately in the full logs. All ten environment
SPIR-V outputs match the host's outputs byte for byte (`vulkan1.3`, `-O`).

- `atmosphere`: LUT/reference comparison, parameter invalidation, presentation,
  irradiance last-valid state, transaction failures, and six-elevation raster/RT
  direct-light and sky/SH receiver gates.
- `froxel-resize`: cloud bundle allocation/disable/failure and lifetime,
  density/prefix slab readback, reprojection/history/tile selection, numerical
  scattering references, and 160x90x128 → 80x45x64 → 320x180x256 → 240x135x192
  → 160x90x128 froxel transitions, with per-stage timestamps.
- `rt-empty-tlas`: retained scene with empty/nonempty geometry selection,
  initialized volume descriptors across frame slots, and reallocation.
- Module CTest: **8/9 pass**. Atmosphere, cloud-layer, volumetric-quality,
  lifetime, scene, composition, native Vulkan and dependency audit pass.
  The native Vulkan test includes the existing matched `physical-sky-product`,
  `environment-irradiance`, `cloud-optical-depth` and `integrated-volume` fixtures
  plus explicit unsupported-capability and retention/failure checks.

`matter_graphics_cloud_shadow` still fails three unchanged assertions: constant
slab transmittance, local/coarse no-overlap, and froxel-exit truncation. An
unmodified full test with exact original header blobs from the reference commit
reproduces the same three failures with matched
`/DWIN32 /D_WINDOWS /EHsc /O2 /Ob1 /DNDEBUG /std:c++17 /MD /Zi` flags and SDK.
The relocated full test gives the same result. Thus this failure predates the
move under these build inputs; the suite is **not** reported as passing.
Instrumenting the current full test shows local tau 5/2.5/1.5 and coarse tau 2,
but total tau 2/2/2. With `/Od /Ob0`, that diagnostic passes with totals
7/4.5/3.5. The unmodified test also passes on Linux g++ `-O2`. This establishes
optimization-dependent behavior, not its root cause. No algorithm, assertion,
tolerance or production optimization flag was changed to hide it.

[Full test logs, matched baseline/current compiler commands and shader hashes](tests/evidence/environment-relocation.txt)
retain the failure separately from GPU success. The static archive compiles
independently against the audited platform declarations; runtime smoke exercises
the moved code through the existing host's forwarding translation units. New
facade producer APIs, an independently linked device host and complete live-world
compatibility qualification remain integration work. This move does not
advertise the pending atmosphere/cloud/volume producer capabilities.

### Optimized CPU reference follow-up

`clear-crest-48.4.10` resolves the three historical CPU failures above. The
pinned MSVC 19.44.35211 `/O2 /Ob1` build miscompiles the reference helper's
unrolled accumulation: when the step count is divisible by four, the aggregate
member receives the correct local optical depth but the total uses a stale
zero register. A standalone reproducer and the emitted instructions establish
this cause; inlining can mask it.

The helper now accumulates in a local float and assigns the aggregate member
after the loop. It retains every per-step addition, input guard, step cap and
optical-depth clamp. No shader, production dispatch, assertion, tolerance or
optimization setting changes. The original full `cloud_shadow_tests.cpp` is
byte-identical. A separate regression exercises the out-of-line helper across
0–40 requested steps, froxel exits, invalid inputs and saturation; it reports
35 failures against the original header and passes with the fix.

The optimized native module now passes **10/10 CTests**, including the original
cloud-shadow suite and the new regression. Rebuilt native `froxel-resize`
passes with **zero validation errors**, including the seeded local/coarse
optical-depth checks, cloud/fog separation, reprojection and resize gates. Both
CPU suites also pass GCC `-O2` with address/undefined-behavior sanitizers. This
closes the numerical reference gap without extending the relocation's facade
or live-world qualification claims.

[Cause, commands, assembly and full qualification logs](tests/evidence/cloud-shadow-msvc.txt)
and the [standalone reproducer](tests/evidence/cloud-shadow-msvc-repro.cpp)
preserve the original failing form for compiler/toolchain follow-up. The pinned
toolchain remains supported with this bounded source workaround.
