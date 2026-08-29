# Vulkan Temporal Foundation Design

**Status:** Approved design direction; awaiting written-spec review (2026-07-13)

## Goal

Replace the MatterViewer OpenGL renderer with a Vulkan renderer, remove the
engine's implicit matrix transposition conventions, and establish a reliable
foundation for dense motion vectors, temporal antialiasing, and NVIDIA DLSS.

The first delivery phase ends with the existing GPU-driven raster and OptiX
lighting paths running through Vulkan on Windows with CUDA enabled. Motion
vectors and DLSS are subsequent phases, not prerequisites for declaring the
Vulkan migration complete.

## Decisions

- Vulkan is a clean cutover, not a permanent second renderer. The current
  OpenGL implementation remains available in Git history and reference
  captures, but no runtime backend selector or long-lived dual-backend
  abstraction will be built.
- The Windows NVIDIA path is the first supported target. It uses Vulkan 1.3,
  the Vulkan validation layers in development builds, CUDA Driver API, and
  OptiX. Linux support resumes after Windows feature parity; it must continue
  to compile for headless engine tests throughout the migration.
- Matrix storage is row-major and matrix algebra uses column vectors. Row-major
  storage is conventional and is not itself a defect. The defect being removed
  is the mixture of ordinary row-major instance transforms with camera matrices
  that depend on an implicit raylib/OpenGL transpose.
- The engine uses a right-handed world, positive Y up, and camera forward along
  negative view-space Z. Vulkan clip depth is `[0, 1]`. A negative Vulkan
  viewport height preserves the viewer's top-left framebuffer convention
  without embedding an API-specific Y flip in camera matrices.
- Unjittered camera matrices are authoritative. Temporal jitter is tracked in
  pixel units and applied only when producing a jittered projection for a
  render pass. This matches Streamline's requirement that common matrices be
  row-major and unjittered while jitter is supplied separately.
- Phase 1 does not attempt to preserve the currently faulty temporal history.
  Temporal accumulation remains disabled during motion until dense motion
  vectors and a reference TAA resolve validate the new frame data.

## Current-State Findings

### Matrix mismatch

Engine instance transforms already behave as affine row-major matrices using
column vectors. Translation occupies elements `[3, 7, 11]`, and composition is
ordinary `parent * child` multiplication.

The camera path in `MatterEngine3/src/render/raster_cull.h` differs:
`make_lookat` and `make_perspective` construct data that is intentionally
transposed in memory, `mul16(view, proj, vp)` depends on that representation,
and raylib/OpenGL upload completes the implicit conversion. Frustum extraction,
GPU culling, and the RT reprojection path consequently reason about matrices in
different representations. This is a credible cause of directionally wrong
reprojection and invented silhouettes during camera translation.

### Renderer coupling

- `MatterViewer/main.cpp` lets raylib own window creation and camera input.
- `MatterViewer/ui.cpp` uses the GLFW/OpenGL ImGui backends.
- `raster_composer`, `gpu_culler`, `probe_texture`, `tileset_gl_ctx`, and
  `rt_lighting` own OpenGL objects directly.
- `world_session.h` exposes raylib `Camera3D` in the public engine API.
- OptiX currently exchanges depth, G-buffer, and lighting images through
  CUDA-OpenGL registration.
- Several bake and geometry modules still use raylib POD types. Removing those
  unrelated uses is not required for the renderer cutover, but Vulkan rendering
  must not call raylib graphics or window functions.

## Architecture

### Application and Vulkan ownership

MatterViewer will create a GLFW window with `GLFW_NO_API`. A focused
`VulkanContext` owns the Vulkan instance, debug messenger, selected physical
device, logical device, queues, surface, swapchain, and per-frame command and
synchronization objects. `VulkanRenderer` owns rendering resources and passes;
it receives resolved scene data from the existing engine pipeline.

The cutover uses focused Vulkan modules rather than a generic cross-API
interface:

- `vk_context`: device, queues, swapchain, validation, resize and device loss.
- `vk_resources`: buffers, images, memory allocation, staging and debug names.
- `vk_pipeline`: SPIR-V modules, descriptor layouts, pipeline layouts and
  graphics/compute pipeline creation.
- `vk_scene_renderer`: mesh/instance upload, GPU culling, G-buffer raster,
  lighting composite and presentation.
- `vk_cuda_interop`: physical-device matching plus external memory and semaphore
  exchange with CUDA/OptiX.

This separation is internal organization, not a dual-backend abstraction.

### Frame data and matrix contract

The public API replaces `Camera3D` with engine-owned POD types:

