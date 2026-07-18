# Phase 3 Task 7 Report: ExplorerDemo Retirement and Build Closure

## Result

Retired the obsolete `ExplorerDemo` application. All fourteen tracked files
under that directory, including flight/package/time tools, are deleted. The
active build matrix, warm-cache smoke block, ignore entries, and Phase 2 Box3D
closure no longer require it. Historical docs/specs/plans were preserved.

## TDD Static-Checker Evidence

The new `sector-streaming-phase3-static-check.ps1` was written and run before
deletion. Its final RED result was:

```text
FAIL: Sector streaming Phase 3 closure (3 issue(s))
 - tracked ExplorerDemo files remain: ...
 - build-all.sh retains an Explorer build/runtime/package/smoke expectation
 - .gitignore retains an Explorer build/runtime/package/smoke expectation
```

The checker uses `git -C <repo> ls-files -- ExplorerDemo`, so it verifies the
index rather than only the working filesystem. After staging the requested
deletions, that command produced no paths and the checker passed.

## Closure Contracts

- Public `matter/streaming.h` types remain data-only; the audit rejects
  coordinator, streamer, render, worker, mutex, smart-pointer, and raw-pointer
  storage in public type bodies.
- Every discovered Makefile source graph containing Runtime includes the ECS
  streaming system and coordinator exactly once.
- The Viewer Linux and Windows app graphs each compile the streaming anchor
  controller once. The audit verifies ImGuizmo's include/path/source, upstream
  pin, license, and flattened Windows basename uniqueness.
- `MatterViewer/main.cpp` contains no `set_bake_focus` call.
- Task 4 durable capacity, exact cancellation, pending eviction, profile gate,
  transaction, and one app-eviction-helper contracts remain source-audited.

## Fresh Verification

```text
powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers/sdd/box3d-phase2-static-check.ps1
PASS: Box3D Phase 2 build contract

powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers/sdd/sector-streaming-phase3-static-check.ps1
PASS: Sector streaming Phase 3 closure

phase3-task4-round4-{async,coord,ecs,physics,streamer}.exe
ALL PASS

phase3-task5-focused-test.exe
ALL PASS

MSVC C++17 compile-only MatterViewer/ui.cpp and MatterViewer/main.cpp
exit 0

git diff --cached --check
exit 0
git diff --check
exit 0
```

The compile-only Viewer check used the existing Windows source-build defines and
include graph. It emitted one pre-existing `APIENTRY` macro redefinition warning
between Windows headers and GLFW; no source was changed for that warning.

## Scope and Concerns

No screenshot, flight, performance, package, smoke-runtime, or GPU automation
was run. GNU Make is unavailable on this host, so the canonical Make targets
were not invoked; the available focused executables and MSVC compile-only gate
were used instead. The unrelated pre-existing modification to
`.superpowers/sdd/progress.md` remains unstaged and excluded from this commit.

## Fix Round 1: Active-Checker and Lifecycle-Audit Hardening

### RED Evidence

The Phase 3 active-automation scan was first tightened to reject either slash
form of an Explorer path. Before changing the older Flecs checker it failed as
intended:

```text
FAIL: Sector streaming Phase 3 closure (1 issue(s))
 - .superpowers\sdd\flecs-task-7-static-check.ps1 retains an Explorer path or build/runtime/package/smoke expectation
```

### Changes

- `flecs-task-7-static-check.ps1` no longer reads `ExplorerDemo/Makefile` or
  checks an Explorer source graph. Its engine/tests/Viewer Flecs closure is
  retained and its now-stale C99 rules were updated to the current C17 rules.
- The Phase 3 active-automation scan now rejects `ExplorerDemo[\\/]` in every
  active checker/gate, catching both Windows and POSIX path forms.
- Lifecycle/admission checks now strip C++ comments, extract balanced function
  bodies from anchored implementation signatures, and require the relevant
  statements inside `Coordinator::next_request`,
  `SectorStreamer::cancel_request`, the session stream-admission step, the
  clear/barrier function, and the app eviction helper. A built-in fixture proves
  a commented-out cancellation token cannot satisfy the checker while a real
  statement can.
- The Viewer audit now requires `$(IMGUIZMO_SRC)` exactly once in each of
  `IMGUI_SRC_LINUX` and `IMGUI_SRC_WIN` in addition to its standalone source
  declaration.

### Verification

From both the repository root and a different current directory, all three
path-robust checkers passed:

```text
PASS: Flecs Task 7 build contract
PASS: Box3D Phase 2 build contract
PASS: Sector streaming Phase 3 closure
```

Focused controller, ECS, and coordinator executables each printed `ALL PASS`.
Both staged and unstaged `git diff --check` exited zero. No product source,
GPU automation, screenshot, smoke, performance, package, or flight test was
run. The ignored Task 7 brief scratch artifact remains available for review at
`.superpowers/sdd/phase3-task-7-brief.md` and was not force-tracked.
