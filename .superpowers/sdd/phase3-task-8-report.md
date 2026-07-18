# Phase 3 Task 8 Report: Fast Verification and Manual Acceptance

## Delivered

- Added the user-run checklist at
  `docs/superpowers/specs/2026-07-18-sector-streaming-manual-acceptance.md`.
  It covers setup and every required attach/follow/detach/gizmo/remove/re-add,
  regenerate, duplicate-owner, closed-world, and stale-counter observation.
  It also states why this is manual in-app acceptance and excludes screenshot,
  long-flight, cinematic, performance, and GPU-runtime automation.
- Closed the Task 6 stale-hover minor. `viewer::Ui` resets a private
  `gizmo_submitted_` flag each successful ImGui frame, marks it immediately
  before the detached translation `Manipulate` call, and evaluates
  `IsOver(ImGuizmo::TRANSLATE)` only when that frame submitted the gizmo.
  `IsUsing()` remains unconditional for safe active-operation capture.

## TDD evidence for the minor

The Phase 3 source checker was extended before changing production source. Its
fresh RED run exited 1 with the intended four missing contracts:

```text
Viewer per-frame gizmo submission state
Viewer frame resets gizmo submission state
Viewer gizmo submission precedes manipulation
Viewer hover capture requires this-frame gizmo submission
```

After the minimal state/reset/submission/guard implementation, the same
current-source checker exited 0 and printed `PASS: Sector streaming Phase 3
closure`.

## Fresh fast verification

All commands below ran from the current Task 8 working source. No screenshot,
flight, long-flight, cinematic timing, performance, or GPU runtime automation
was run.

| Gate | Result |
| --- | --- |
| `powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers/sdd/flecs-task-7-static-check.ps1` | exit 0, PASS |
| `powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers/sdd/box3d-phase2-static-check.ps1` | exit 0, PASS |
| `powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers/sdd/sector-streaming-phase3-static-check.ps1` | exit 0, PASS |
| Same Phase 3 checker with current directory `MatterEngine3` | exit 0, PASS |
| Fresh MSVC C17 coordinator test (`sector_streaming_coordinator_tests.cpp`, coordinator, streamer, Flecs) | exit 0, `ALL PASS` |
| Fresh MSVC C17 async-queue test (`async_queue_tests.cpp`, `async_bake.cpp`) | exit 0, `ALL PASS` |
| Fresh MSVC C17 ECS Runtime test (all Runtime/physics/streaming TUs, Flecs, fresh MSVC-built Box3D C objects) | exit 0, `ALL PASS` |
| Fresh MSVC C17 physics test (same closed source graph) | exit 0, `ALL PASS` |
| Fresh MSVC C17 focused Viewer controller/Flecs executable | exit 0, `ALL PASS` |
| Fresh MSVC C17 compile of vendored `Libraries/ImGuizmo/ImGuizmo.cpp` | exit 0 |
| Fresh MSVC C17 compile of `MatterViewer/ui.cpp` and `MatterViewer/main.cpp` with the Windows Viewer definitions | exit 0; one existing `APIENTRY` macro-redefinition warning from Windows headers/GLFW |
| Fresh MSVC C20 compile of `MatterEngine3/src/matter_engine.cpp` | exit 0 |
| Fresh MSVC C20 compile of `MatterEngine3/tests/world_stream_tests.cpp` | exit 0 |
| `git diff --check -- . ':(exclude)Libraries/ImGuizmo'` | exit 0 |
| `git diff --check` | exit 0 |
| `git ls-files ExplorerDemo` | exit 0, no paths |

The source-only checks and executable rebuilds use the repository's current
sources. The ECS and physics executables compile the existing Box3D C sources
with MSVC because the tracked `Libraries/box3d/libbox3d.a` is a GNU archive and
MSVC rejects it as `LNK1136`; this changes neither Box3D nor product source.

## Unavailable or intentionally omitted gates

- The pure `SectorStreamer` executable was not run fresh because its only
  existing regression includes the prohibited long-flight case. Task 7 records
  its earlier evidence; this report makes no fresh-run claim.
- `matter_engine.cpp` does not compile under C++17 in this environment because
  MSVC reports C7555 at the pre-existing designated initializer
  `{.camera_cut = frame.swapchain_recreated}`. This is not a Phase 3 change:
  the exact expression exists at `MatterEngine3/src/matter_engine.cpp:3235` in
  baseline `11aabdc`, was introduced in `297abd2e` (2026-07-14), and current
  blame attributes the shifted line to the same pre-Phase-3 work. The TU was
  compile-checked with the C++20 mode required by that syntax; no C++17 product
  compile pass is claimed.
- `MatterViewer/tools/check_vulkan_viewer.ps1` was not run. Its required input
  `MatterViewer/tools/smoke_vulkan_interop_faults.ps1` remains absent; the path
  is also absent in baseline `11aabdc`. The checker reads it unconditionally.
  Task 8 did not alter unrelated smoke infrastructure.
- GNU Make remains unavailable on this host, so canonical GNU test recipes and
  full Viewer-logic execution are not claimed.

## Requirement checklist

| Approved design / task requirement | Evidence |
| --- | --- |
| No streaming without an explicit component; one active owner | Existing ECS/coordinator executable gates passed; manual checklist starts with explicit attach. |
| Camera-independent resolved anchor controls streaming | Existing ECS Runtime gate passed; manual follow and detached-gizmo checks are included. |
| Generation-safe attach/detach/reload/regenerate/publication lifecycle | Existing coordinator/ECS/async gates passed; manual remove/re-add and regenerate checks are included. |
| No phantom/stale residency | Existing coordinator/ECS/async gates passed; checklist requires watching resident/inflight/generation over detach/reload/regenerate. |
| MatterViewer follow plus detached translation gizmo | Focused controller passes; ImGuizmo compiles; UI/main compile; stale invisible-hover guard has a RED/GREEN source contract. |
| ExplorerDemo retirement | All three closure checkers pass; `git ls-files ExplorerDemo` has no output. |
| User-run manual acceptance | The new checklist contains the eight required scenarios and the explicit automation boundary. |

The working tree also contained an unrelated, pre-existing modification to
`.superpowers/sdd/progress.md`; it is deliberately excluded from the Task 8
commit.
