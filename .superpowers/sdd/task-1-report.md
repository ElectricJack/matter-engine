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

### GREEN: full Task 1 diff hygiene

Command:

```powershell
git diff d923e4a --check
```

Output: no output (exit code 0).

## Self-review

- The original pre-commit `git diff --check` only covered already-tracked edits;
  it did not validate the then-untracked brief/report. A post-commit
  `git diff HEAD^ HEAD --check` correctly exposed the brief's trailing blank
  line, which the review follow-up removes. The full base-to-worktree diff is
  checked again with the result recorded above.
- A public-header scan for `box3d`, `box2d`, and `b3*` produced no matches.
- Commit `d90e5a4` includes the four requested product/test files together with
  the committed task brief and this committed report.

## Files

- `MatterEngine3/include/matter/physics.h` (new)
- `MatterEngine3/src/ecs/ecs_runtime.cpp` (modified)
- `MatterEngine3/tests/physics_tests.cpp` (new)
- `MatterEngine3/tests/Makefile` (modified)
- `.superpowers/sdd/task-1-brief.md` (committed task brief)
- `.superpowers/sdd/task-1-report.md` (committed task report)

## Concerns / follow-up

- The required GNU make gate is blocked on this host and must be rerun where GNU
  make/WSL is available; it is not represented as passing here.
- This task deliberately only declares the public functions. Their runtime
  implementations, Box3D ownership, stepping, command/event/query behavior, and
  build closure belong to later tasks.

## Review-fix verification

### RED: focused GNU recipe did not request C17

Command:

```powershell
$makefile = Get-Content -Raw 'MatterEngine3\tests\Makefile'; if ($makefile -notmatch 'FLAVOR_physicsc_FLAGS\s*:=\s*-std=c17') { Write-Error 'RED: physics contract has no dedicated Flecs C17 flavor'; exit 1 }
```

Output:

```text
RED: physics contract has no dedicated Flecs C17 flavor
```

### GREEN: focused GNU recipe is isolated under C17

Command:

```powershell
$makefile = Get-Content -Raw 'MatterEngine3\tests\Makefile'; $has_flags = $makefile -match 'FLAVOR_physicsc_FLAGS\s*:=\s*-std=c17'; $has_objects = $makefile -match 'PHYSICS_CONTRACT_OBJS\s*=.*obj_list,physicsc'; $has_sources = $makefile -match 'physicsc_C_SRCS\s*:=.*PHYSICS_CONTRACT_C'; if (-not ($has_flags -and $has_objects -and $has_sources)) { Write-Error 'physics contract C17 flavor wiring is incomplete'; exit 1 }; Write-Output 'PASS: physics contract Flecs source is isolated under -std=c17'
```

Output:

```text
PASS: physics contract Flecs source is isolated under -std=c17
```

### GREEN: strengthened physics contract under MSVC C17/C++17

Command (run from `MatterEngine3/tests`; no Box3D include or link input):

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /I"..\..\Libraries\flecs" /c "..\..\Libraries\flecs\flecs.c" /Fo"build\msvc-physics-contract\flecs.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c physics_tests.cpp /Fo"build\msvc-physics-contract\physics_tests.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c "..\src\ecs\ecs_runtime.cpp" /Fo"build\msvc-physics-contract\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c "..\src\ecs\transform_system.cpp" /Fo"build\msvc-physics-contract\transform_system.obj" && link /nologo "build\msvc-physics-contract\flecs.obj" "build\msvc-physics-contract\physics_tests.obj" "build\msvc-physics-contract\ecs_runtime.obj" "build\msvc-physics-contract\transform_system.obj" /OUT:"build\msvc-physics-contract\physics_contract_tests.exe" && "build\msvc-physics-contract\physics_contract_tests.exe"'
```

Output:

```text
flecs.c
physics_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

The strengthened suite uses `world.lookup()` for registration liveness, so the
assertions cannot register a missing component. It now verifies velocity fields,
every shared collider-property field, each collider's nested properties and
geometry metadata, all specified defaults, and both the refined and original
Phase 1 dependency edges.

### GREEN: final Phase 1 ECS regression under MSVC C17/C++17

Command (run from `MatterEngine3/tests`):

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c17 /I"..\..\Libraries\flecs" /c "..\..\Libraries\flecs\flecs.c" /Fo"build\msvc-ecs\flecs.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c ecs_tests.cpp /Fo"build\msvc-ecs\ecs_tests.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c "..\src\ecs\ecs_runtime.cpp" /Fo"build\msvc-ecs\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /I"..\include" /I"..\src" /I"..\src\render" /I"..\src\provider" /I"..\..\MatterSurfaceLib\include" /I"..\..\Libraries\raylib\src" /I"..\..\ParticleFlowLib\include" /I"..\..\Libraries\Vulkan-Headers\include" /I"..\..\Libraries\flecs" /c "..\src\ecs\transform_system.cpp" /Fo"build\msvc-ecs\transform_system.obj" && link /nologo "build\msvc-ecs\flecs.obj" "build\msvc-ecs\ecs_tests.obj" "build\msvc-ecs\ecs_runtime.obj" "build\msvc-ecs\transform_system.obj" /OUT:"build\msvc-ecs\ecs_tests.exe" && "build\msvc-ecs\ecs_tests.exe"'
```

Output:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```