```cpp
struct Float3 { float x, y, z; };

struct CameraDesc {
    Float3 position;
    Float3 target;
    Float3 up{0.0f, 1.0f, 0.0f};
    float vertical_fov_radians;
    float near_plane;
    float far_plane;
};

struct Mat4f {
    float m[16]; // row-major storage; column-vector algebra
};

struct CameraFrame {
    Mat4f world_to_view;
    Mat4f view_to_clip_unjittered;
    Mat4f world_to_clip_unjittered;
    Mat4f clip_to_world_unjittered;
    Float3 position;
    Float3 forward;
    Float3 right;
    Float3 up;
    float jitter_pixels[2];
    uint64_t frame_index;
    bool history_reset;
};
```

For matrices `A` and `B`, `A * B` means that `B` is applied first. Therefore:

```text
world_to_clip = view_to_clip * world_to_view
clip_position = world_to_clip * world_position
object_to_clip = world_to_clip * object_to_world
```

No renderer boundary accepts an untyped `float[16]` camera matrix. GPU upload is
explicit: GLSL buffers use row-major declarations where practical, and any
required packing is performed by a named conversion with tests. Persisted part
and instance transform bytes do not change because they already follow the
selected contract.

### Vulkan frame flow

Each frame executes:

1. MatterEngine resolves visible world instances and updates scene data.
2. Vulkan compute culling writes indirect draw commands and counters.
3. Vulkan rasterization fills linear HDR color/albedo, world normal, material
   properties, depth, and eventually motion vectors.
4. Vulkan signals an exported semaphore after interop inputs are ready.
5. CUDA waits, OptiX traces and denoises, and CUDA signals completion.
6. Vulkan waits, composites RT lighting, renders ImGui, and presents.

The first Vulkan milestone can present raster-only output, but Phase 1 is not
complete until the OptiX path also works with `HAVE_CUDA=1`.

### CUDA and OptiX interoperability

CUDA-OpenGL registration is replaced with Vulkan external-memory and
external-semaphore interop. Windows images intended for CUDA are allocated with
exportable Win32 memory handles. CUDA imports those allocations, maps image
subresources to CUDA arrays, and OptiX/CUDA consumes them. Exported Vulkan
semaphores establish ownership and ordering without `vkDeviceWaitIdle` or a
per-frame CPU round trip.

Vulkan physical-device selection must match CUDA by UUID/LUID before any shared
resource is created. If the selected Vulkan device cannot interoperate with the
CUDA device, RT lighting is reported unavailable and the raster renderer remains
usable. Handles are closed exactly once after import; resize destroys imported
CUDA objects before their Vulkan allocations.

### Shader and resource model

Runtime GLSL compilation is not introduced. Existing GLSL shaders are ported to
Vulkan GLSL, compiled to SPIR-V as build dependencies, and embedded using the
existing generated-header pattern. Descriptor sets replace implicit GL binding
state. Per-frame constants, scene buffers, G-buffer images, and composite inputs
have fixed descriptor-set ownership documented beside their C++ layouts.

The GPU-driven path retains compute culling and indirect drawing. HiZ remains
off by default until its known false positives have their own validation work;
the Vulkan migration must not silently broaden that debugging scope.

## Delivery Phases

### Phase 1: Vulkan and canonical matrices

1. Add `Mat4f`, camera construction, inverse, projection/unprojection, frustum,
   and packing tests. Migrate all camera consumers and remove the implicit
   transpose functions.
2. Replace the public `Camera3D` dependency with `CameraDesc`; move window and
   camera input ownership fully into MatterViewer.
3. Add the Vulkan build/toolchain, direct GLFW window, validation-enabled Vulkan
   context, swapchain, resize handling, and ImGui Vulkan backend.
4. Port mesh upload, instance buffers, compute culling, indirect rasterization,
   G-buffer creation, composite, and presentation.
5. Port RT lighting from CUDA-OpenGL to CUDA-Vulkan external interop and prove
   that the Vulkan and CUDA devices match.
6. Remove OpenGL renderer sources from the viewer build and delete obsolete GL
   shader/resource plumbing after parity tests pass.

Phase 1 acceptance criteria:

- Windows `viewer.exe` builds with `HAVE_CUDA=1`; the build log proves that the
  OptiX sources and Vulkan/CUDA interop module were compiled and linked.
- The viewer starts with Vulkan validation enabled and produces no validation
  errors during world load, resize, camera movement, RT toggle, or shutdown.
- CornellBox and one large instanced world render correctly in raster and full
  RT lighting modes.
- Scripted camera captures cover yaw, pitch, strafe, and forward/back movement.
  CPU projected points, GPU debug pixels, and frustum decisions agree.
- GPU-culling draw and cull counts match fixed expected captures within the
  documented tolerance; no objects flow in the opposite direction.
- The executable imports Vulkan images into CUDA without OpenGL, and repeated
  resize/shutdown cycles produce no leaked or double-closed resources.
- Headless non-rendering MatterEngine3 tests still pass.

