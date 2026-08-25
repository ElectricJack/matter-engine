# Task 4 Report: Allocation-Free River Float System

Date: 2026-08-24

Base: `b1064eb3e0bae8644676d62ff41c2e7820a1a671`

Implementation/tests commit: `32dcea40` (`feat: float Box3D bodies on accepted river fields`)

Review-repair implementation/tests commit:
`2dbbe564527c959341a0261c8c52da93b605de0d`
(`fix: harden river float force delivery`)

## Outcome

Task 4 is implemented. Authored `RiverFloatBody` Box3D bodies now consume the
currently accepted immutable river runtime field through the existing
publication slot, compute bounded fixed-capacity probe forces, and enqueue
force-at-world-point commands before `PhysicsPush`. Production rows use a
generic internal guarded batch lane; the public Task 3 bridge remains unchanged.
Bodies do not write water state.

The implementation includes the exact public component defaults, private
generation/invalid/checksum state, scene reflection and validation, authored
instantiation, Play/Stop restoration, deterministic phase ordering, hard-invalid
disable/reset behavior, replay checksums, and allocation instrumentation around
the real fixed ECS phase.

## TDD evidence

Tests and registrations were authored before production code.

- `tools/build-windows.ps1 -Config RelWithDebInfo -Target river_float_system_tests`
  produced the intended RED under MSVC: fatal C1083, missing
  `ecs/river_float_system.h`.
- `tools/build-windows.ps1 -Config RelWithDebInfo -Target scene_registry_tests`
  produced the intended RED under MSVC: `RiverFloatBody` was undeclared and its
  reflected/instantiated component contract did not exist.
- The first exact combined regression run subsequently exposed the expected
  stale Task 3 assertion that `PhysicsPush` depended directly on
  `PhysicsReconcile`. The focused assertion was updated to the Task 4 chain and
  returned green.

The first sandboxed wrapper launch could not execute the installed native
Python launcher. The same required wrapper was rerun with approved host-tool
access; no alternate compiler or build path was used.

### Independent-review repair RED/GREEN

The independent review reported 0 Critical and 6 Important findings. Each
repair began with a focused failing regression before production changes:

- The first MSVC river-float run produced 40 expected failures covering default
  inset density/draft, 1x/2x/4x drag and angular-torque resolution, checked
  `FLT_MAX` arithmetic, tiny/zero/large quaternion parity, and all legacy source
  closures.
- After that kernel cycle was green, the guarded-publication test cycle failed
  to compile because `RiverFloatState::diagnostic_identity` and the internal
  post-enqueue test hook did not exist.
- The all-or-none guarded-batch physics regression then failed before the
  generic internal batch API existed.
- A final `denorm_min` quaternion regression caught inverse-normalization
  overflow and was red before the exact PhysicsContext semantics were applied.

The only fixture defect after implementation was a scoped Flecs lookup: the
system is registered as `matter::physics::MatterRiverFloatForces`, so an
unqualified lookup returned zero. Correcting the test to the registered full
path made the structural check exercise the intended single callback. All
production-contract assertions were already green.

## Final verification

All commands used `RelWithDebInfo` and the repository Windows wrapper:

- `tools/build-windows.ps1 -Config RelWithDebInfo -Target river_float_system_tests` — PASS
- `tools/build-windows.ps1 -Config RelWithDebInfo -Target scene_registry_tests` — PASS
- `tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests` — PASS
- `tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_viewer_objects` — PASS
- Visual Studio CTest, exact regex
  `^(river_float_system_tests|scene_registry_tests|physics_tests)$` with
  `--output-on-failure` — 3/3 PASS
- Focused verbose Visual Studio CTest for `river_float_system_tests` — PASS
- `git diff --check` / staged `git diff --cached --check` — PASS
- Static direct-Box3D-call census in `river_float_system.{h,cpp}` — 0

The viewer object build accepted and compiled the updated source census:
124 headless core + 21 surface sources and 166/167 viewer sources without/with
the autoremesher.

