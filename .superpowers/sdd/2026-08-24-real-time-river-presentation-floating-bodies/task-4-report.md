# Task 4 Report: Allocation-Free River Float System

Date: 2026-08-24

Base: `b1064eb3e0bae8644676d62ff41c2e7820a1a671`

Implementation/tests commit: `32dcea40` (`feat: float Box3D bodies on accepted river fields`)

## Outcome

Task 4 is implemented. Authored `RiverFloatBody` Box3D bodies now consume the
currently accepted immutable river runtime field through the existing
publication slot, compute bounded fixed-capacity probe forces, and enqueue only
`physics_apply_force_at_world_point` commands before `PhysicsPush`. Bodies do
not write water state.

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

- sample checksum: `4007601633090329933`
- force checksum: `9848921408926096928`
- transform checksum: `15802835571377753722`

Checksums hash explicit scalar representations, binding generation, sample
outcomes, accepted force points, and forces; they do not hash pointers, padding,
unordered iteration, or time.

## Allocation evidence and limitation

The test overrides global C++ allocation, Flecs' malloc/calloc/realloc counters,
and Box3D's allocator. After body/world warm-up, allocation-free entry/exit hooks
bracket the real `RiverFloatForces` ECS phase. The hooks are function pointers,
perform no allocation, and assert exactly 1,000 ordered begin/end pairs for 24
bodies. This bracket includes the ECS query, pure kernel, and Task 3 force queue.

Final phase-local deltas:

- C++: `0`
- Flecs: `0`
- Box3D: `0`

Whole-`Runtime::tick` counters remain visible as a non-gating diagnostic:

- C++: `86011`
- Flecs: `0`
- Box3D: `1586`

These occur outside `RiverFloatForces`. Existing `PhysicsContext::push` creates
local command/entity scratch containers, `capture_events` creates a fresh
`PhysicsEvents` vector set, and Box3D allocates while stepping. Per the parent
ruling and Task 4 ownership boundary, this report records that pre-existing
limitation; `physics_context.cpp` and Box3D were not modified and the owned
kernel/phase/enqueue zero-allocation assertion was not weakened.

## Publication and failure behavior

`WorldSession::Impl` installs a stable non-owning acquisition callback into the
private ECS binding state. The callback loads the existing
`authored_fluid_publication_slot`; therefore CPU floating and rendering observe
the same successful publication linearization and no second generation channel
exists. Failed/cancelled publication performs no store and preserves the prior
binding/state. The callback is cleared under the same hydrology-generation lock
before destructor publication reset, preventing a dangling session context.

Dry/miss is a valid no-support result. Waterfall has bounded drag but no upward
buoyancy. Non-finite or stale/malformed data fails closed. Eight consecutive
hard-invalid ticks disable once; a changed accepted generation resets private
history before force computation.

## Files

Production and integration:

- `MatterEngine3/include/matter/river_runtime.h`
- `MatterEngine3/src/ecs/river_float_system.h`
- `MatterEngine3/src/ecs/river_float_system.cpp`
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
- The measurement hook is internal and inert unless a test installs it.
- Private invalid/checksum state is restored by SimulationControl but is never
  exposed as authored scene recipe data.
