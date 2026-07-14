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
is successfully recorded. It contains current and previous canonical
Vulkan-ZO camera matrices, Halton-sequence jitter in input pixels, prior
render extent, a monotonic frame index, and reset state. Resize, world load,
camera cut, renderer reset, or a missing previous transform invalidates
history for exactly one frame.

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

### Streamline and DLSS SR

Streamline is a Windows-only optional runtime dependency. It is initialized
before Vulkan device creation so its queried required Vulkan instance/device
extensions, feature bits, and queue requirements participate in physical-device
selection and logical-device creation. The executable ships the legally
obtained Streamline and DLSS SR runtime artifacts next to the viewer; source
builds remain possible without those artifacts and select the native fallback.

After opaque rendering and before UI, the renderer tags the HDR color,
nonlinear depth, motion vectors, jitter offsets, render/output extents,
exposure, and reset flag. It evaluates DLSS SR on the acquired Vulkan command
buffer at the correct Streamline marker. A feature query selects the requested
quality preset and supplies the internal extent. Runtime failures disable DLSS
for the session, preserve the direct-composite fallback, and expose a clear
diagnostic rather than aborting rendering.

The first supported mode is DLSS Super Resolution only. The UI reports
`Native`, `DLSS Quality`, `Balanced`, and `Performance`; a selected but
unavailable DLSS mode displays the native fallback status. No vendor or driver
blacklist is hard-coded by the engine; Streamline feature support determines
availability.

### Native Vulkan ray tracing

The Vulkan renderer enables and validates the KHR acceleration-structure,
deferred-host-operations, buffer-device-address, ray-tracing-pipeline, and
SPIR-V ray-tracing capability set. If any required feature is absent, raster
plus DLSS/native fallback remains functional.

BLAS objects are built per immutable uploaded part mesh and retained until that
part is released. TLAS instances are rebuilt or updated from the resolved
instance transforms using the same stable part hashes and transform convention
as raster. All acceleration-structure scratch, build, and retained resources
obey the existing per-frame-slot fence lifetime contract.

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

1. Acquire frame slot and derive jittered current/previous temporal state.
2. Record compute culling and barrier into opaque G-buffer rasterization.
3. Record acceleration-structure update/build barriers as needed.
4. Record native RT shadow pass and barrier into HDR composite.
5. Tag/evaluate DLSS SR or record direct native composite.
6. Record UI over the output image, then submit/present.

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
