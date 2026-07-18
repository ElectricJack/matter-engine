# Box3D Runtime Physics Phase 2 — Task 8 Report

## Result

Task 8 closes the runtime build graph and normal regression entry points. The
implementation is complete, while mandatory GNU, MinGW, linked WorldSession/GPU,
and product gates remain pending because this Windows host has no GNU/MinGW
toolchain, no WSL distribution, and no supported GPU test path. Those gates are
not reported as passing.

## TDD RED

The WorldSession test first gained a live session physics body plus context and
stats persistence assertions. Its required GNU link/run command was attempted but
could not reach compilation because `make` is unavailable and WSL reports no
installed distribution.

The strengthened static checker was run before build edits and failed with exactly
eight expected issues:

- engine, tests, Viewer, and Explorer still compiled shared Flecs as C99 (four);
- MatterEngine3 standard `test` omitted `run-physics` and failed its ordering check;
- build-all omitted `run-ecs` and `run-physics`.

## Implementation

- `world_stream_tests.cpp` creates a dynamic, zero-gravity sphere in the
  session-owned ECS runtime, executes one fixed tick, captures its context and
  stats, and proves reload/regenerate preserve the same context, full body ID,
  live-body count, creation count, and step count. Session replacement proves the
  old body is absent and the new session owns a valid empty context.
- The checker now discovers all literal assignments containing `ecs_runtime.cpp`
  and requires each to contain `physics_context.cpp`, `physics_shapes.cpp`, and
  `physics_systems.cpp` exactly once.
- Final runtime-bearing test/application recipes are checked for exactly one
  consumed selected Box3D archive. Shared include publication, all public headers,
  standard/build-all ordering, and flattened Windows basename uniqueness are also
  checked.
- Flecs now uses C17 in the engine, shared tests, Viewer Windows, and Explorer
  Windows graphs. Box3D remains C17 and bridge sources remain C++17.
- `MatterEngine3 test` and `build-all.sh test` run `run-ecs` followed by the
  independently invocable `run-physics` exactly once before legacy engine suites.
  Explorer receives build closure only; no feature work or retirement work was
  added.

## Verification

| Gate | Result |
|---|---|
| Strengthened Box3D Phase 2 static checker | PASS |
| `git diff --check` | PASS |
| WorldSession lifecycle test MSVC C++17 translation unit | PASS |
| Fresh focused MSVC build `msvc-box3d-task8-verify1` | PASS: 49 Box3D C17 objects, Flecs C17, physics/ECS C++17, `ALL PASS` twice |
| Fresh focused MSVC build `msvc-box3d-task8-verify2` | PASS: 49 Box3D C17 objects, Flecs C17, physics/ECS C++17, `ALL PASS` twice |
| GNU `run-physics run-ecs run-tilesetphysics run-worldstream` | BLOCKED: WSL has no installed distribution; no native make/GCC/G++ |
| `make -C MatterEngine3 test` | BLOCKED: same environment limitation |
| `./build-all.sh test` | BLOCKED: same environment limitation |
| `make -C MatterViewer windows -j2` | BLOCKED: no WSL distribution or MinGW toolchain |
| `make -C ExplorerDemo windows -j2` | BLOCKED: no WSL distribution or MinGW toolchain |
| Linked WorldSession/GPU run | BLOCKED: no GNU build and no GL 4.6/GALLIUM path |

The first attempted MSVC verification exposed an incomplete diagnostic include
list (`vulkan/vulkan.h` missing), not a repository defect. Both required fresh
builds were then run with the normal engine/test include roots and passed.

## Independent Review Fixes

Independent review found two Important checker-quality gaps, with no product
runtime defect. Both were reproduced before fixing:

1. Swapping build-all to `run-physics run-ecs ...` incorrectly passed. The checker
   now parses the actual MatterEngine3 suite loop, requires each gate exactly once,
   and enforces ECS, then physics, then the first legacy target.
2. Adding a direct Viewer Box3D archive while retaining archive-bearing `LDLIBS`
   incorrectly passed. The checker now evaluates the effective selected-archive
   contract: every repeated/continued test `LDLIBS` block is Box3D-free, focused
   and GPU rules carry one direct archive, and application final recipes consume
   their archive-bearing link variable once with no second direct archive. It also
   proves focused object unions consume their source unions and their final rules
   consume those object unions.

The strengthened checker and diff check pass after both fixes.

## Separate Phase 1 Gap

The existing non-blocking Phase 1 reflection gap is recorded separately: tests do
not directly assert every `LocalTransform` and `WorldRuntimeState` member type and
omit the redundant `Loading != Failed` enum inequality. It is not a Phase 2 build
or runtime defect.
