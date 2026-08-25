# Task 3 report — queued Box3D force at world point

## Scope and commits

- Exact base: `4ef25ce94bf1b0d26f05d8e41c571fe6dad13fe5`
- Implementation commit: `bfca77afef526ec345932beb62f27b9e19a1d902`
- Implementation commit message: `feat: queue Box3D forces at world points`
- This report is committed separately from the implementation.

## TDD RED

Tests and CMake registration were edited before production code.

Command:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests
```

The first sandboxed invocation could not see the installed native Python and
was not accepted as the RED. The approved rerun entered the Visual Studio 2022
Developer Command Prompt, configured the required Windows-MSVC build tree, and
failed in `MatterEngine3/tests/physics_tests.cpp` for the intended missing
feature:

- MSVC `C2039` / `C3861`: `physics_apply_force_at_world_point` was not a
  member of `matter::physics` / was not found.
- MSVC `C2039`: `enqueue_force_at_world_point` was not a member of
  `matter::physics::detail::PhysicsContext`.
- MSVC `C2838` / `C2065`: `PhysicsCommandKind::ForceAtPoint` was absent.
- Ninja stopped after the focused target compile failure.

This was a meaningful RED because the registered real test target compiled
against the existing engine and failed specifically on all three missing
contract surfaces.

## Implementation

- Added the provider-free public API
  `physics_apply_force_at_world_point(flecs::entity, Float3, Float3)`.
- Added `PhysicsContext::enqueue_force_at_world_point(...) noexcept`.
- Extended command-kind order to
  `Teleport -> Velocity -> Force -> ForceAtPoint -> Impulse -> Wake`.
- Reused the ordinary-force lane for `ForceAtPoint`; no feature-specific queue
  was introduced.
- Bounded the shared force lane at 4096 rows and pre-reserved its active and
  drain buffers so enqueue performs no heap allocation, including after a
  fixed-step drain.
- Rejected overflow increments the existing `failed_commands` diagnostic once.
- Kept entity/world/body/liveness/dynamic/error and finite-vector admission
  checks on the existing path.
- Centralized all six command mutations in the one deferred
  `PhysicsContext::push` switch.
- The `ForceAtPoint` case resolves the entity-owned body and calls exactly:

```cpp
b3Body_ApplyForce(
    bridge->body, box_vector(command.primary),
    box_position(command.secondary), true);
```

- Trace encoding is `kind == ForceAtPoint`, `primary == force`, and
  `secondary == world_point`.

## Tests added

`MatterEngine3/tests/physics_tests.cpp` now proves:

1. An off-centre force-at-point is queued and leaves Box3D linear/angular
   velocity unchanged before `PhysicsPush`.
2. The complete deterministic trace order is exactly Teleport, Velocity,
   Force, ForceAtPoint, Impulse, Wake.
3. Trace primary/secondary vectors exactly retain the finite force and world
   point inputs.
4. After the fixed runtime path, an upward force at positive X produces
   positive-Z angular velocity within tolerance, while the centred equivalent
   produces no torque.
5. Foreign-world, stale/destroyed, bodyless, static, NaN/Inf force, and NaN/Inf
   point calls return false without queue, trace, diagnostic, or retained-body
   mutation.
6. Ordinary Force and ForceAtPoint share a bounded lane; the first rejected
   overflow increments `failed_commands` once, and every earlier admitted row
   drains in deterministic order.
7. Existing command tests remain active and retain their prior meaning; the
   shared rejection helper now also covers the new command.

During GREEN verification, the combined invalid-input assertion initially
reported that the retained body position changed. Systematic isolation showed
that queue, trace, velocities, and diagnostics were correct: the fixture had
placed dynamic and static spheres at the same origin, so a legitimate physics
step resolved overlap. The fixture was spatially separated, assertions were
kept individually diagnostic, and the focused gate then passed.

## Final GREEN evidence

Build command:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests
```

Result: exit 0. CMake configured/generated
`MatterEditor/build/cmake/windows-msvc/relwithdebinfo`; Ninja rebuilt and linked
`physics_tests.exe` successfully.

Focused test command:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^physics_tests$' --output-on-failure
```

Result: exit 0, `1/1` passed, `0` failed, `0.90 sec` real time. No broad CPU
suite was run, per the Task 3 brief.

## Static audit

- `git diff --check`: exit 0, no output.
- Public bridge audit: `public_bridge_b3_calls=0`.
- Deferred dispatch audit: `deferred_command_switches=1`.
- Box3D force-at-point audit: `b3Body_ApplyForce_calls=1`, located in the
  `PhysicsCommandKind::ForceAtPoint` case of `PhysicsContext::push`.
- Build/test toolchain: only `tools/build-windows.ps1`, MSVC v143, Ninja, and
  Visual Studio CTest were used. No Make, GCC, g++, MinGW, MSYS/UCRT binary,
  `collect2`, legacy test executable, or broad CPU suite was run.

## Files changed by the implementation commit

- `MatterEngine3/include/matter/physics.h`
- `MatterEngine3/src/ecs/physics_context.h`
- `MatterEngine3/src/ecs/physics_context.cpp`
- `MatterEngine3/tests/physics_tests.cpp`
- `cmake/MatterEngine.cmake`

The legacy `MatterEngine3/tests/Makefile` did not need modification because the
existing source was already present there; only CMake registration was missing.
All unrelated tracked/untracked worktree content was preserved.

## Remaining concern

No Task 3 functional concern remains after the focused gate and audits. The
MSVC build continues to print existing warnings from Flecs/template call sites
(`C4099`, `C4127`, and `C4244`); none originates in the Task 3 changes and the
focused target links and passes.
