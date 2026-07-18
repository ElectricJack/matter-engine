# Flecs ECS Foundation — Task 9 Report

## Result

**DONE_WITH_CONCERNS.** The standard `MatterEngine3 test` recipe now delegates
to the independently invocable headless `run-ecs` suite before its existing
test suites. The design remains approved with Phase 1 implemented but mandatory
GNU/MinGW regression and finite-world viewer verification pending; it is not
marked `Implemented — Phase 1 verified`.

`MatterEngine3/tests/Makefile` required no Task 9 edit because its `run-ecs`
phony target and link/run recipe were already present and separately invocable.

## Static RED → GREEN

Before the production edit, a PowerShell assertion parsed the `test:` recipe,
required exactly one `$(MAKE) -C tests run-ecs`, and required it to precede
`run-partv2` and `run-script`. It exited 1 for the expected missing gate:

```text
RED: standard MatterEngine3 test gate expected exactly one
'$(MAKE) -C tests run-ecs' recipe, found 0
```

After adding the one recipe line, the unchanged assertion exited 0:

```text
PASS: standard test gate delegates run-ecs first
```

## Passed / Blocked Matrix

| Gate | Result | Evidence |
|---|---|---|
| Standard `test` recipe static assertion | PASS | Exactly one `run-ecs`, ordered before both existing suites |
| Focused ECS C17/C++17 compile-link-run #1 | PASS | Fresh `msvc_task9_verify1`; one `flecs.obj`; `ALL PASS` |
| Focused ECS C17/C++17 compile-link-run #2 | PASS | Fresh `msvc_task9_verify2`; one `flecs.obj`; `ALL PASS` |
| `world_stream_tests.cpp` MSVC C++17 TU | PASS | Compiled to `world_stream_tests.obj` |
| `matter_engine.cpp` MSVC C++17 TU | PASS | Compiled with normal engine script-host defines/includes; only known C4996/C4244 warnings |
| Task 7 build contract checker | PASS | One C Flecs object and both ECS C++ objects across engine/test/Windows build surfaces |
| Flecs pin and hashes | PASS | v4.1.6; `flecs.h` and `flecs.c` SHA-256 values match `VERSION` |
| ECS scope exclusions | PASS | No named networking, sector ECS, Box3D step, ImGuizmo, or Flecs REST symbols |
| `git diff b56286a --name-only` review | PASS | 45 files total: 25 planned feature/build/dependency/design files and 20 SDD trace artifacts; no unrelated product subsystem |
| Viewer/Explorer caller MSVC TU probes | KNOWN PORTABILITY BLOCKERS | No old `tick()` arity error; see details below |
| Focused GNU `run-ecs run-sectorstream run-worldstream` | BLOCKED BY ENVIRONMENT | WSL has no installed distribution |
| `MatterEngine3 test` | BLOCKED BY ENVIRONMENT | WSL has no installed distribution |
| `build-all.sh` | BLOCKED BY ENVIRONMENT | WSL has no installed distribution |
| MatterViewer Windows `-j2` | BLOCKED BY ENVIRONMENT | WSL has no installed distribution; no MinGW toolchain |
| ExplorerDemo Windows `-j2` | BLOCKED BY ENVIRONMENT | WSL has no installed distribution; no MinGW toolchain |
| Finite-world MatterViewer smoke | BLOCKED BY ENVIRONMENT | No built MatterViewer `.exe`; no supported GPU execution path; nothing launched |

## Supplemental MSVC Evidence

Two independent, previously absent build directories were used. In each run,
Visual Studio 2022 compiled vendored `flecs.c` with `/TC /std:c17`, compiled
`ecs_tests.cpp`, `ecs_runtime.cpp`, and `transform_system.cpp` with
`/std:c++17 /EHsc /W3`, linked the three C++ objects with exactly one
`flecs.obj`, and ran the executable:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

