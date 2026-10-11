# Native renderer migration boundary

Task `clear-crest-48.4.2` extracts concrete scene/visibility and Vulkan buffer
resource machinery into this module. Supervisor directive
`msg-5142619b351a4c0085f68c36a0d6dfd9` permits module-local extraction while the
original implementations/callers remain until downstream integration task
`clear-crest-48.9.1` rewires them. External source moves, legacy forwarding, central
manifests and live world integration remain owned by that downstream task. It is
not a prerequisite for the standalone graphics module.

The delivered shared contract gate and Graphics facade are prerequisites already
in this branch's ancestry. Public contracts and central source manifests are
unchanged. This document records integration work, not authorization to perform
it from the graphics module's task.

| Existing source and symbols | Local entrypoint / intended ownership | Required `.9.1` change |
| --- | --- | --- |
| `MatterEngine3/src/render/matrix_math.cpp/.h`: `extract_frustum_planes_zo` and camera math; `MatterEngine3/include/matter/math_types.h`: `Mat4f` | Currently compiled once from its original source by local CMake; `SceneBackend::set_camera` explicitly converts neutral row-major values | Move/share the pure renderer implementation under graphics and leave ordered compatibility headers if needed. Remove duplicate registration from central renderer targets when linking `matter_graphics`. Keep existing matrix test oracles. |
| `MatterEngine3/src/render/raster_cull.h`: `aabb_culled` | `src/scene_backend.cpp`: conservative support-point AABB visibility, using the shared plane extraction | Replace legacy consumers through an ordered pure helper boundary. The old header includes `part_store.h`; it cannot be a graphics dependency. Preserve its conservative visibility oracle. |
| `MatterEngine3/src/render/part_store.*`: `PartStore`, `LoadedPart`, `script_host::BakedGeometry`; `vk_instance_cache.*`: `VulkanInstanceCache` | `SceneBackend::begin_upload/update_scene`, `SceneAsset`, `MeshBuffers`, `DrawInstance` | Coordinator converts owned neutral asset/prototype data and explicit scene updates. Parent transforms and prototype identity enter as data; graphics never reads PartStore, procedural hashes, providers or live sectors. Replace legacy instance resolution at the world entrypoint. |
| `MatterEngine3/src/render/vk_resources.*`: `VkBufferResource`, `create_buffer`, `upload_buffer`, `submit_immediate`, allocation retention; `vk_device_internal.h` lifetime/retention access | `src/vulkan_resources.cpp`: extracted allocation/memory selection, mapped coherency, staging upload/readback and fence retention; `VulkanResourceDevice`, `VulkanBuffer`, `VulkanSubmission` | Rewire legacy buffer consumers to the extracted implementation and remove the superseded definitions in dependency order. The standalone target deliberately excludes original `vk_resources.cpp`. Dedicated command pools/fences and explicit dependency owners replace device-global retention. Allocation bytes include live upload staging; error paths wait before freeing. |
| `MatterEngine3/src/render/vk_context.cpp`: `VulkanDevice::Impl` and device access/retention definitions; `streamline_bridge.*`: `StreamlineBridge` | `VulkanDeviceBinding` injects physical/device/queue handles plus lifetime owner and shared queue mutex; native tests create a headless device directly | Supply the binding from the live device owner. Split the device setup from `streamline_bridge.h`'s `matter/world_session.h` include and its ECS/terrain/streaming header closure. Supply DLSS render settings as graphics values. Merely linking `vk_context.cpp` drags forbidden dependencies through that closure. |
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

The optional `matter_graphics_vulkan` target compiles the extracted native buffer
implementation and links only this module, contracts/pure math and the Vulkan
loader. Headless tests perform actual staging upload and exact device readback,
share native buffers across synthetic scene instances, change visibility, hold
a GPU submission pending with a host-signalled timeline semaphore, release all application references,
and verify fence-based retirement and disposal with uploads outstanding. Native
MSVC checks require Khronos validation and reject validation errors through device
teardown. The deterministic device fixtures additionally cover allocation failures,
exceptions and memory-budget boundaries without forcing real device loss.

This extraction supplies conservative CPU scene visibility and native Vulkan
buffer ownership, uploads, readback, submission and retirement. GPU indirect
culling parity, acceleration structure construction, virtual-texture residency,
texture decoding/images and material/effect drawing remain renderer tracks; their
structures stay graphics-owned and are never added to the portable contracts.
No rendered-image or performance parity is claimed by these resource tests.
After downstream wiring, run the existing native Vulkan resource/device lifetime,
GPU culling and rendered-scene oracles against the live entrypoint as well.
