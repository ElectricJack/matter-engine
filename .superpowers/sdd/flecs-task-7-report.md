# Flecs ECS Foundation — Task 7 Report

## Status

Implemented the build-only Flecs integration for the MatterEngine3 static
archive, all test flavors that compile `matter_engine.cpp`, the MatterViewer
Windows direct-source build, and the temporary ExplorerDemo Windows
direct-source build. No ECS application behavior was added and ExplorerDemo
was not removed.

The repeatable static build contract and a focused MSVC C/C++ compile-link-run
are green. The required GNU/MinGW Make gates remain **BLOCKED BY ENVIRONMENT**
because WSL has no installed distribution; none of those gates is reported as
passing.

## RED Static Evidence

Before any Makefile edit, `.superpowers/sdd/flecs-task-7-static-check.ps1` was
created and run from the worktree root:

```powershell
& '.\.superpowers\sdd\flecs-task-7-static-check.ps1'
```

It exited 1 with `FAIL: Flecs Task 7 build contract (39 issue(s))`. The failures
were the expected missing Task 7 contract: MatterEngine3 had no Flecs
directory/include/source/object/C rule/archive/clean membership or ECS C++
source/object membership; `VIEWER_LOGIC_OBJS` and `GPU_SHARED_OBJS` had no
`flecsc` object; and MatterViewer/ExplorerDemo had no Flecs include/source/name/
object/C rule/`W_ALL_OBJ` membership or ECS C++ sources. Existing `ecs_tests`
single-Flecs C-flavor assertions were already green.

## GREEN Static and Diff Evidence

After the minimal Makefile edits, the same checker exited 0:

```text
PASS: Flecs Task 7 build contract
 - MatterEngine3 archive has one C-compiled flecs.o plus both ECS C++ objects
 - every matter_engine.cpp test flavor shares exactly one flecsc object
 - Viewer and Explorer Windows unions have unique basenames and one C Flecs source
```

`git diff --check` also exited 0 with no whitespace errors.

The checker asserts all of the following:

- MatterEngine3 declares `FLECS_DIR`, the include, `FLECS_C`, `FLECS_OBJ`, both
  ECS C++ sources and objects, both new `vpath` directories, the dedicated
  `gcc -std=c99` rule, and archive/clean membership.
- `VIEWER_LOGIC_OBJS` and `GPU_SHARED_OBJS` each reference the same single
  `flecsc_C_OBJS`; `ECS_OBJS` still requests exactly one `flecsc` object.
- Both Windows direct-source builds declare the Flecs source/name/object,
  include the two ECS C++ sources, add the one object to `W_ALL_OBJ`, and compile
  it through `WIN_CC -std=c99` outside `WIN_ALL_CPP_SRC`.
- The flattened engine archive object set and both flattened Windows source
  unions have no duplicate basenames.

## Required GNU/MinGW Commands — Blocked

All four required commands were attempted against this worktree:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3 clean && make -C MatterEngine3 -j2'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests clean && make -C MatterEngine3/tests run-ecs'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterViewer clean && make -C MatterViewer windows -j2'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C ExplorerDemo clean && make -C ExplorerDemo windows -j2'
```

Each exited 1 before `bash`, `make`, `gcc`, `g++`, or MinGW could run, with the
same exact leading error:

```text
Windows Subsystem for Linux has no installed distributions.
```

PATH probes additionally found no `make`, `gcc`, `g++`, or
`x86_64-w64-mingw32-gcc`. No toolchain was installed.

## Supplemental MSVC Evidence

A fresh `MatterEngine3/tests/build/msvc_task7_verify` directory was used with
Visual Studio 2022 `vcvars64.bat`. The chain compiled vendored `flecs.c` as C17
with `/TC /std:c17`, compiled `ecs_tests.cpp`, `ecs_runtime.cpp`, and
`transform_system.cpp` as C++17, linked exactly those three C++ objects with one
`flecs.obj`, and ran the executable twice. Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
ALL PASS
```

This is focused source/link evidence only. It does not replace the blocked GNU
Make syntax/archive gate or either blocked MinGW build gate.

## Source/Object Uniqueness Analysis

- MatterEngine3 keeps its existing flat archive object model. `ecs_runtime.o`,
  `transform_system.o`, and `flecs.o` do not collide with any existing QJS,
  engine, ParticleFlow, MSL C++, or MSL C basename. `flecs.c` is absent from
  `ME3_CPP` and is owned only by the explicit `FLECS_OBJ` C rule.
