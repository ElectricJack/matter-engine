# Optional solid compute generation

`matter_geometry_compute` evaluates a bounded ordered solid request and returns
an immutable `matter::services::AssetProduct`. It does not link a graphics scene,
window, world, authoring host, terrain scheduler, or frame loop. The default build
is disabled: `capabilities()` is empty and generation explicitly returns
`unsupported_capability`, including when a device is supplied. A host can select
`matter::geometry::PrimitiveGeometry` independently as its CPU fallback. No GPU
handle enters an asset, and no Vulkan dependency is required for CPU/WASM builds.

## Ownership and integration

This task owns `libs/GeometryCompute/` and `libs/ComputeDevice/`. Only these local
build files change. Ordered integration task `clear-crest-48.9.4` owns registration,
ScriptHost/DslState conversions, and replacement of live legacy callers. The
shared contract gate at `764cf480b79a90b69d389773bbb8a3309ca13fe8` precedes this work.

The algorithm authority remains the existing `solid_{field,classify,scan,emit}`
shaders and their GLSL includes under `MatterEngine3/shaders_vk/`, the canonical
marching-cubes table, and `gpu_solid_mesher_common.cpp`. This target compiles these
sources directly, embedding only those four shaders. There is no copied field,
meshing or validation algorithm. New code replaces renderer-specific dispatch,
lowers authoring values into the private packed tape, and converts readback into
the portable asset boundary. Exact zero-area table triangles at grid-aligned
isosurfaces are discarded using the contract's double-precision cross product;
nonzero triangles are retained without a tolerance change.

`SolidRequest` v1 is owned authoring data: five primitive shapes, physical
metre dimensions, ordered union/subtraction/intersection, polynomial smoothing,
proper rotation/translation, one opaque PBR material, spacing and source identity.
It is **not** the shader's 96-byte `SolidOp`. Lowering is private and validated.
It deliberately does not reinterpret the CPU `solid.ordered` grouped-CSG recipe
semantics as this backend's left fold. Generic recipe adaptation belongs above
this optional interface; neither JS objects nor packed tape cross its public API.

| Capability | Behavior |
| --- | --- |
| `matter.geometry.solid.compute`, version 1 | Available only in enabled builds with an injected device |
| Box, rounded box, sphere, ellipsoid, Y capsule | Canonical legacy field/meshing shader implementation |
| Ordered union, subtraction, intersection, smoothing | Preserves tape order and existing polynomial field semantics |
| Proper rotation and translation | Accepted; scale/shear/reflection fail explicitly |
| Asset/PBR v1 | CPU positions, normals, indices, material and instance; shared immutable ownership |
| Backend disabled / device absent | `unsupported_capability`, no dispatch |
| Unknown required capability/version | Explicit failure, no dispatch |
| Device-local handoff | Not offered; output is always CPU-owned |

Particle-fluid visual meshing, surface material projection and renderer-specific
GPU products are not advertised by this solid capability. Their current legacy
implementations remain authoritative. This module supplies the neutral dispatch
service for later adapters without inventing a new particle/terrain contract.

## Device and synchronization

The lower `matter_compute_device` target depends only on contracts. The optional
`matter_compute_vulkan` target adds Vulkan. `create_vulkan(true, diagnostics)`
creates a compute queue without a surface and requires validation including
synchronization validation. `bind_vulkan` instead retains a host's device owner
and requires a mutex shared with **every** user of that queue. Native handle
provenance is a trusted host responsibility; opaque service IDs reject accidental
cross-device requests at both public boundaries.

The synchronous service owns buffers, descriptors, pipelines, command storage and
fence until completion. HOST-to-compute, compute-to-compute and compute-to-HOST
barriers order uploads, all four dispatches and readback. CPU bytes are copied
only after a successful fence wait. Releasing the backend/device cannot invalidate
an existing result. No concurrent frame assumptions or frame-retention calls
exist. A busy shared queue returns `resource_limit` without blocking admission.

Cancellation/deadlines are cooperative checkpoints before lowering/allocation,
before submission, after completion, during decoding and before publication. An
already submitted Vulkan dispatch cannot be portably preempted; it is drained
before failure returns. If neither fence nor idle status proves retirement and
the device is not lost, the complete run and its device owner are deliberately
quarantined rather than freeing possibly pending resources. This is not a hard
GPU timeout or an arbitrary untrusted-SPIR-V execution service.

Finite limits cover 256 elements, 4,194,304 lattice samples, 2,097,152 output
vertices, caller operation/output/working budgets and hardware descriptor,
allocation and dispatch limits. Actual aligned buffer allocations are checked
against the admitted byte ceiling. Working estimates include readback, decoded
mesh and serialization headroom; Vulkan driver's private compilation/allocator
storage is outside the application-visible byte budget. Exceeding output capacity
returns no partial product. Device-local output and implicit fallback are absent.

## Build and fixtures

From native PowerShell, use the repository's pinned MSVC toolchain resolver:

```powershell
libs/GeometryCompute/build-windows.ps1 -Config RelWithDebInfo -Test
libs/GeometryCompute/build-windows.ps1 -Config RelWithDebInfo -Disabled -Test
```

The enabled run requires a Vulkan 1.2 compute device and the Khronos validation
layer; unavailable required tools/hardware fail instead of skipping the fixture.
From WSL invoke the same script through `powershell.exe -File` with `wslpath -w`.
The disabled portable build also runs independently on Linux:

```sh
cmake -S libs/GeometryCompute -B /tmp/matter-geometry-compute-off -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMATTER_GEOMETRY_COMPUTE=OFF
cmake --build /tmp/matter-geometry-compute-off -j 2
ctest --test-dir /tmp/matter-geometry-compute-off --output-on-failure
```

For an enabled standalone Linux configuration add
`-DMATTER_GEOMETRY_COMPUTE=ON` with Vulkan headers/loader and `glslc` installed.

`matter_geometry_compute` exercises `GPU-HEADLESS-SPHERE`, `GPU-LEGACY-SHAPES`,
`GPU-ORDERED-SUBTRACTION`, `GPU-EMPTY-SUCCESS`, repeat topology/position identity,
`GPU-CAPACITY-OVERFLOW`, `GPU-SYNCHRONIZATION-VALIDATION`, `GPU-CPU-OWNERSHIP`,
`GPU-DEVICE-MISMATCH`, `GPU-CAPS`, resource ceilings, malformed/stale results,
cancellation before dispatch and publication, plus `GPU-DISABLED`/`GPU-UNAVAILABLE`.
Sphere tolerance is pinned at 0.02 metres radial error at 0.25m spacing, with
normal alignment >=0.99; contract topology validation rejects degenerate faces.
`matter_geometry_compute_dependencies` checks target links, source includes and
the standalone executable graph for forbidden upward dependencies. There is no
editor launch or graphics scene in these tests.

Validation messages distinguish actual validation failures from loader warnings
about unrelated installed implicit layers. The fixture requires zero validation
errors; loader diagnostics remain visible in verbose CTest output.