## Determinism evidence

The fixed phase dependency and observed trace are exactly:

`PhysicsReconcile -> RiverFloatForces -> PhysicsPush -> Physics -> PhysicsPull`

The Play/Stop test snapshots authored component, transform, velocity, and
private float state, runs 300 ticks, restores, reruns 300 ticks, and compares
all three values per tick. Aggregate replay evidence from the final verbose run:

- sample checksum: `10055899834916655463`
- force checksum: `13196851216949461115`
- transform checksum: `13997781128351431705`

Checksums hash explicit scalar representations, binding generation, sample
outcomes, accepted force points, and forces; they do not hash pointers, padding,
unordered iteration, or time.

## Allocation evidence and limitation

The test overrides global C++ allocation, Flecs' malloc/calloc/realloc counters,
and Box3D's allocator. After 16 body/world warm-up ticks, allocation-free
entry/exit hooks bracket the real `RiverFloatForces` ECS callback itself. The
hooks are function pointers, perform no allocation, and assert exactly 1,000
ordered begin/end pairs for 24 bodies. This bracket includes the real ECS query,
the pure kernel, exact accepted `RiverRuntimeBinding` validation ownership, and
the guarded fixed-capacity physics queue.

Final phase-local deltas:

- C++: `0`
- Flecs: `0`
- Box3D: `0`

Whole-`Runtime::tick` counters remain visible as a non-gating diagnostic:

- C++: `86012`
- Flecs: `0`
- Box3D: `992`

The pre-repair run recorded the specifically requested pre-existing diagnostic
of C++ `86011`, Flecs `0`, and Box3D `1586`. The final count changed slightly
after the gate began using the real accepted binding and guarded batch path;
both measurements are non-gating and the phase-local result remains 0/0/0.

The source boundaries prove those whole-tick allocations occur outside
`RiverFloatForces`: `PhysicsContext::push` creates local unordered maps, sets,
and vectors and reserves its entity/trace scratch storage
(`physics_context.cpp:964-1001`); `PhysicsContext::step` calls `b3World_Step`
before `capture_events` (`physics_context.cpp:1101-1113`); and `capture_events`
constructs a fresh `PhysicsEvents` value (`physics_context.cpp:1207-1209`). The
single float callback brackets only `river_float_system.cpp:712-859`. Per the
parent ruling and Task 4 ownership boundary, the repair did not change Box3D or
weaken the owned kernel/phase/queue zero-allocation gate.

## Publication and failure behavior

`WorldSession::Impl` installs a stable non-owning acquisition callback into the
private ECS binding state. The callback loads the existing
`authored_fluid_publication_slot`; therefore CPU floating and rendering observe
the same successful publication linearization and no second generation channel
exists. Each production body enqueues all of its at-most-64 rows atomically with
the exact sampled `shared_ptr<RiverRuntimeBinding>` erased to
`shared_ptr<const void>` plus a `noexcept` validator. Immediately before Box3D
mutation, `PhysicsPush` asks `RiverRuntimeBindingAccess` whether that binding's
publication slot still contains the same identity. A replacement published
after sampling drops every stale row; a failed/cancelled replacement performs
no store and leaves every A row valid. Capacity failure rejects the whole body
batch without admitting a partial row.

This guarded seam is hydrology-agnostic: `PhysicsContext` owns only the erased
validation owner and function pointer. River-specific current-slot/identity
logic stays in the hydrology internal access type. The public Task 3 bridge and
direct-Box3D boundary are unchanged. The acquisition callback is cleared under
the same hydrology-generation lock before destructor publication reset,
preventing a dangling session context.

Dry/miss is a valid no-support result. Waterfall has bounded drag but no upward
buoyancy. Non-finite or stale/malformed data fails closed. Eight consecutive
hard-invalid ticks disable once and diagnose with authored `SceneEntityId`
(runtime ECS id only when authored identity is genuinely absent); a changed
accepted generation resets private history before force computation.