- The test Makefile generates one path-mangled
  `build/flecsc/...flecs.o` from the single `flecsc_C_SRCS` union. ECS,
  viewer-logic, and every GPU shared-object consumer reuse that object; no link
  rule repeats the source or creates another Flecs object.
- MatterViewer and ExplorerDemo each define exactly one
  `build/windows/flecs.o`. The two new ECS C++ basenames are unique in each
  flattened `W_CPP_OBJ` union, `flecs` is unique across the C unions, and
  `W_FLECS_OBJ` is appended once to `W_ALL_OBJ`.
- The explicit Flecs rules use `gcc`/`WIN_CC`; no `g++`/`WIN_CXX` rule consumes
  `flecs.c`.

## Files

- `MatterEngine3/Makefile`
- `MatterEngine3/tests/Makefile`
- `MatterViewer/Makefile`
- `ExplorerDemo/Makefile`
- `.superpowers/sdd/flecs-task-7-static-check.ps1`
- `.superpowers/sdd/flecs-task-7-report.md`

## Self-Review

- Scope is limited to build membership and repeatable static verification.
- Session-bearing tests are fixed at the requested shared-object-list level;
  individual binary link rules were not duplicated.
- Flecs has one C99 compilation per build surface and is never placed in a C++
  source union.
- Existing Windows object-directory cleanup removes `W_FLECS_OBJ`; no redundant
  clean command was introduced.
- ExplorerDemo remains present as the Phase 1 temporary compatibility build.
- No Task 8 callers, physics, sector ECS conversion, UI, scripting,
  persistence, or networking code was touched.

## Concerns

The actual GNU Make parsing/archive composition and MinGW Windows builds could
not be executed in this environment. They must still be run on a host with the
required GNU/MinGW toolchains. The static checker and MSVC run reduce risk but
do not make those blocked gates pass.

## Review-Fix Evidence

Review of commit `641f78a` found two dependency-closure blockers that the first
checker did not model: the GPU source union compiled `matter_engine.cpp` without
the two ECS implementation translation units, and both Windows builds flattened
the new ECS filenames without adding their directory to the C++ `vpath`.

The checker was strengthened before any Makefile fix. It now identifies the
only test source list that literally owns `matter_engine.cpp`, verifies that its
complete GPU union includes `ecs_runtime.cpp` and `transform_system.cpp` exactly
once, verifies that `gpu_CPP_SRCS` and `GPU_SHARED_OBJS` consume that complete
union, and checks the continued C++ `vpath` in each Windows Makefile.

RED command:

```powershell
& '.\.superpowers\sdd\flecs-task-7-static-check.ps1'
```

RED exit 1, exactly four failures:

```text
FAIL: Flecs Task 7 build contract (4 issue(s))
 - GPU_ALL_CPP Runtime implementation expected 1 occurrence(s) of '../src/ecs/ecs_runtime.cpp', found 0
 - GPU_ALL_CPP transform implementation expected 1 occurrence(s) of '../src/ecs/transform_system.cpp', found 0
 - MatterViewer C++ vpath missing '$(ME3_DIR)/src/ecs'
 - ExplorerDemo C++ vpath missing '$(ME3_DIR)/src/ecs'
```

The minimal production fix added both ECS C++ sources once to `GPU_ALL_CPP` and
added `$(ME3_DIR)/src/ecs` to the Viewer and Explorer C++ `vpath` lines. No link
rule, application caller, or runtime behavior changed.

GREEN rerun exited 0:

```text
PASS: Flecs Task 7 build contract
 - MatterEngine3 archive has one C-compiled flecs.o plus both ECS C++ objects
 - every matter_engine.cpp test flavor links both ECS C++ objects and one flecsc object
 - Viewer and Explorer Windows unions have unique basenames and one C Flecs source
```

`git diff --check` exited 0 with no output.

Fresh supplemental MSVC verification used the previously absent directory
`MatterEngine3/tests/build/msvc_task7_review_fix_verify`. Visual Studio 2022
compiled `flecs.c` with `/TC /std:c17`, compiled `ecs_tests.cpp`,
`ecs_runtime.cpp`, and `transform_system.cpp` with `/std:c++17 /EHsc /W3`,
linked one Flecs object, and ran the executable twice. Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
ALL PASS
```

The GNU/MinGW commands remain blocked by the unchanged no-WSL-distribution
environment and are still not claimed passing. The supplemental MSVC suite does
not replace those gates.
