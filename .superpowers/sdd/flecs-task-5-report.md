# Flecs ECS Foundation — Task 5 Report

## Status

Implemented fixed-step and frame ECS pipelines, accumulator validation/clamping/drop behavior, public tick/stat declarations, and the approved last-write-wins hierarchy command queue. The scoped ECS suite passes twice under the installed Visual Studio 2022 supplemental toolchain.

The required GNU gate remains **BLOCKED BY ENVIRONMENT** because WSL has no installed distribution. It is not reported as passing.

## RED Evidence

The scheduling and hierarchy queue tests were added before production implementation. This exact compiler chain was then run from the worktree root:

```powershell
New-Item -ItemType Directory -Force 'D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red' | Out-Null
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /W0 /TC /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs\flecs.c" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\flecs.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\ecs_tests.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\ecs_tests.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\ecs_runtime.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\transform_system.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\transform_system.obj" && cl /nologo /EHsc "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\ecs_tests.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\ecs_runtime.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\transform_system.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\flecs.obj" /Fe"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_red\ecs_tests.exe"'
```

Exit 1. Flecs compiled as C17; the new C++17 tests then failed on the missing Task 5 contract. The first exact diagnostics were:

```text
flecs.c
ecs_tests.cpp
MatterEngine3\tests\ecs_tests.cpp(87): error C2039: 'ecs_runtime': is not a member of 'matter'
MatterEngine3\tests\ecs_tests.cpp(87): error C3083: 'ecs_runtime': the symbol to the left of a '::' must be a type
MatterEngine3\tests\ecs_tests.cpp(87): error C2039: 'Runtime': is not a member of 'matter'
MatterEngine3\tests\ecs_tests.cpp(87): error C2065: 'Runtime': undeclared identifier
...
MatterEngine3\tests\ecs_tests.cpp(883): fatal error C1003: error count exceeds 100; stopping compilation
```

The omitted lines were cascading uses of the same missing `Runtime`, `TickResult`, and `TickDesc` types; compilation never reached linking or execution.

## GREEN Evidence

The first full GREEN compiler/link/run used build directory `msvc_task5_green1`. Because `TickDesc` is intentionally declared in the existing public `world_session.h`, the supplemental MSVC command included the same source/dependency include roots supplied by the GNU ECS target.

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

Exit 0.

Fresh final verification used a previously absent `msvc_task5_verify` directory and this exact chain, with two separate executable runs:

```powershell
New-Item -ItemType Directory 'D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify' | Out-Null
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /W0 /TC /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs\flecs.c" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\flecs.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\ecs_tests.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_tests.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\ecs_runtime.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\transform_system.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\transform_system.obj" && cl /nologo /EHsc "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_tests.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_runtime.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\transform_system.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\flecs.obj" /Fe"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_verify\ecs_tests.exe"'
```

Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
ALL PASS
```

The identical two-run result checks that separate Runtime instances do not leak static/global Flecs or queue state.

## GNU Gate

The required command was run exactly:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-ecs'
```

Exit 1:

```text
Windows Subsystem for Linux has no installed distributions.
```

Therefore the GNU Make/gcc/g++ gate is blocked and was never claimed as passing.

## Scheduling and Accumulator Coverage

The pinned Flecs v4.1.6 API supported the approved custom pipeline design without conflict. Each custom pipeline query mirrors the built-in ordering pattern: `EcsSystem`, a cascading `EcsPhase` source over `DependsOn`, the fixed/frame membership tag, disabled-system exclusions, and entity-ID ordering.

The recorded one-step order is exactly:

```text
FixedPreUpdate, FixedUpdate, PrePhysics, Physics,
PostPhysics, FixedPostUpdate, FrameUpdate
```

Covered cases:

- `0.05 / 0.1`: zero fixed steps and one `FrameUpdate`; a second `0.05` completes one fixed step.
- `0.2 / 0.1`: two complete six-phase fixed steps, followed by one frame phase.
- frame `1.0`, fixed `0.1`, max `2`: contribution clamps to `0.25`; two fixed steps, zero dropped steps, `0.05` fractional remainder; the later `0.05` completes one step.
- frame `0.25`, fixed `0.01`, max `2`: two fixed steps, 23 dropped complete steps, no retained whole step; a later `0.005 + 0.005` pair completes exactly one step.
- negative/NaN frame delta, zero/NaN fixed delta, and zero max steps return `{0,0,true}` and run neither pipeline.
- Fixed systems receive exactly the configured fixed delta; the frame system receives the contributed/clamped frame delta.

The accumulator is `double`. Validation uses `std::isfinite` before time or queue progression. Only contributed frame time clamps, to `0.25` seconds. Complete excess steps are removed after the fixed catch-up limit while fractional time is preserved.

## Hierarchy Queue Coverage

- Queue state is a Flecs world singleton containing one last-write-wins command per child.
- Drain happens only after validation, at the beginning of a valid Runtime tick and before both pipelines.
- Three pre-tick requests for one child collapse to the final requested parent and cause only one ChildOf add.
- Invalid ticks do not drain; the command applies on the next valid tick.
- Commands enqueued from a ChildOf observer during a pending reparent land in the next swapped batch. They do not apply in the same observer/merge sequence.
- An observer's clear-then-reparent pair collapses to the final parent.
- If a command is still marked pending at drain, it is retained unless a newer same-child command already replaced it.
- Dead child, dead parent, and cross-world parent commands are discarded. A later valid request for that child still applies.

## Files

