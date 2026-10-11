# MatterGraphics asset and lifetime facade

`matter_graphics` implements the frozen `matter::services::GraphicsService` v1
in `matter::graphics::Graphics`. It consumes immutable `AssetDocument` owners
and resolved bytes. It links contracts, neutral MathLib, and the existing pure
renderer `matrix_math.cpp` compiled directly from its source; no generation,
terrain, script host, ECS or legacy engine archive is needed.

The facade has fake-backend fixtures. `SceneBackend` additionally implements
prototype buffer preparation, resource budgets, scene expansion, visibility,
and retained render snapshots. Its device fixtures simulate asynchronous buffer
completion/failure; they do not establish actual Vulkan upload or drawing.
The [integration migration map](INTEGRATION.md) names the native GPU work still
required by `.9.1`. No Vulkan renderer or rendered acceptance is claimed here.

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

Supported native MSVC tooling (uses the repository toolchain resolver unchanged):

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

`BUILD_TESTING=OFF` needs no Python/Node. With testing enabled, CMake 3.27+ and
Python 3.9+ are required for the standard-library File API audit (no Python package dependency).
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
The repository does not yet supply the native Vulkan implementation of this seam.

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
| `GFX-INDEPENDENT-GRAPH` | Facade, scene, contracts and shared pure math; forbidden sources/links/compiler dependencies absent |
| `SCENE-UPLOAD/BUFFERS/MATERIAL` | Pending/ready buffer completion, little-endian indices, appearance metadata retention |
| `SCENE-ASSEMBLY/AFFINE/SHARING` | Nested repeated prototypes, unsorted parents, mirrored/nonuniform placement and normal frames |
| `SCENE-CULL/PERSPECTIVE/CAMERA/REGION/LIGHTS` | Six planes, crossing/sheared bounds, standard/reversed depth, explicit region and immutable camera/light state |
| `SCENE-LIMIT/ROLLBACK/BUDGET/PENDING-BUDGET` | Bounded expansion, stale/malformed updates, requested/allocated bytes and delayed allocation growth |
| `SCENE-LIFETIME/SNAPSHOT/BUFFER-OWNER` | Caller/scene/frame/device release order and reclaimed/reused residency |
| `SCENE-FAIL/CANCEL/CAPS` | Partial failure/exception cleanup, cancellation and unsupported capability rejection |
