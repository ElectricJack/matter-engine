# MatterGraphics asset and lifetime facade

`matter_graphics` implements the frozen `matter::services::GraphicsService` v1
in `matter::graphics::Graphics`. It consumes immutable `AssetDocument` owners
and resolved bytes. It links contracts and neutral MathLib only; no generation,
terrain, script host, ECS or legacy engine archive is needed.

This task supplies the scene/resource facade and an injected backend seam, with
fake backend fixtures. It supplies no Vulkan renderer or rendered acceptance.
The subsequent graphics backend task owns actual drawing, shaders/materials,
BLAS/TLAS, culling and residency implementations. Legacy entrypoint conversion
and central source registration remain ordered integration work.

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
dependencies, sources and includes with procedural/terrain/host targets absent.

| Fixture IDs | Assertions |
| --- | --- |
| `GFX-SUBMIT/PENDING/READY/IMPORTED` | Hand-built textured asset and checked-in `fixture.repeated-assembly` JSON, explicit completion with shared mesh/assembly prototypes |
| `GFX-SHARING/CHANGE/AFFINE/SCENE` | Shared instance deletion, placement/replacement, mirrored nonuniform transform, stale revisions, validation/backend rollback |
| `GFX-OWNER/INPUTS/RELEASE/RETIRE` | Immutable assets/bytes outlive device/upload owners; caller/scene/device retirement separated |
| `GFX-ENVELOPE/HANDLE/VALIDATE/CAPS/PAYLOAD` | Every envelope field, foreign/stale/type-confused handles, invalid geometry, exact capabilities and missing/corrupt bytes |
| `GFX-FAIL/CANCEL/TICKET/CONTROL/CALLBACK` | Submission/completion errors and exceptions, malformed completion, cancellation before/during completion, ticket disposal and capture release |
| `GFX-CAMERA/LIGHTS/DEMAND/BUDGET/DISPOSE` | Camera/light validation and rollback, data-only demand, owner/scene ceilings, busy-device and pending-upload disposal |
| `GFX-INDEPENDENT-GRAPH` | Runtime facade and contracts only, forbidden sources/includes/targets absent |
