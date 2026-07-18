# Phase 3 Task 6 Report: ImGuizmo and Sector Streaming Panel

## Result

Implemented the Windows MatterViewer Sector Streaming editor panel and
translation gizmo. The Viewer can create, select, and clear runtime anchors;
attach/remove `SectorStreaming`; follow the editor camera; frame a detached
anchor; regenerate with an explicit `uint64_t` seed; and display the full owner
ID, coordinator state, generation, resident/inflight counts, and recoverable
errors. Selection is validated against the current Flecs world before access.

The detached gizmo uses `ImGuizmo::TRANSLATE` in `ImGuizmo::WORLD`, explicitly
transposes engine row-major matrices to/from ImGuizmo memory, preserves anchor
rotation/scale, and blocks camera motion for either ImGui capture flag or either
ImGuizmo hover/use flag. MatterViewer no longer calls `set_bake_focus` every
frame; the public closed-world API remains unchanged.

## Dependency

Vendored unmodified `ImGuizmo.h`, `ImGuizmo.cpp`, and `LICENSE` from the
official repository at commit
`dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d`. `Libraries/ImGuizmo/UPSTREAM.md`
records the source, exact pin, upstream paths, date, and SHA-256 hashes. No Git
submodule was added. `MatterViewer/Makefile` records the same pin beside
`IMGUIZMO_PATH` and compiles the source once in each Linux/Windows ImGui graph.

## TDD evidence

Canonical Viewer logic assertions were added before helper/UI wiring for
create/select/attach/remove, duplicate error retention, camera follow and
detach preservation, session replacement, row-major/ImGuizmo round trips and
translation, framing, and the four capture inputs.

The focused MSVC RED compile failed on the missing lifecycle/matrix/framing
APIs and the old two-argument capture predicate. After the minimal helper
implementation, the current-source focused C17/C++17 executable printed
`ALL PASS`. Self-review then found that the model helper did not yet normalize
quaternions like the engine transform system; a new assertion failed with
`FAIL: model matrix normalizes quaternion like engine transforms`, and passed
after matching the engine normalization/fallback convention.

## Verification

- Fresh focused MSVC C17/C++17 Flecs/controller suite: `ALL PASS`.
- Same gate compiles official vendored `ImGuizmo.cpp` against vendored ImGui.
- Focused MSVC production compile of `MatterViewer/ui.cpp` and `main.cpp`:
  `VIEWER COMPILE PASS`.
- Current ECS regression: `ALL PASS`.
- Current sector coordinator regression: `ALL PASS`.
- Box3D Phase 2 static checker: `PASS: Box3D Phase 2 build contract`.
- `git diff --check -- . ':(exclude)Libraries/ImGuizmo'`: exit 0.

The full `MatterViewer/tools/check_vulkan_viewer.ps1` was run, but it aborts
before evaluating its assertions because the worktree does not contain the
pre-existing path `MatterViewer/tools/smoke_vulkan_interop_faults.ps1`, which
the checker reads unconditionally. GNU Make is not installed and WSL has no
distribution, so no `make run-viewer-logic` result is claimed.

The full staged `git diff --check` reports the unchanged upstream whitespace at
`Libraries/ImGuizmo/ImGuizmo.cpp:2921`. The vendored blob hash exactly matches
the official pinned commit, so that third-party line was intentionally not
edited; all first-party Task 6 changes pass the whitespace check.

## Concern

While batching subsystem verification, I mistakenly invoked the existing
streamer regression, whose output identifies an internal long-flight case.
It passed and created no screenshot/flight artifacts, but it was outside this
task's requested short gate set and was not run again.

## Fix Round 1: Current-frame capture, detached gizmo, and smoke closure

The camera input sequence now starts the current ImGui frame and constructs all
panels plus `ImGuizmo::Manipulate` before reading current ImGui/ImGuizmo capture
state. A production-consumed `CurrentFrameInputOrder` seam enforces
`begin_ui -> build_ui -> decide_capture -> camera update`; camera follow, ECS
tick, and render then consume the resulting current-frame camera. The render
overlay is still recorded after world rendering.

Detached gizmo eligibility now means a selected alive entity with
`LocalTransform` and `follow_editor_camera == false`. It deliberately does not
depend on whether `SectorStreaming` is attached, so a detached active owner can
be translated without removing its streaming component.

The Vulkan smoke target now lists and compiles
`streaming_anchor_controller.cpp` exactly once, carries
`-I$(IMGUIZMO_PATH)`, and links the existing dedicated C17 Flecs object required
by the controller. ImGuizmo remains present once through `IMGUI_SRC_WIN` and
all flattened source basenames are unique.

### TDD and verification

- RED: focused MSVC compilation failed because `CurrentFrameInputOrder` and
  `gizmo_translation_allowed` did not exist.
- GREEN: focused real-Flecs controller/ImGuizmo suite: `ALL PASS`.
- Focused MSVC `ui.cpp` and `main.cpp` compile: `VIEWER COMPILE PASS`.
- ECS regression: `ALL PASS`.
- Sector coordinator regression: `ALL PASS`.
- Box3D Phase 2 checker: `PASS: Box3D Phase 2 build contract`.
- First-party `git diff --check`: exit 0.

The actual MinGW smoke executable target was invoked directly without running
the executable. It reached the unified compile/link command with ImGuizmo,
the controller, its include paths, and Flecs present, then stopped on a
pre-existing unrelated test-source defect:
`matter::win32_process_handle_count` is referenced by
`vulkan_smoke_tests.cpp:598` but has no declaration or definition anywhere in
the repository. No screenshot, smoke runtime, or flight automation ran.

## Fix Round 2: operation-aware capture and immutable frame camera

The current-operation hover query now uses the pinned API explicitly:
`ImGuizmo::IsOver(ImGuizmo::TRANSLATE)`, paired with `ImGuizmo::IsUsing()`
after the current frame's `Manipulate` call. A compile probe includes the real
vendored headers and links the real vendored source, while the static checker
also confirms both the pinned declaration/definition and the production call.

After all panel actions and gizmo work, `main.cpp` copies `camera` to an
immutable `frame_camera`. Streaming follow, scene tick/render, UI submission,
and frame statistics all consume that snapshot. The order seam now requires
`begin/build/capture -> tick -> render -> end frame`; only then can uncaptured
free-fly input update `camera` for the next frame. This also defines Frame
Anchor precisely: its panel-time mutation is included in the snapshot used by
the later gizmo and scene render.

### TDD and verification

- RED: focused compilation failed on the missing tick/render/end order stages;
  the static checker rejected zero-argument `ImGuizmo::IsOver()` and the old
  in-frame camera update.
- Real vendored ImGuizmo compile/API probe plus focused controller behavior:
  `ALL PASS`.
- Task 6 production API/order static checker: `PASS`.
- Focused MSVC `ui.cpp` and `main.cpp` compile: `VIEWER COMPILE PASS`.
- ECS regression and sector coordinator regression: `ALL PASS`.
- Box3D Phase 2 checker: `PASS: Box3D Phase 2 build contract`.
- First-party `git diff --check`: exit 0.
- The repository Vulkan checker still aborts at its pre-existing missing
  `MatterViewer/tools/smoke_vulkan_interop_faults.ps1` input.
- A fresh compile-only smoke invocation emitted the expected unified command
  with ImGuizmo/controller/Flecs, but this sandbox run stopped before compiling
  because MSYS could not create its global temporary file. Round 1 already
  reached the known unrelated `win32_process_handle_count` source boundary.

No screenshot, runtime smoke, long-flight, or heavy automation ran in this
fix round.
