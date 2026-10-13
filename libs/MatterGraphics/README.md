# MatterGraphics asset and lifetime facade

`matter_graphics` implements the frozen `matter::services::GraphicsService` v1
in `matter::graphics::Graphics`. It consumes immutable `AssetDocument` owners
and resolved bytes. It links contracts, neutral MathLib, and the existing pure
renderer `matrix_math.cpp` compiled directly from its source; no generation,
terrain, script host, ECS or legacy engine archive is needed.

The facade has fake-backend fixtures. `SceneBackend` additionally implements
prototype buffer preparation, resource budgets, scene expansion, visibility,
and retained render snapshots. `matter_graphics_vulkan` supplies the extracted
Vulkan allocation/staging/submission machinery with an explicit device binding.
Headless native tests check actual upload, readback and fence retirement;
deterministic device fixtures cover failures and budgets. The
[integration migration map](INTEGRATION.md) records provenance and downstream
legacy wiring. Resource tests do not establish rendered-image parity.

`VulkanComposition` additionally extracts production material/light/effect
composition and display into an offscreen pass with explicit evaluated inputs.
It owns images, pipelines, descriptors and GPU submissions. Fourteen native
matched image fixtures cover selected shading behavior. See the
[rendering contract, capability map and producer inventory](RENDERING.md) for
precise support and pending RT/environment/water integration; a composition
consumer does not imply its product producer is available.

`VulkanRaster` consumes `SceneFrame` and produces mesh G-buffers using the
production raster shaders, pipeline creation and command recorder. It accepts
uniform opaque PBR meshes, including nested assemblies and mirrored/nonuniform
placements. It applies the facade's inverse-transpose normal contract and packs
geometry per placement; this adapter makes no batching/performance claim.
Textured appearances fail explicitly. Native skin/water/VT/sparse extensions
remain in the existing host, which delegates to the same extracted mesh pass.
See the [native boundary and provenance](src/native/README.md).

The [standalone native sample](examples/README.md) connects imported and
hand-built assets to those production passes, with camera, directional lighting,
shared-instance updates, resource retirement and reproducible image captures.
It builds through the component's supported MSVC wrapper and is included in
the forbidden-dependency audit.

```cpp
#include <matter/graphics/vulkan_raster.h>
matter::graphics::VulkanRaster raster(binding);
matter::graphics::SceneFrame frame;
auto status = scene->frame(frame);
matter::graphics::RasterOutput gbuffer;
if (status.ok()) status = raster.render(frame, 128, 128, gbuffer);
// Feed albedo/normal/orm/depth/identity/materials into CompositionInputs;
// the coordinator supplies evaluated lighting/environment products separately.
```

The injected device must enable Vulkan 1.3 dynamic rendering, synchronization2,
shader demote, buffer device address, nonuniform sampled-image indexing, shader
draw parameters and indirect first-instance draws. `render` waits for retirement,
reads back native attachments, and commits output only on success. Identity
channels index returned material records and coordinator instance/path records.
The camera uses a reversed-Z, zero-to-one projection without an additional Y
flip, matching the host's negative viewport. Raster budgets bound dimensions,
triangle packing and padded GPU allocations. The native imported-fixture test
exercises real pixels and validation.

```cpp
#include <matter/graphics/graphics.h>
// Backend implementation supplies graphics allocation/scene/device operations.
matter::graphics::Graphics graphics(std::move(backend));
auto submitted = graphics.upload(job, {immutable_asset, resolved_bytes, {}}, {});
// Poll submitted.value()->handle on the application lane until ready/failure.
// Coordinator checks the echoed request/revision/epoch before updating scene.
```

All facade calls are thread confined. Cancellation tokens may be signalled from
another thread. No generation callback or source resolver enters graphics;
progress captures are retained through terminal completion but never invoked
on a backend/driver thread. Unknown required appearance/representation names or
versions fail before submission. Backend capabilities are the exact asset
validation capability set. Optional metadata cannot introduce visual semantics.

An upload retains immutable asset/payload owners until completion or cancellation
has drained. Upload inputs are validated (including byte identity) before backend
work. The current shared `AssetProduct` validator copies document data for that
validation; this facade establishes correctness, not upload performance targets.
Polling a ticket is explicit; pending results contain no resource. Terminal
results are retained until `release_ticket`, including failures. Repeated ready
polls return the same caller ownership, never another resource. A released caller
ownership cannot be reacquired by polling its old ticket. Releasing a completed
ticket does not release its resource. Slots are monotonically allocated and
handles are service scoped; foreign/stale/repeated releases fail explicitly.

Scene transactions use increasing revisions and validate the entire proposed
change before publication. Mesh and assembly prototypes belong to the uploaded
asset, and instance IDs belong to the scene. Removal of one instance preserves
other instances' shared prototype. Changing/replacing instances adjusts resource
references atomically. After caller release, existing scene instances may still
change their placement or prototype using their retained resource; creating a
new reference requires caller ownership. Duplicate upserts/removals and IDs
appearing in both lists are rejected; removing an absent instance is idempotent.
Transforms follow neutral `parent * local`, including mirrored/nonuniform
placement. The backend owns conversion to its normal/winding/device conventions.

