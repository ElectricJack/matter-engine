# Flecs ECS Foundation — Task 6 Report

## Status

Implemented one value-owned `ecs_runtime::Runtime` per `WorldSession::Impl`, public mutable/const `ecs()` accessors, tick-result statistics, preserved provider/live-edit polling order, and a mutex-protected plain-data bake-state command bridge. Runtime entities survive authored reload/regeneration and the Flecs world dies with the session.

The focused ECS suite passes under the installed Visual Studio 2022 toolchain. The required GNU/GPU `run-worldstream` gate is blocked because WSL has no installed distribution; it is not reported as passing. Task 7 build integration was intentionally not added.

## RED Evidence

Integration assertions were added to `world_stream_tests.cpp` before production changes. The required command was attempted from this worktree:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-worldstream'
```

It exited 1 before Make with `Windows Subsystem for Linux has no installed distributions.`

The integration translation unit was then compiled with MSVC C++17 using the project, Flecs, Vulkan-Headers, MatterSurfaceLib, and raylib include roots. It exited 1 with the expected missing-feature diagnostics, led by:

```text
world_stream_tests.cpp(72): error C2039: 'ecs': is not a member of 'matter::WorldSession'
world_stream_tests.cpp(95): error C2039: 'ecs': is not a member of 'matter::WorldSession'
```

A focused runtime queue regression was also added before implementation. The MSVC ECS chain exited 1 on missing `Runtime::enqueue_world_state` and missing `WorldStateCommandKind::{Loading,Ready,Failed}`.

## Available GREEN Evidence

Fresh final verification used the previously absent directory `MatterEngine3/tests/build/msvc_task6_verify2` and Visual Studio 2022 `vcvars64.bat`:

- compiled vendored `flecs.c` as C17 (`/TC /std:c17 /W0`);
- compiled `ecs_tests.cpp`, `ecs_runtime.cpp`, and `transform_system.cpp` as C++17 (`/std:c++17 /EHsc /W3`);
- linked and ran the focused executable twice;
- compiled `world_stream_tests.cpp` as a C++17 translation unit;
- compiled full `matter_engine.cpp` as a C++17 translation unit with the normal project defines and include roots.

Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
ALL PASS
world_stream_tests.cpp
matter_engine.cpp
```

The facade compile emitted only existing-style MSVC warnings: `getenv` C4996 in `gl46.h`/`matter_engine.cpp` and double-to-float C4244 in pre-existing refine/config code. This is syntax/translation-unit evidence, not the GPU integration run.

One fresh verification initially aborted because the new test used `matter.ecs` with `world::lookup`. Investigation against pinned Flecs showed the API's default separator is `::`; changing the assertion to `matter::ecs` resolved the test-only issue, and the completely fresh `verify2` run above passed twice.

## Lifecycle and Command Placement Audit

- `WorldSession::Impl` contains exactly one `ecs_runtime::Runtime` value member. `WorldSession` shutdown cancels commands, shuts down GPU jobs, joins the worker, and only then destroys `Impl` and its Flecs world.
- `execute_bake` and non-empty `execute_rebake_cone` each enqueue `Loading` on the worker before `BakeStarted`.
- `publish_pipeline` has the only `Ready` site, after a successful blocking finalize and a final cancellation check, before `BakeFinished`. `Ready` increments `content_generation` once.
- BakeAll/Reload, shared publish, and RebakeCone fatal helpers enqueue `Failed` only for non-`Cancelled` errors. Both worker-loop top-level exception pairs also enqueue `Failed` only while the command token is not cancelled.
- A failed live-edit cone rebuild enqueues one `Failed` for the failed attempt, without touching runtime entities.
- Install/child-install partial failures, demand-bake/load per-part failures, cone partial install errors, and deferred tileset errors remain nonfatal. They still publish `Ready` if finalize succeeds.
- Sector-stream errors and the sector `BakeFinished` path contain no ECS state command, so streaming cannot double-increment authored content generation.
- Runtime tick validates first. Invalid ticks neither drain world-state commands nor poll providers. Valid ticks drain commands on the tick thread, run ECS fixed/frame pipelines, update all three ECS stats, then call the mechanically preserved `poll_runtime_sources()` helper.
- Static checks passed: one Runtime member, one `Ready` site, two `Loading` sites, three cancellation-aware fatal helpers, no sector-state enqueue, and exact runtime → invalid guard → stats → provider-poll tick ordering.

## Integration Assertions Added

The existing GPU/world-stream fixture now checks module/singleton resolution, initial `Loading`, exactly one generation increment per authored publish, no increment on sector completion, same named entity ID after `reload()` and `regenerate(7)`, deterministic fatal missing-world transition to `Failed` without entity deletion, and inability of a replacement session to resolve the old raw entity ID. Existing streaming, sea-level, rendering, focus, and regenerate assertions remain in the same binary.

## Files

- `MatterEngine3/include/matter/world_session.h`
- `MatterEngine3/src/ecs/ecs_runtime.h`
- `MatterEngine3/src/ecs/ecs_runtime.cpp`
- `MatterEngine3/src/matter_engine.cpp`
- `MatterEngine3/tests/ecs_tests.cpp`
- `MatterEngine3/tests/world_stream_tests.cpp`
- `.superpowers/sdd/flecs-task-6-report.md`

## Self-Review

- Scope is limited to Task 6 ownership, command bridge, accessors, tick integration, and necessary tests. No Makefile/build integration, broad caller migration, physics, sector ECS conversion, UI, scripting, persistence, or networking was added.
- No worker path accesses Flecs directly; workers enqueue plain-data commands only.
- Provider/live-edit polling logic is preserved in-place as an `Impl` helper, including the bake-active/provider early returns and Linux debounce behavior.
- `git diff --check` and the exact static placement audit passed before commit.

## Concerns / Blocked Gates

- Full `run-worldstream` compile/link/run remains blocked by the missing WSL distribution and required d3d12 GPU environment. In addition, linking the runtime into product/GPU targets belongs to Task 7 and is intentionally absent here.
- The GPU integration assertions therefore have compile-only MSVC evidence, not execution evidence. The focused runtime queue and module/state behavior have executable MSVC coverage.