The kernel now separates inset sample/application points from true represented
cell bounds, partitions the entire collider at the default `probe_inset=.15`
even with `probes_y=2`, distributes projected face area across the full lattice,
and keeps drag/torque stable at 1x/2x/4x relevant resolutions. Every potentially
overflowing transform, extent, submersion, force, drag, cap, and quaternion
intermediate uses checked double arithmetic before float storage. Quaternion
normalization exactly follows `PhysicsContext`: finite positive double norm,
float inverse, and rejection of zero/non-finite/inverse-overflow results.

MSVC verifies and pins the public `RiverFloatBody` ABI at size 56 and alignment
4. The legacy Makefile source census uses one shared
`RIVER_FLOAT_SYSTEM_CPP` dependency for every direct `scene_registry.cpp` or
`physics_systems.cpp` target closure; the Makefile was inspected and tested as
text only and was never executed.

## Files

Production and integration:

- `MatterEngine3/include/matter/river_runtime.h`
- `MatterEngine3/src/ecs/physics_context.h`
- `MatterEngine3/src/ecs/physics_context.cpp`
- `MatterEngine3/src/ecs/river_float_system.h`
- `MatterEngine3/src/ecs/river_float_system.cpp`
- `MatterEngine3/src/hydrology/river_runtime_internal.h`
- `MatterEngine3/src/hydrology/river_runtime.cpp`
- `MatterEngine3/src/ecs/ecs_runtime.cpp`
- `MatterEngine3/src/ecs/physics_systems.cpp`
- `MatterEngine3/src/ecs/scene_registry.h`
- `MatterEngine3/src/ecs/scene_registry.cpp`
- `MatterEngine3/src/ecs/simulation_control.h`
- `MatterEngine3/src/ecs/simulation_control.cpp`
- `MatterEngine3/src/matter_engine.cpp`

Tests and registration:

- `MatterEngine3/tests/river_float_system_tests.cpp`
- `MatterEngine3/tests/scene_registry_tests.cpp`
- `MatterEngine3/tests/physics_tests.cpp`
- `MatterEngine3/tests/Makefile`
- `cmake/MatterEngine.cmake`
- `cmake/MatterViewer.cmake`
- `cmake/manifests/engine-core.sources`
- `cmake/tests/viewer_graph_tests.cmake`

`simulation_control.{h,cpp}` and the focused physics phase assertion are
strictly necessary additions to the plan's initial file list. Read-only mapping
proved SimulationControl is the sole Play/Stop component/state restoration path;
the parent explicitly authorized extending it rather than creating a parallel
snapshot mechanism.

The review repair's scoped `physics_tests.cpp` addition tests the generic
physics concern at its owner: after independently discovering the bounded force
lane capacity, it leaves one slot free, rejects a two-row guarded batch exactly
once, ticks Push, and proves neither sentinel row entered the command trace. This
keeps capacity atomicity out of the River-specific fixture while retaining the
earlier Task 4 phase-dependency assertion in the same focused target.

## Toolchain audit

Only the repository's native Windows path was used: Visual Studio 2022
Community developer environment 17.14.7, pinned MSVC 14.44.35207, Windows SDK
10.0.26100.0, Visual Studio CMake/Ninja, native Python 3.13, and the pinned
Vulkan SDK used by the viewer-object graph. Tests ran only through Visual Studio
CTest in `MatterEditor/build/cmake/windows-msvc/relwithdebinfo`.

No Make, GCC, g++, MinGW, MSYS/UCRT, `collect2`, broad CPU suite, PhysX artifact,
legacy executable, or subagent was used.

## Concerns for review

- Whole runtime ticks are not globally allocation-free for the documented
  pre-existing physics/Box3D reasons; Task 4's owned phase is proven zero.
- The measurement and post-enqueue hooks are internal and inert unless a test
  installs them.
- Private invalid/checksum state is restored by SimulationControl but is never
  exposed as authored scene recipe data.