Caller release, scene removal and device completion are independent. When no
caller or scene owner remains, `collect_garbage()` drops a resource only after
`Backend::resource_idle` reports completion of all submitted uses. Upload/resource
destructors drain their own outstanding work, including partial failures. The
facade destroys those objects before releasing borrowed input storage. Backend
request descriptors are borrowed through the submission call; backend copies
descriptors that outlive it and may borrow pointed immutable storage through
completion. Scene descriptors are likewise copied through their synchronous
transaction; resource pointers stay alive through backend scene replacement.
Backend updates must preserve previous state on failure or exception. Backend
exceptions become `backend_failure` at service boundaries.

`dispose()` and destruction synchronously drain the backend once, drop all scene
references, destroy outstanding uploads/resources, and invalidate all handles.
The backend object outlives its derived resources. Operations after disposal fail
explicitly; observations become empty. Cancellation/ticket release may block
while a backend upload destructor drains; asynchronous retirement can remain
inside backend allocation owners. No performance claim follows from fake tests.

Graphics observations carry instance ID, projected error in pixels, resident
bytes and optional versioned representation demand to the coordinator. Graphics
never schedules that demand. Malformed observations or unknown scene instances
are filtered; the frozen observation API has no diagnostic return channel.

## Focused checks

Supported native MSVC tooling (enables Vulkan and requires the pinned SDK validation layer):

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test
```

WSL equivalent:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/libs/MatterGraphics/build-windows.ps1")" \
  -Config RelWithDebInfo -Test
```

Independent GNU/Linux:

```bash
cmake -S libs/MatterGraphics -B /tmp/matter-graphics-facade -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-graphics-facade --parallel 2
ctest --test-dir /tmp/matter-graphics-facade --output-on-failure -j 1
```

For the native Vulkan tests, configure with `-DMATTER_GRAPHICS_VULKAN=ON` and an
installed Vulkan SDK. On Linux with the repository headers and a system loader:

```bash
cmake -S libs/MatterGraphics -B /tmp/matter-graphics-vulkan -G Ninja \
  -DBUILD_TESTING=ON -DMATTER_GRAPHICS_VULKAN=ON -DCMAKE_BUILD_TYPE=Debug \
  -DVulkan_INCLUDE_DIR="$PWD/third_party/Vulkan-Headers/include" \
  -DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so.1
cmake --build /tmp/matter-graphics-vulkan --parallel 2
ctest --test-dir /tmp/matter-graphics-vulkan --output-on-failure -j 1
```

The raster fixture requires Vulkan 1.3 and the features above. The existing
resource/composition fixture retains its Vulkan 1.2 timeline-semaphore device
requirement. Both fail if no suitable graphics device is available. Validation is enabled when installed; set
`MATTER_GRAPHICS_REQUIRE_VALIDATION=1` to require it (the MSVC wrapper does so).
`BUILD_TESTING=OFF` with Vulkan disabled needs no Python/Node. The optional
Vulkan target requires glslc and Python 3.9+ to compile/embed production shaders.
With testing enabled, CMake 3.27+ and Python 3.9+ are required for the
standard-library File API audit (no Python package dependency).
Public headers compile individually. The audit inspects actual compiled targets,
dependencies, sources, linker fragments and compiler-discovered headers with
procedural/terrain/host targets absent. Only three exact legacy math source/header
paths are admitted; adding PartStore or world/device context fails the audit.

## Concrete scene/resource backend

```cpp
#include <matter/graphics/scene_backend.h>
auto backend = std::make_unique<matter::graphics::SceneBackend>(resource_device);
auto* scene = backend.get(); // Borrowed until Graphics destruction.
matter::graphics::Graphics graphics(std::move(backend));
// Use the same upload/poll/update_scene/set_camera/set_lights facade calls.
matter::graphics::SceneFrame frame;
auto status = scene->frame(frame);
// A render-pass adapter consumes frame.visible and retains its buffer owners
// until submitted device work retires. The frame also owns its camera/lights.
```

`ResourceDevice` is an explicit native allocation/upload boundary, with no scene
or world callbacks. A device adapter returns a `DeviceBuffer` owner before
submission, retains its input byte owner until the last read, reports completion,
resident allocation size and idle state, and drains submitted uses on destruction.
It may attach native vertex/index/storage handles or backend acceleration data to
its derived buffer owners. `drain()` is synchronous. A null device fails explicitly.
`VulkanResourceDevice` implements this seam in the optional Vulkan target:

```cpp
#include <matter/graphics/vulkan_resources.h>
auto resource_device = std::make_shared<matter::graphics::VulkanResourceDevice>(
    matter::graphics::VulkanDeviceBinding{
        physical_device, device, queue, queue_family, device_owner, queue_mutex});
```

