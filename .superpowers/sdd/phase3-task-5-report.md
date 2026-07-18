# Phase 3 Task 5 Report: MatterViewer Streaming Anchor Controller

## Result

Added a rendering-independent `matter_viewer::StreamingAnchorState` controller.
It validates a full Flecs entity ID against the current world, keeps camera
following and gizmo translation scoped to `LocalTransform`, preserves rotation
and scale, adds `TransformDirty`, and arbitrates camera input without touching
camera or streaming APIs.

World replacement is detected with a private ECS singleton that receives a
monotonic token. The controller stores only that integer token; it never
retains a Flecs world pointer across frames.

## TDD evidence

The canonical Viewer logic tests were written first for follow, detach,
dead/recycled IDs, world replacement, row-major gizmo translation, invalid
selection, and the four input-arbitration cases. The first MSVC C++17 compile
reached the new test include and failed as expected because
`streaming_anchor_controller.h` did not exist:

```text
viewer_logic_tests.cpp(19): fatal error C1083: Cannot open include file:
'../../MatterViewer/streaming_anchor_controller.h'
```

After implementing the controller, a fresh focused executable compiled Flecs
as C17 and the controller/test as C++17, then printed `ALL PASS`. It exercises
the same controller behavior with a real Flecs world. The canonical test
translation unit also accepts the new header but cannot be built directly by
MSVC because the pre-existing test file includes POSIX-only `unistd.h`.

`Mat4f` is row-major with column-vector algebra, so gizmo translation reads
matrix elements `3`, `7`, and `11`; the regression intentionally supplies
different values at `12`, `13`, and `14`.

## Source graphs

- `MatterEngine3/tests/Makefile:VIEWER_LOGIC_CPP` contains the controller once.
- `MatterViewer/Makefile:LINUX_APP_SRC` contains it once.
- `MatterViewer/Makefile:APP_SRC` contains it once for the Windows source union.

## Verification

- Fresh focused MSVC C17/C++17 Flecs/controller executable: `ALL PASS`.
- Existing ECS, coordinator, and physics regressions: each `ALL PASS`.
- Box3D Phase 2 static checker: `PASS: Box3D Phase 2 build contract`.
- Source-graph occurrence check: Viewer logic, Linux, and Windows each exactly once.
- `git diff --check`: exit 0.

## Environment note

GNU Make is not installed on this Windows host, so
`make -C MatterEngine3/tests run-viewer-logic` cannot run. No GNU result is
claimed. MSVC compilation of the existing full Viewer logic translation unit
also stops at its pre-existing `unistd.h` include; the real focused MSVC/Flecs
executable was used for behavioral verification instead.

## Fix Round 1: Gizmo Rejection and Recycled-ID Coverage

Added canonical and focused assertions for `apply_gizmo_translation` with an
empty selection and with a live selected entity that has no `LocalTransform`.
Both return `false` and leave the target entity unchanged: no transform is
created or edited and no `TransformDirty` tag is added. The focused MSVC/Flecs
controller executable prints `ALL PASS`, so these regressions exposed no
production bug and no controller code changed.

The recycled-ID test now uses Flecs' supported `ECS_ENTITY_MASK` and
`ECS_GENERATION` helpers. It destructs the anchor, retries allocation up to 16
times until the same entity-number bits are reused, then proves the replacement
has a different generation and is alive while the stale full ID is dead.
`validate_anchor` clears the stale selection instead of selecting that
replacement. ECS, coordinator, and physics regressions each printed `ALL PASS`;
the Phase 2 static checker passed and `git diff --check` exited 0.
