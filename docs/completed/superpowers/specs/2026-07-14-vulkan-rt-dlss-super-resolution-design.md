# Vulkan RTX and DLSS Super Resolution Design

## Goal

Deliver a Vulkan-first RTX rendering path with customizable engine-owned ray
tracing and NVIDIA DLSS Super Resolution through Streamline. The initial
vertical slice adds native Vulkan ray-traced sun shadows to the existing PBR
raster lighting, renders the opaque world at the DLSS input resolution, and
upscales before UI composition.

## Scope and sequencing

This work is deliberately staged so ray tracing is built against the final
temporal render contract rather than a temporary full-resolution path:

1. Establish a Vulkan temporal frame contract: jittered camera matrices,
   previous-frame history, rigid-instance transform history, opaque-world
   motion vectors, internal render extent, and reset/disocclusion rules.
2. Integrate Streamline and DLSS Super Resolution using those Vulkan
   resources. Native-resolution presentation remains the required fallback.
3. Add a native Vulkan ray-tracing shadow pass that consumes the same scene
   geometry and transform data as the raster renderer.
4. Make the ray-tracing pass configurable and extendable to GI and
   reflections without changing the DLSS resource contract.

DLSS Ray Reconstruction and Frame Generation are expressly out of scope for
this phase. The existing OptiX/CUDA implementation remains untouched as a
legacy OpenGL path; the Vulkan viewer must not depend on CUDA-GL interop.

## Architecture

### Temporal opaque-world contract

`WorldSession` owns a `VulkanTemporalState` that advances only after a frame
is successfully submitted and presented. It contains current and previous canonical
Vulkan-ZO camera matrices, Halton-sequence jitter in input pixels, prior
render extent, a monotonic frame index, and reset state. Resize, world load,
camera cut, renderer reset, or a missing previous transform invalidates
history for exactly one frame.

Failed command recording, UI recording, submission, acquire, or presentation
never publishes the candidate frame as history. The next successful frame uses
the last presented state or forces a reset; it cannot consume a transform,
jitter, or DLSS frame token from a failed frame.

The current and previous object-to-world transforms are tracked by stable
resolved-instance identity. A newly visible or changed-topology instance gets
zero motion for one frame. Motion is generated only for opaque world geometry;
UI is rendered after DLSS and is never included in depth or velocity inputs.

The Vulkan raster G-buffer keeps albedo, normal/emission, ORM, and depth, and
adds an `R16G16_SFLOAT` velocity attachment. The vertex shader projects the
same local position with the current jittered `world_to_clip` and prior
jittered `world_to_clip * previous_object_to_world`. The fragment shader
writes current-to-previous velocity in input pixel space. Background and
invalid history write `(0, 0)`.

The world is rendered at an internal extent selected from Streamline DLSS
optimal settings. The swapchain extent remains the output extent. When DLSS is
disabled, unavailable, rejected by Streamline, or the feature is not supported
by the current adapter, internal and output extent are equal and the Vulkan
composite presents directly.

For every recorded frame, `VulkanTemporalState` produces an explicit
`sl::Constants` payload and the Streamline bridge allocates a unique,
never-reused attempt `sl::FrameToken` before DLSS evaluation. This token is
independent of the temporal state: an evaluation followed by failed UI
recording, submission, acquire, or presentation consumes/drops its token,
does not publish its temporal candidate, and forces the next DLSS frame to
reset history. Constants use Streamline's
required row-major, **unjittered** current/previous clip transforms,
`clipToPrevClip`, inverse transforms, input-pixel jitter offsets,
`mvecScale = (1 / internal_width, 1 / internal_height)`, depth-inverted flag,
camera-motion flag, `motionVectorsJittered = true`, and the reset flag. The
raster velocity shader deliberately uses jittered current/previous projections;
the matching flag prevents Streamline from applying a second jitter correction.

### Streamline and DLSS SR

Streamline is a Windows-only optional runtime dependency. This renderer uses
the **native Vulkan/manual-hooking** integration, not the global interposer
creation path: `slInit` with `eUseManualHooking` and requested DLSS SR feature
occurs before any Vulkan instance, device, surface, swapchain, acquire, or
presentation call. `vk_context.cpp` queries `slGetFeatureRequirements` before
physical-device selection, unions every requested instance/device extension,
Vulkan 1.2/1.3 feature chain, and additional queue count with the engine's
requirements, then creates the native instance/device. It calls
`slSetVulkanInfo` immediately after native device/queue creation.

The manual integration securely loads Streamline's Vulkan dispatch and routes
all required proxy entry points (`vkCreateSwapchainKHR`,
`vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`,
`vkQueuePresentKHR`, surface creation/destruction, and device wait idle) through
the Streamline proxy only when Streamline is active. The presentation path is
single-funnel and invokes the common plugin `presentCommon()` exactly once per
active frame, including resize transitions. Test-only Vulkan instances bypass
Streamline and retain their existing direct loader path.

The executable ships legally obtained, signature-verified Streamline and DLSS
SR runtime artifacts next to the viewer. Source builds remain possible without
those artifacts and select the native fallback.

