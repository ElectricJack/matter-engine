# Native renderer migration boundary

Task `clear-crest-48.4.2` prepares concrete local scene/resource logic and adapter
entrypoints. The supervisor's directive `msg-1f1d454b5fc2491b8a6d4ed889b4d038`
reserves external source moves, legacy forwarding, and VulkanDevice/Streamline/
world rewiring to ordered integration task `clear-crest-48.9.1`.

The delivered shared contract gate and Graphics facade are prerequisites already
in this branch's ancestry. Public contracts and central source manifests are
unchanged. This document records integration work, not authorization to perform
it from the graphics module's task.

| Existing source and symbols | Local entrypoint / intended ownership | Required `.9.1` change |
| --- | --- | --- |
| `MatterEngine3/src/render/matrix_math.cpp/.h`: `extract_frustum_planes_zo` and camera math; `MatterEngine3/include/matter/math_types.h`: `Mat4f` | Currently compiled once from its original source by local CMake; `SceneBackend::set_camera` explicitly converts neutral row-major values | Move/share the pure renderer implementation under graphics and leave ordered compatibility headers if needed. Remove duplicate registration from central renderer targets when linking `matter_graphics`. Keep existing matrix test oracles. |
| `MatterEngine3/src/render/raster_cull.h`: `aabb_culled` | `src/scene_backend.cpp`: conservative support-point AABB visibility, using the shared plane extraction | Replace legacy consumers through an ordered pure helper boundary. The old header includes `part_store.h`; it cannot be a graphics dependency. Preserve its conservative visibility oracle. |
| `MatterEngine3/src/render/part_store.*`: `PartStore`, `LoadedPart`, `script_host::BakedGeometry`; `vk_instance_cache.*`: `VulkanInstanceCache` | `SceneBackend::begin_upload/update_scene`, `SceneAsset`, `MeshBuffers`, `DrawInstance` | Coordinator converts owned neutral asset/prototype data and explicit scene updates. Parent transforms and prototype identity enter as data; graphics never reads PartStore, procedural hashes, providers or live sectors. Replace legacy instance resolution at the world entrypoint. |
| `MatterEngine3/src/render/vk_resources.*`: `VkBufferResource`, `create_buffer`, `upload_buffer`, `submit_immediate`, allocation retention; `vk_device_internal.h` lifetime/retention access | Implement `ResourceDevice::upload/drain` and a native `DeviceBuffer` over the shared implementation, owned by graphics/backend | Move the existing implementation rather than copying it. Retain bytes through upload completion, publish failure diagnostics, retain allocations through fences, and preserve failed-submission/device-loss teardown. `DeviceBuffer::resident_bytes` reports allocation bytes, not just payload bytes. |
| `MatterEngine3/src/render/vk_context.cpp`: `VulkanDevice::Impl` and device access/retention definitions; `streamline_bridge.*`: `StreamlineBridge` | Native graphics device/platform adapter below `ResourceDevice` | Split the device setup from `streamline_bridge.h`'s `matter/world_session.h` include and its ECS/terrain/streaming header closure. Supply DLSS render settings as graphics values. Merely linking `vk_context.cpp` drags forbidden dependencies through that closure. |
| `MatterEngine3/src/render/vk_scene_renderer.*`, `shaders_vk/cull.comp` | A render-pass backend consumes `SceneFrame` mesh owners, transforms/normal frames, camera and lights | Convert the current frame/draw/culling path to explicit scene snapshots. Retain the canonical `lod_distance.h` rule; do not infer generation availability in culling. This task supplies CPU conservative visibility, not a replacement or parity proof for GPU indirect culling. |
| Renderer BLAS/TLAS, VT residency, material pipelines and effects | Graphics-owned native resource/render modules using the same asset/frame lifetimes | Preserve native features through explicit capabilities; unsupported representations fail or require a separately tested fallback. Do not move these structures into portable contracts or invoke a generator from graphics. |
| Central CMake/source manifests and Matter world/editor entrypoints | Integration owner only | Register shared graphics implementation exactly once, wire public uploads/scenes, and leave independently disabled procedural/terrain/QuickJS builds possible. |

## Verification boundary

The synthetic device tests execute the concrete scene backend, not a fake
`Backend`. They exercise real buffer packing, prototype sharing, bounded assembly
expansion, transforms, visibility, rollback, pending/failing allocation and
retirement. Only the device allocator/fence completion is simulated.

The independent CMake build links the facade, scene backend, contract runtime and
pure renderer matrix implementation. Its audit examines every compiled target,
source and include directory, actual linker library fragments, and Ninja's
compiler-discovered dependencies. The only legacy inputs admitted are exactly
`matrix_math.cpp`, `matrix_math.h`, and `matter/math_types.h`. Procedural, terrain,
QuickJS, ECS, PartStore, VulkanDevice and the monolithic engine targets are absent.

**Unmet extraction acceptance:** actual current Vulkan allocation/upload, GPU
culling, renderer acceleration/residency and a native graphics-only GPU execution
have not been extracted or demonstrated. Those require the authorized `.9.1`
source/device separation above plus the rendering/material track. The passing
CPU/native-compiler fixtures do not prove GPU correctness, visual parity,
performance, or complete replacement of the current renderer. Do not mark the
full renderer extraction complete based on this branch alone.

After integration, run the existing native Vulkan resource/device lifetime and
culling oracles, plus the graphics-only synthetic/imported scene through the
real device adapter. Require actual buffer completion/readback, shared geometry
through removal, mirrored/nonuniform visible results, busy-device retirement,
shutdown with work pending, and an audited source/link graph with generation,
terrain and QuickJS disabled. Preserve the synthetic failure fixtures to cover
deterministic fault cases alongside those native checks.