### Phase 2: Temporal frame state and dense motion vectors

Track current and previous unjittered camera matrices, jitter, render extent,
previous object transforms, and camera cuts. Add a dense motion-vector
attachment covering both camera and object motion. Define one vector convention
at the shader boundary and provide scale/sign diagnostic views rather than
guessing from the resolved image.

Acceptance requires analytic tests and scripted captures: static-camera vectors
are zero, known one-pixel translations produce the expected vector, rotating and
forward/back cameras produce the expected radial fields, disoccluded pixels are
marked invalid, and matrix-based reconstruction agrees with the rasterized
vector field.

### Phase 3: Reference TAA

Build a simple non-vendor TAA resolve using the same color, depth, normal,
motion, jitter, and reset data intended for DLSS. It uses bounds checking,
bilinear history gathering, depth/normal rejection, and neighborhood clamping.
This is a diagnostic oracle and fallback, not a competing long-term upscaler.
The current ad hoc OptiX accumulation is removed or limited to stationary-camera
sampling once this path is validated.

### Phase 4: Streamline and DLSS Super Resolution

Integrate Streamline early in Vulkan initialization, query feature and device
requirements, supply unjittered common matrices plus pixel-space jitter, tag
color/depth/motion input and output Vulkan resources, and evaluate DLSS before
UI composition. Reference TAA remains the runtime fallback. DLAA can be exposed
through the same integration where supported.

### Phase 5: Optional DLSS Ray Reconstruction and Frame Generation

Ray Reconstruction is evaluated only after the OptiX path exposes the required
separated noisy signals and material/geometry guides. Frame Generation is a
separate product decision because it also affects presentation, Reflex, UI/HUD
separation, latency instrumentation, and overlay compatibility. Neither is part
of the initial DLSS Super Resolution milestone.

## Validation Strategy

### Matrix tests

- Identity, translation, rotation, composition and inverse golden cases.
- Look-at basis orthonormality and camera-position-to-origin checks.
- Vulkan near/far depth mapping to `0` and `1`.
- Project/unproject round trips at center, corners, and several depths.
- CPU frustum results checked against clip-space inequalities.
- Persisted instance-transform fixtures prove byte compatibility.
- A shader test writes transformed known vertices to a readback buffer and is
  compared with CPU `Mat4f` results.

### Renderer tests

- Validation-layer smoke test for create, three frames, resize and destroy.
- Deterministic render captures using fixed world, camera, exposure and random
  seed; compare structural buffers before comparing tone-mapped color.
- Culling counter readback for fixed scenes.
- RenderDoc/Nsight frame captures at the raster-only and RT-interoperability
  milestones.
- A debug-view selector for depth, normal, material ID, lighting, motion-vector
  magnitude/direction, history rejection and disocclusion.

### Temporal tests

- Scripted camera paths: stationary, strafe, yaw, dolly forward/back, orbit,
  teleport, resize and world reload.
- Motion-vector images and reprojected UVs are captured independently from the
  accumulated result.
- History is reset on cuts, resize, render-scale changes, world replacement and
  any frame where previous transforms are unavailable.

## Error Handling and Diagnostics

- Vulkan initialization failures include the failed call, `VkResult`, requested
  extensions/layers, selected adapter and driver.
- Validation messages are routed to the existing log and treated as test
  failures at error severity.
- Shader build failures identify the logical shader and compiler diagnostics.
- CUDA/Vulkan adapter mismatch and external-handle import failures disable RT
  lighting with a visible reason; they do not crash raster rendering.
- Every temporal resource carries extent and frame generation. Mismatches force
  a history reset rather than sampling stale memory.

## Non-goals

- A permanent OpenGL fallback or general-purpose cross-API rendering framework.
- Removing every raylib POD use from baking and geometry code during Phase 1.
- Fixing the existing HiZ false-positive issue as part of the Vulkan port.
- Tuning the existing reprojection until it looks acceptable without validated
  matrices and motion vectors.
- DLSS Frame Generation in the first DLSS milestone.
- Shipping Streamline development DLLs or enabling unsigned plugins in release
  builds.

## Principal Risks and Checkpoints

1. **Matrix cutover:** require all CPU golden tests before porting draw shaders.
2. **GPU-driven parity:** require raster/culling captures before adding CUDA.
3. **External interop:** prove one shared Vulkan image and semaphore round trip
   in a small test before modifying `RtLighting`.
4. **OptiX parity:** require resize and device-lifetime stress tests before
   deleting CUDA-OpenGL code.
5. **DLSS readiness:** do not integrate Streamline until reference motion-vector
   and TAA tests pass.

## Documentation References

- [NVIDIA Streamline Programming Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
- [NVIDIA Streamline DLSS Frame Generation Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)
- [Vulkan specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)

