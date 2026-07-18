# Task 1: Public Physics Contract and Reflection Report

## Scope delivered

- Added the public, engine-native `matter/physics.h` contract with no Box3D
  include or type exposure.
- Added `PhysicsModule` reflection registration and refined the fixed pipeline:
  `PrePhysics -> PhysicsReconcile -> PhysicsPush -> Physics -> PhysicsPull -> PostPhysics`.
- Added a `PhysicsSettings` singleton with the specified defaults.
- Added a focused physics contract/reflection test target that links only Flecs,
  `ecs_runtime.cpp`, `transform_system.cpp`, and the test; it does not link Box3D.
- Did not add physics-world ownership, Box3D sources, commands, stepping, events,
  queries, or later-task runtime behavior.

## TDD evidence

### RED: requested GNU target

Command:

```powershell
make -C MatterEngine3/tests run-physics-contract
```

Output:

```text
make : The term 'make' is not recognized as the name of a cmdlet, function,
script file, or operable program.
```

Result: **blocked**, not passed. GNU make is unavailable because WSL has no
installed distribution.

### RED: MSVC C++17 equivalent

Command:

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\..\Libraries\flecs" /c physics_tests.cpp /FoNUL'
```

Output:

```text
physics_tests.cpp
physics_tests.cpp(2): fatal error C1083: Cannot open include file:
'matter/physics.h': No such file or directory
```

Result: failed for the expected missing public header.

### RED: inspectable public event/query records

After adding assertions for reflected event, stats, and ray-hit fields, the
contract test failed before their reflection registrations were added.

Output:

```text
FAIL: PhysicsBodyEvent reflects its fields
FAIL: PhysicsPairEvent reflects its fields
FAIL: PhysicsHitEvent reflects its fields
FAIL: PhysicsStats reflects its fields
FAIL: PhysicsRayHit reflects its fields
5 FAILURE(S)
```

### GREEN: focused physics contract

Command (MSVC C17 Flecs plus C++17 test/runtime/transform compile, link, and run;
the include list intentionally contains no Box3D path):

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /I"..\..\Libraries\flecs" /c "..\..\Libraries\flecs\flecs.c" /Fo"build\msvc-physics-contract\flecs.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c physics_tests.cpp /Fo"build\msvc-physics-contract\physics_tests.obj" && cl /nologo /std:c++17 /EHsc [same non-Box3D includes] /c "..\src\ecs\ecs_runtime.cpp" /Fo"build\msvc-physics-contract\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc [same non-Box3D includes] /c "..\src\ecs\transform_system.cpp" /Fo"build\msvc-physics-contract\transform_system.obj" && link /nologo "build\msvc-physics-contract\flecs.obj" "build\msvc-physics-contract\physics_tests.obj" "build\msvc-physics-contract\ecs_runtime.obj" "build\msvc-physics-contract\transform_system.obj" /OUT:"build\msvc-physics-contract\physics_contract_tests.exe" && "build\msvc-physics-contract\physics_contract_tests.exe"'
```

Output:

```text
flecs.c
physics_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

### GREEN: Phase 1 ECS regression

Command: same MSVC C17/C++17 focused compile/link sequence, replacing
`physics_tests.cpp` and output objects with `ecs_tests.cpp` and
`build\msvc-ecs\ecs_tests.exe`.

Output:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

## Self-review

- `git diff --check` completed with no output.
- A public-header scan for `box3d`, `box2d`, and `b3*` produced no matches.
- The product diff is restricted to the four requested Task 1 implementation/test
  files. This report is intentionally kept outside that source commit.

## Files

- `MatterEngine3/include/matter/physics.h` (new)
- `MatterEngine3/src/ecs/ecs_runtime.cpp` (modified)
- `MatterEngine3/tests/physics_tests.cpp` (new)
- `MatterEngine3/tests/Makefile` (modified)

## Concerns / follow-up

- The required GNU make gate is blocked on this host and must be rerun where GNU
  make/WSL is available; it is not represented as passing here.
- This task deliberately only declares the public functions. Their runtime
  implementations, Box3D ownership, stepping, command/event/query behavior, and
  build closure belong to later tasks.