The first facade diagnostic command omitted the normal
`MATTER_HAVE_SCRIPT_HOST` flavor and therefore exposed unavailable guarded
`LocalProvider::host_baker` members. Inspection confirmed the repository's
MatterEngine3 flavor defines `MATTER_HAVE_SCRIPT_HOST`; the corrected command
with the engine's real defines and include roots compiled `matter_engine.cpp`
successfully. This was a diagnostic-command mismatch, not a repository
regression.

The Task 8 caller probes reached the same pre-existing MSVC portability
diagnostics recorded by that task, with no `C2660` parameterless-tick error:

- `MatterViewer/main.cpp`: C2589/C2059 at line 587.
- `ExplorerDemo/main.cpp`: C4576 at lines 498 and 523–530.
- `MatterViewer/main_linux.cpp`: C1083 for Linux-only `unistd.h` at line 32.
- `MatterEngine3/tests/async_bake_tests.cpp`: C1083 for `unistd.h` at line 35.

These diagnostics were not changed because the task explicitly excludes
unrelated pre-existing portability fixes.

## Mandatory Product Commands — Exact Blocker

The following commands were each attempted against this worktree:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-ecs run-sectorstream run-worldstream'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3 test'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && ./build-all.sh'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterViewer windows -j2'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C ExplorerDemo windows -j2'
```

Every attempt returned PowerShell exit `-1` before `bash`, Make, GCC, or MinGW
could run, beginning with:

```text
Windows Subsystem for Linux has no installed distributions.
```

PATH probes also found no native `make`, `gcc`, `g++`,
`x86_64-w64-mingw32-gcc`, or `x86_64-w64-mingw32-g++`.

## Final Contract and Scope Review

- `Libraries/flecs/VERSION` names 4.1.6. SHA-256:
  - header: `526036a5a41678e2a43a3cb835e9eaa70fd1993868b1978950c0d275752f69b1`
  - source: `6005392eb13c0f3c7abdecb2f85271e50934f63919afebf0bbe85f6dfc7320d6`
- `Runtime` value-owns one `flecs::world`; `WorldSession` value-owns one Runtime
  and exposes mutable/const references to that world.
- `WorldSession::tick` has only the explicit `TickDesc` signature. The remaining
  parameterless calls found mechanically belong to the distinct live-edit API.
- The passing focused suite exercises fixed phase order, clamp, catch-up cap,
  drops, fractional remainder, invalid tick non-progress, transform roots and
  three levels, reparent/detach/destruction/cycle rejection, and named-field
  reflection/editing.
- `world_stream_tests.cpp` retains assertions for reload/regenerate survival,
  session-replacement isolation, invalid provider polling, and authored state;
  its TU compiles, but its linked GPU/runtime gate remains blocked.
- The exact exclusion scan returned no matches for
  `GameNetworkingSockets|NetworkId|SectorStreaming|b3World_Step|ImGuizmo` in
  the public ECS header or ECS implementation directory. A separate Flecs REST
  scan also returned no matches.
- No Box3D runtime, sector ECS, editor UI, scripting, persistence, networking,
  or other Phase 2+ behavior was added by Task 9.

## Finite-World Smoke

No `.exe` exists under `MatterViewer`, the Windows product build cannot run,
and this host has no supported GPU execution path. Consequently there is no
launchable executable with which to open a finite world. The smoke was not
performed, no world is claimed, and no GUI/external state was launched or
changed.

## Changed Files

- `MatterEngine3/Makefile`
- `docs/superpowers/specs/2026-07-17-flecs-ecs-foundation-design.md`
- `.superpowers/sdd/flecs-task-9-brief.md`
- `.superpowers/sdd/flecs-task-9-report.md`

## Concerns

The required GNU test/build gates, both MinGW Windows builds, the linked
world-stream regression, and the finite-world viewer smoke remain unverified.
The user's explicit environment exception permits the gate wiring to be
completed and committed, but it does not make these gates pass. The design
therefore remains pending mandatory verification.