The binding owns no window or device setup. Its shared owner must keep the device
and physical-device instance alive, and every external queue user must share the
same mutex. Allocation and scene methods are thread confined. Vertex, index and
opaque texture payload buffers use device-preferred memory and host-visible staging;
noncoherent mappings flush/invalidate the whole allocation. `VulkanBuffer` exposes
the native handle. Submit every native use through `VulkanResourceDevice::submit`
with all referenced buffer owners; it retains buffers and command storage until
the fence completes, including when the caller drops the submission ticket.
A raw externally submitted use cannot be inferred or retired by this adapter.
Buffer destruction drains outstanding upload work; device destruction drains all
registered submissions. `readback` synchronously copies into a host-visible
allocation and returns exact bytes after completion. Unknown/foreign buffer owners
fail before recording. Device-loss/error paths wait before releasing dependencies. If neither a fence
nor device idle/loss establishes completion, submission storage and its device
owner remain retained; an unproven driver failure must not free in-flight memory.

Meshes upload once per asset/prototype, including position, index, normal, tangent
and UV streams. Resolved payloads retain exact bytes and material/texture metadata
for the rendering adapter; texture decoding, images, shaders and PBR evaluation
remain rendering work. Unknown required capabilities fail. Core asset/PBR support
here denotes preserved scene/buffer inputs, not independently rendered appearance.

`SceneBackend` expands nested assemblies with bounded instance/depth budgets and
parent-before-local composition regardless of stored parent order. Each draw has
the coordinator's instance ID, its assembly child path, shared mesh buffers,
world matrix, inverse-transpose normal matrix and mirrored flag. The rendering
adapter combines mirrored parity with prototype winding and tangent handedness.
Asset top-level instances are not inserted implicitly: the application supplies
the scene update. No generation, source lookup or implicit world query occurs.

Scene updates prepare a complete replacement before publication. Visibility uses
the existing renderer's normalized frustum planes and a conservative object-space
support-point AABB test, valid for shear, mirrored/nonuniform scale, and standard
or reversed Vulkan zero-to-one depth. No second LOD rule is introduced. Camera
and placement matrices use the same explicit region; the double-precision region
origin travels with the snapshot and is never silently narrowed into vertices.

Requested upload bytes and actual driver allocation sizes count against residency,
including pending uploads, old frame snapshots, and buffers retained directly by
render submissions after their assets are released. Device completion does not
delete scene references, and instance removal does not delete shared geometry.
Shutdown drains the device; retained snapshots keep their allocations and device
owner alive until their own release. Snapshots obtained before shutdown are for
retirement only: submitting new work after shutdown is invalid. Visible-instance
observations report resource bytes without scheduling generation or inventing LOD
demand; repeated assembly children report one observation per coordinator ID.

| Fixture IDs | Assertions |
| --- | --- |
| `GFX-SUBMIT/PENDING/READY/IMPORTED` | Hand-built textured asset and checked-in `fixture.repeated-assembly` JSON, explicit completion with shared mesh/assembly prototypes |
| `GFX-SHARING/CHANGE/AFFINE/SCENE` | Shared instance deletion, placement/replacement, mirrored nonuniform transform, stale revisions, validation/backend rollback |
| `GFX-OWNER/INPUTS/RELEASE/RETIRE` | Immutable assets/bytes outlive device/upload owners; caller/scene/device retirement separated |
| `GFX-ENVELOPE/HANDLE/VALIDATE/CAPS/PAYLOAD` | Every envelope field, foreign/stale/type-confused handles, invalid geometry, exact capabilities and missing/corrupt bytes |
| `GFX-FAIL/CANCEL/TICKET/CONTROL/CALLBACK` | Submission/completion errors and exceptions, malformed completion, cancellation before/during completion, ticket disposal and capture release |
| `GFX-CAMERA/LIGHTS/DEMAND/BUDGET/DISPOSE` | Camera/light validation and rollback, data-only demand, owner/scene ceilings, busy-device and pending-upload disposal |
| `GFX-INDEPENDENT-GRAPH` | Facade, scene, contracts, shared pure math and optionally the extracted Vulkan adapter/loader; forbidden sources/links/compiler dependencies absent |
| `VK-UPLOAD/READBACK/SCENE/SHARING/CULL/FENCE/RETIRE/DISPOSE/OWNERSHIP` | Real device bytes, shared geometry, visibility, deterministically pending GPU use, retirement, pending disposal, foreign adapter rejection and validation-clean teardown |
| `SCENE-UPLOAD/BUFFERS/MATERIAL` | Pending/ready buffer completion, little-endian indices, appearance metadata retention |
| `SCENE-ASSEMBLY/AFFINE/SHARING` | Nested repeated prototypes, unsorted parents, mirrored/nonuniform placement and normal frames |
| `SCENE-CULL/PERSPECTIVE/CAMERA/REGION/LIGHTS` | Six planes, crossing/sheared bounds, standard/reversed depth, explicit region and immutable camera/light state |
| `SCENE-LIMIT/ROLLBACK/BUDGET/PENDING-BUDGET` | Bounded expansion, stale/malformed updates, requested/allocated bytes and delayed allocation growth |
| `SCENE-LIFETIME/SNAPSHOT/BUFFER-OWNER` | Caller/scene/frame/device release order and reclaimed/reused residency |
| `SCENE-FAIL/CANCEL/CAPS` | Partial failure/exception cleanup, cancellation and unsupported capability rejection |