Each in-flight frame slot owns a DLSS input HDR image at the internal extent
and a distinct full-output-extent `R16G16B16A16_SFLOAT` DLSS output image.
Both have the image usages Streamline requires plus sampled/transfer access for
engine composition. Depth is created with a sampled depth view in addition to
its attachment view; velocity is a sampled `R16G16_SFLOAT` image. The renderer
retains all five DLSS images/views (HDR input, depth, velocity, output, and
exposure when enabled) until that frame slot fence is observed complete.

After opaque rendering and RT composite, command recording transitions tagged
inputs and the output to the exact layouts/accesses declared in their
`sl::Resource` tags, sets the constants/frame token, tags HDR color, nonlinear
depth, motion vectors, jitter, render/output extents, exposure, and reset, then
evaluates DLSS SR on the acquired Vulkan command buffer. It restores command
buffer state and transitions the output to sampled/transfer source for a
full-resolution composite/copy to the swapchain before UI. A feature query
selects the requested quality preset and supplies the internal extent. Runtime
failures disable DLSS for the session, preserve the direct-composite fallback,
and expose a clear diagnostic rather than aborting rendering.

The first supported mode is DLSS Super Resolution only. The UI reports
`Native`, `DLSS Quality`, `Balanced`, and `Performance`; a selected but
unavailable DLSS mode displays the native fallback status. No vendor or driver
blacklist is hard-coded by the engine; Streamline feature support determines
availability.

### Native Vulkan ray tracing

The Vulkan renderer enables and validates `VK_KHR_acceleration_structure`,
`VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`,
`VK_KHR_spirv_1_4`, and `VK_KHR_shader_float_controls`, plus buffer device
address. Its device feature chain includes
`VkPhysicalDeviceBufferDeviceAddressFeatures`,
`VkPhysicalDeviceAccelerationStructureFeaturesKHR`, and
`VkPhysicalDeviceRayTracingPipelineFeaturesKHR`. Pipeline creation reads
`VkPhysicalDeviceRayTracingPipelinePropertiesKHR` and obeys its shader-group
handle size, handle alignment, base alignment, recursion depth, and stride
limits when creating the SBT. If any requirement is absent, raster plus
DLSS/native fallback remains functional.

BLAS objects are built from **per-part pinned device-addressable geometry
buffers**, not the renderer's re-growable shared raster vertex buffer. A part's
geometry address remains valid until its BLAS is destroyed; releasing or
reuploading a part retires its BLAS and pinned buffers only after the relevant
frame-slot fence. TLAS instances are rebuilt or updated from the resolved
instance transforms using the same stable part hashes and transform convention
as raster. All acceleration-structure scratch, build, SBT, and retained
resources obey the existing per-frame-slot fence lifetime contract.

The first ray pipeline has a ray-generation shader, miss shader, and
shadow-any-hit/closest-hit behavior sufficient to produce an internal-
resolution shadow visibility buffer. It consumes depth/world reconstruction
and scene lights, then modulates the existing raster/PBR composite. The
engine-owned `VulkanRayTracingSettings` includes enabled state, render scale,
max trace distance, ray bias, shadow samples, and debug output. Future GI and
reflection ray types add SBT records and output attachments without changing
motion vectors, temporal history, or Streamline tagging.

### Command ordering and ownership

For each frame, command recording order is:

1. Acquire frame slot and derive candidate jittered current/previous temporal
   state without publishing it.
2. Record compute culling and barrier into opaque G-buffer rasterization.
3. Record acceleration-structure update/build barriers as needed.
4. Record native RT shadow pass and barrier into HDR composite.
5. Tag/evaluate DLSS SR or record direct native composite.
6. Record UI over the output image, then submit/present through the Streamline
   presentation funnel and publish the candidate temporal state only on success.

No CPU readback, immediate submit, or wait is introduced between culling,
raster, RT, DLSS, and presentation. All history transitions and images remain
owned by the same acquired `VulkanFrame` command buffer.

## Validation and acceptance criteria

- Canonical Vulkan-ZO and jitter math has unit coverage. A static camera
  converges to zero motion after its reset frame; camera and rigid-instance
  motion vectors have deterministic expected values.
- Resize, camera cut, world reload, instance creation/removal, and renderer
  recovery reset DLSS history without stale velocity or use-after-fence.
- DLSS SR feature discovery, Vulkan extension negotiation, resource tagging,
  fallback behavior, and output dimensions have deterministic seam tests.
- Native Vulkan RT capability gating, BLAS/TLAS lifecycle, shader pipeline
  creation, ray visibility output, and teardown are covered by Vulkan smoke
  tests. Unsupported hardware is a clean skip/fallback, never a crash.
- CUDA-enabled Windows build remains supported. Default, cull, raster,
  Streamline-disabled, and RT-disabled smoke modes have zero Vulkan validation
  errors. DLSS-enabled testing runs where its legal runtime artifacts and an
  RTX adapter are present.
- The viewer exposes current render mode, internal/output resolution, DLSS
  availability/reason, RT settings, and frame cadence so quality/performance
  comparisons are auditable.

## Non-goals

- DLSS Ray Reconstruction, Frame Generation, optical flow, and UI motion
  vectors.
- Replacing all existing lighting with path tracing in the first vertical
  slice.
- Retaining or extending CUDA-GL interop for the Vulkan viewer.
- Changing authored scene content, LOD policy, or the OpenGL renderer as part
  of this phase.
