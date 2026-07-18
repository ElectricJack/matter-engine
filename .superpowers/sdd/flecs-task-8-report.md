# Flecs Task 8 — WorldSession Tick Caller Migration

## Result

Migrated every remaining `WorldSession::tick()` caller to the explicit
`TickDesc` API. Viewer and Explorer loops use their existing measured frame
delta in seconds. The Linux-only live-edit E2E async pumps use
`matter::TickDesc{0.0f}` so they continue to pump provider work without
advancing fixed simulation. `live_edit::Session::tick()` calls in
`dev_live_edit_tests.cpp` remain unchanged.

## Changed Files

- `MatterViewer/main.cpp`: passes the existing `std::chrono`-derived `dt`.
- `MatterViewer/main_linux.cpp`: stores its existing single `GetFrameTime()`
  result as `frame_delta_seconds`, reuses it for camera and session tick.
- `ExplorerDemo/main.cpp`: passes the existing `GetFrameTime()` `dt`.
- `MatterEngine3/tests/async_bake_tests.cpp`: passes zero frame time in the
  three Linux-only async live-edit E2E loops.

## Compile-First Evidence

MSVC translation-unit checks used Visual Studio 18 Community with the project
defines and include roots.

Before the migration:

- `MatterViewer/main.cpp(618)`: `C2660` — `WorldSession::tick` did not take
  zero arguments.
- `ExplorerDemo/main.cpp(457)`: the same `C2660` error.
- `MatterViewer/main_linux.cpp`: could not reach the old call because MSVC
  stops at the Linux-only `unistd.h` include (`C1083`).

After the migration, neither portable caller reports `C2660` for `tick`.
Their checks still stop on pre-existing MSVC portability diagnostics:

- `MatterViewer/main.cpp(587)`: `C2589`.
- `ExplorerDemo/main.cpp(498, 523–530)`: `C4576`.
- `MatterViewer/main_linux.cpp(32)`: Linux-only `unistd.h` is unavailable to
  MSVC.

## Verification

- `rg -n "session->tick\\(\\)" MatterViewer ExplorerDemo MatterEngine3\\tests`
  exited 1 with no matches.
- Inspected every remaining parameterless `.tick()`/`->tick()` match: all 11
  are `LiveEditSession sess` calls in `dev_live_edit_tests.cpp`, a distinct
  API intentionally outside this migration.
- `git diff --check` exited 0.
- Fresh focused MSVC ECS build compiled vendored `flecs.c` as C17 and
  `ecs_tests.cpp`, `ecs_runtime.cpp`, and `transform_system.cpp` as C++17;
  it linked and ran `MatterEngine3/tests/build/msvc_task8_verify/ecs_tests.exe`
  successfully with `ALL PASS` (exit 0).

## Blocked Product Gates

`run-ecs`, `run-worldstream`, MatterViewer Windows, and ExplorerDemo Windows
GNU/MinGW Make targets were not run: this host has no GNU Make, GNU compiler,
MinGW toolchain, or WSL distribution. The MSVC checks above are supplemental
translation-unit/test evidence only; they do not make the required product
builds pass.

## Self-Review

- No build files changed.
- No second clock, camera coupling, sleep, or runtime behavior change was
  introduced.
- ExplorerDemo remains present and uses its existing real frame delta.
- Async-only test calls are the only zero-time calls and have a verified
  `WorldSession` receiver.