- `MatterEngine3/include/matter/world_session.h`: `TickDesc`, ECS counters, explicit tick declaration.
- `MatterEngine3/include/matter/ecs.h`: public hierarchy enqueue helpers.
- `MatterEngine3/src/ecs/ecs_runtime.h`: private non-copyable Runtime and TickResult contracts.
- `MatterEngine3/src/ecs/ecs_runtime.cpp`: fixed/frame pipelines, validation, accumulator, clamp/drop policy.
- `MatterEngine3/src/ecs/transform_system.cpp`: world-owned hierarchy command map and valid-tick drain.
- `MatterEngine3/tests/ecs_tests.cpp`: scheduling, timing, validation, and queue regressions.
- `.superpowers/sdd/flecs-task-5-report.md`: this report.

## Self-Review

- `git diff --check`: exit 0.
- Scope review: no WorldSession Runtime ownership, bake-state command bridge, caller migration, physics, sector streaming, UI, scripting, persistence, networking, or new build integration was added.
- Flecs API review: the C++ pipeline builder/query methods and `run_pipeline` are present in pinned v4.1.6 and passed the behavioral phase-order tests.
- Queue review: swapping the world-owned map before applying commands is the key boundary that prevents observer-generated same-child requests from entering the current drain batch.
- Lifetime review: queued IDs retain Flecs generations; parent world identity is compared before constructing/dereferencing a same-world parent handle.
- Tracked-file review: changes are confined to the six Task 5 source/test headers plus this report. The pre-existing untracked brief is excluded from the commit.

## Concerns / Handoff

- The public header now intentionally declares only `WorldSession::tick(const TickDesc&)`, while the Task 6 implementation bridge and Task 8 caller migration remain out of scope. Full-product callers are expected to be staged-broken until those planned tasks land; only the existing `ecs_tests` target is validated here.
- `FrameStats` counters are declarations only in Task 5; Task 6 owns wiring Runtime results into session statistics.
- The GNU gate remains blocked by the host's missing WSL distribution.

---

## Follow-up Fix: Exact Accumulator Boundaries and Pending Queue Retention

### Follow-up RED

The boundary, pending-drain, additional invalid-input, and stale-generation tests were added before the accumulator fix. A fresh build used this exact chain:

```powershell
New-Item -ItemType Directory 'D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red' | Out-Null
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /W0 /TC /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs\flecs.c" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\flecs.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\ecs_tests.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\ecs_tests.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\ecs_runtime.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\transform_system.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\transform_system.obj" && cl /nologo /EHsc "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\ecs_tests.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\ecs_runtime.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\transform_system.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\flecs.obj" /Fe"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_red\ecs_tests.exe"'
```

Exit 1:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
FAIL: frame immediately below fixed delta does not invent a fixed step
FAIL: short accumulator is preserved until exact missing time arrives
FAIL: fraction immediately below a complete excess step is not dropped
3 FAILURE(S)
```

The real pending-drain retention, extra invalid-input, and stale-generation regressions passed during this RED. Only the broad accumulator tolerance failed.

### Root Cause and Minimal Fix

Measured double values derived from the float API were:

```text
fixed                         0.10000000149011612
nextafter(fixed, 0)           0.099999994039535522
one downward float ULP gap    7.4505805969238281E-09
half downward float ULP       3.7252902984619141E-09
clamp remainder + 0.05f       0.099999997764825821
approved clamp shortfall      3.7252902984619141E-09
nextafter(0.2f,0)-fixed       0.099999986588954926
post-step shortfall           1.4901161193847656E-08
```

The old relative epsilon was much broader than the float API's representational boundary and was added to both run eligibility and drop `floor`, inventing and dropping incomplete steps.

The fix:

- removes the relative epsilon entirely;
- keeps fixed-step eligibility as a strict `accumulator >= fixed_delta` comparison;
- keeps dropping strict as `floor(accumulator / fixed_delta)` with no snap/tolerance;
- removes negative-remainder clamping;
- only before the strict run comparison, snaps a positive shortfall no larger than half the measured downward float ULP. This admits the approved decimal clamp sequence, whose shortfall is exactly half an ULP, but rejects the one-ULP and two-ULP `nextafter` regressions.

The first strict implementation intentionally had no snap and exposed the approved decimal compatibility boundary. Build directory `msvc_task5_fix_green` exited 1 with:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
FAIL: clamped tick preserves its fractional 0.05-second remainder
1 FAILURE(S)
```

After the bounded run-only snap, build directory `msvc_task5_fix_green2` exited 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

### Added Queue and Validation Coverage

- A test opens an outer Flecs defer, starts an immediate reparent so the child remains in the pending map, enqueues a final parent, and calls only the private drain seam. The command remains queued; ending the defer commits the first parent; the next valid Runtime tick applies the retained final parent.
- The private drain seam is declared only in internal `src/ecs/ecs_runtime.h`; no pipeline is run inside the outer defer.
- Positive-infinity frame delta, positive-infinity fixed delta, and negative fixed delta are invalid and progress no systems.
- A destroyed queued child ID is immediately recycled with a new Flecs generation; draining the stale command does not mutate the replacement entity.

### Fresh Follow-up Verification

The final verification directory did not exist before the run. The exact command was:

```powershell
New-Item -ItemType Directory 'D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify' | Out-Null
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /W0 /TC /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs\flecs.c" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\flecs.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\ecs_tests.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_tests.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\ecs_runtime.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\render" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterSurfaceLib\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\Vulkan-Headers\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\transform_system.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\transform_system.obj" && cl /nologo /EHsc "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_tests.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_runtime.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\transform_system.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\flecs.obj" /Fe"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task5_fix_verify\ecs_tests.exe"'
```

Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
ALL PASS
```

The GNU gate was rerun exactly and remains blocked, not passing:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-ecs'
```

```text
Windows Subsystem for Linux has no installed distributions.
```

Follow-up `git diff --check` passed. Changes remain limited to `ecs_runtime.cpp`, its private header seam, `ecs_tests.cpp`, and this report.
