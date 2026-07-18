# Task 2 Report: Session-Owned Box3D Context

## Status

Implemented Task 2 on top of reviewed Task 1 commit `3b75ec9`. Every
`matter::ecs_runtime::Runtime` now owns one private Box3D world through an
opaque `physics::detail::PhysicsContext`. The focused physics suite, the
existing ECS suite, and the Phase 2 static build-contract checker pass under
the available Windows verification path.

The required GNU Make/g++/gcc command remains **BLOCKED BY ENVIRONMENT**:
PowerShell cannot resolve `make`. The MinGW cross compiler is also unavailable.
Those gates are not reported as passing.

## Implementation

- Added an internal, noncopyable `PhysicsContext` with PImpl storage so no
  Box3D type or header crosses into `MatterEngine3/include` or the internal
  context header.
- Constructed the pinned Box3D world from `b3DefaultWorldDef`, forced one
  worker, copied the reflected default `PhysicsSettings::gravity`, validated
  the returned ID, and throw `std::runtime_error` on invalid creation.
- Destroyed the private Box3D world from the context destructor and cleared
  the ID after the single destroy call.
- Declared `Runtime::world_` before its `std::unique_ptr<PhysicsContext>` and
  moved `Runtime` destruction out of line. Reverse member destruction therefore
  tears down Box3D before Flecs.
- Imported `PhysicsModule`, created the context, and published only the private
  `PhysicsContextRef` singleton pointer inside the Runtime-owned Flecs world.
- Implemented mutable/const internal lookup, the world-validity test seam,
  copied zero-state stats, const empty event buffers, and zero/empty fail-closed
  public accessors for Flecs worlds without a Runtime context.
- Did not add bodies, shapes, stepping, command behavior, event extraction,
  queries, or any other Task 3+ physics behavior.

## TDD Evidence

### Static build-contract RED

The initial `.superpowers/sdd/box3d-phase2-static-check.ps1` was written and
run before build-graph or production changes. It exited 1 with 30 expected
issues, including the missing context files, Runtime ownership/import/ref,
missing source closure, missing shared include closure, old contract-only test
target, and missing platform archive variables.

### Context lifetime RED

The lifetime/fail-closed tests and `physics-tests`/`run-physics` real-link
target were added before `physics_context.h` or implementation code. The
available MSVC C++17 compile exited 1 at:

```text
physics_tests.cpp(3): fatal error C1083: Cannot open include file:
'ecs/physics_context.h': No such file or directory
```

The required RED command was also attempted:

```powershell
make -C MatterEngine3/tests run-physics
```

It was blocked before compilation because `make` is not installed.

### Lookup contract RED/GREEN

The mutable/const context lookup test was added with both definitions absent.
MSVC C++17 compilation succeeded and link failed with exactly two expected
`LNK2019` unresolved `physics::detail::context` overloads. After the minimal
definitions were restored, the focused executable printed `ALL PASS`.

### GREEN verification

Two independent clean directories were used:

- `MatterEngine3/tests/build/msvc-box3d-task2-green1`
- `MatterEngine3/tests/build/msvc-box3d-task2-green2`

For each verification, all 49 pinned `Libraries/box3d/src/*.c` files were
compiled as C17 by MSVC and archived into a fresh `box3d.lib`; vendored
`flecs.c` was compiled separately as C17; and `physics_tests.cpp`,
`ecs_runtime.cpp`, `physics_context.cpp`, and `transform_system.cpp` were
compiled as C++17, linked, and executed. Both runs exited 0 with:

```text
ALL PASS
```

There was no Box3D leak/assert output. The existing `ecs_tests.cpp` suite was
also compiled and linked against the fresh second build's Runtime, context,
Flecs, and Box3D objects; it exited 0 with `ALL PASS`.

After adding the final mutable/const lookup regression, the entire chain was
repeated once more from the previously absent
`MatterEngine3/tests/build/msvc-box3d-task2-final` directory: all 49 Box3D C17
objects, the Box3D archive, Flecs C17, both test translation units, and all
three engine C++17 translation units were rebuilt. The focused physics and ECS
executables both exited 0 and each printed `ALL PASS`.

The required GREEN command was attempted twice after implementation and was
blocked both times by the absent `make` command. The explicit MinGW compiler
probe for `x86_64-w64-mingw32-g++-posix` was likewise not found.

## Build-Graph Closure

- `MatterEngine3/Makefile`: `ME3_CPP` now owns `physics_context.cpp`; shared
  includes contain Box3D; the engine archive has one native Box3D readiness
  prerequisite without incorrectly nesting the dependency archive.
- `MatterEngine3/tests/Makefile`: every literal list containing
  `ecs_runtime.cpp` (`ECS_CPP`, `PHYSICS_CPP`, `GPU_ALL_CPP`) also contains
  `physics_context.cpp`; focused Runtime binaries link one native Box3D
  archive; existing shared GPU Runtime links retain exactly one archive.
- `MatterViewer/Makefile` and `ExplorerDemo/Makefile`: Windows direct-source
  graphs contain both Runtime and context, shared include graphs contain the
  Box3D headers, Linux links select `libbox3d.a`, and Windows links select the
  one `build-mingw/libbox3d.a` archive.

Final static-check result:

```text
PASS: Box3D Phase 2 build contract
 - Runtime owns one opaque context after its Flecs world member
 - every Runtime source graph includes physics_context.cpp exactly once
 - engine, focused tests, GPU tests, Viewer, and Explorer select one platform archive
```

## Files Changed

- `MatterEngine3/src/ecs/physics_context.h`
- `MatterEngine3/src/ecs/physics_context.cpp`
- `MatterEngine3/src/ecs/ecs_runtime.h`
- `MatterEngine3/src/ecs/ecs_runtime.cpp`
- `MatterEngine3/tests/physics_tests.cpp`
- `MatterEngine3/tests/Makefile`
- `MatterEngine3/Makefile`
- `MatterViewer/Makefile`
- `ExplorerDemo/Makefile`
- `.superpowers/sdd/box3d-phase2-static-check.ps1`
- `.superpowers/sdd/task-2-report.md`

## Self-Review

- Box3D remains absent from the public engine include tree; the private header
  also contains no `b3WorldId`.
- The context pointer is stable for the full Runtime lifetime and is never
  exposed through the public API.
- Two live runtimes are covered simultaneously, destruction of the inner one
  leaves the outer world valid, and both mutable/const lookup paths agree.
- Stats cover every field at zero; events cover every buffer as empty; a bare
  Flecs world exercises the null-safe fail-closed path.
- Source closure is enforced for every literal Runtime-bearing list found in
  the four requested Makefiles; archive choice is platform-specific.
- `git diff --check` is clean.
- No Critical or Important issue was found in local review. An independent
  review subagent was requested, but the agent thread limit was already full;
  this report does not claim an independent review.

## Concerns / Blocked Gates

- GNU Make/g++/gcc and MinGW source/link execution remain unverified in this
  Windows environment. The static checker plus fresh MSVC C17/C++17 runs reduce
  risk but do not replace those blocked gates.
- Full Viewer/Explorer builds are outside the focused Task 2 gate and cannot be
  run here without the absent GNU/MinGW toolchains.
