# Flecs ECS Foundation Final-Review Fix Report

Date: 2026-07-17
Branch: `codex/flecs-ecs-foundation`
Plan: `docs/superpowers/plans/2026-07-17-flecs-ecs-foundation.md`

## Status

All four Important final-review findings are resolved under focused TDD. The three low-risk deferred Minor test gaps are also closed. Two fresh focused MSVC C17/C++17 compile-link-run verifications and the strengthened static build-contract checker pass.

The host still has no usable GNU, MinGW, or WSL Linux toolchain, and the GPU/manual product gates were outside this focused review-fix run. Those gates remain unverified and are not claimed here.

## Important fixes

### 1. Shared build include contract

- Added `-I$(FLECS_DIR)` to the shared `INCLUDE_PATHS` assignments in `MatterViewer/Makefile` and `ExplorerDemo/Makefile`, so public engine headers that expose Flecs compile on non-Windows paths too.
- Strengthened `.superpowers/sdd/flecs-task-7-static-check.ps1` to require Flecs in both products' shared include blocks.
- RED checker result before the Makefile fix:

```text
FAIL: Flecs Task 7 build contract (2 issue(s))
 - MatterViewer shared include paths missing '-I$(FLECS_DIR)'
 - ExplorerDemo shared include paths missing '-I$(FLECS_DIR)'
```

### 2. One Flecs frame lifecycle per valid Runtime tick

- `Runtime::tick` now enters one RAII-managed `frame_begin`/`frame_end` scope after input validation and delta clamping, before command drains and pipeline progress.
- Invalid ticks return before the scope and therefore do not begin/end a frame or run post-frame actions.
- Valid zero-delta ticks pass negative signed zero. Pinned Flecs v4.1.6 uses a bitwise `ECS_EQZERO` check in `ecs_frame_begin`: positive zero requests measured wall time, while negative signed zero is numerically zero but bypasses that substitution. This preserves explicit zero time while still advancing one frame and running post-frame work.
- Tests assert multi-fixed-step ticks still advance one frame, pipeline work observes `EcsWorldFrameInProgress`, post-frame callbacks run once, contributed delta reaches Flecs frame info, explicit zero does not advance world time, and every invalid-input case leaves lifecycle counters unchanged.
- RED runtime result before the implementation:

```text
FAIL: one valid runtime tick advances exactly one Flecs frame
FAIL: frame pipeline runs inside the Flecs frame lifecycle
FAIL: valid runtime tick executes Flecs post-frame actions once
FAIL: Flecs frame info receives the contributed frame delta
FAIL: zero-delta tick still advances exactly one Flecs frame
FAIL: zero-delta frame pipeline runs inside the Flecs frame lifecycle
FAIL: zero-delta tick executes Flecs post-frame actions once
7 FAILURE(S)
```

### 3. Deterministic cross-child hierarchy command conflicts

- The hierarchy drain now extracts and sorts full `flecs::entity_t` child IDs before applying the per-child last-write-wins commands.
- A queued `A -> B` / `B -> A` cycle conflict now has a stable rule: the lower full child ID is considered first and its edge survives, regardless of enqueue order or unordered-map iteration order.
- RED assertion before the implementation:

```text
FAIL: cross-child survivor is independent of enqueue/hash iteration order
```

### 4. One world-transform write per affected entity per propagation pass

- Transform propagation now keeps a world-matrix cache local to one Flecs system run.
- A dirty descendant may compute missing/dirty ancestors once, while later dirty rows reuse cached matrices. Clean ancestors with an existing `WorldTransform` are read and cached without a redundant write.
- Each computed entity stages one `WorldTransform` set and one dirty-tag removal; final hierarchy results and missing-ancestor behavior remain intact.
- A three-level observer test counts `OnSet<WorldTransform>` notifications after dirtying the root and verifies exactly one write for the root, child, and grandchild plus the expected translations.
- RED assertion before the implementation:

```text
FAIL: one dirty cascade writes and notifies each affected entity once
```

## Deferred Minor test gaps closed

- JSON reflection round-trip now verifies the nested written `translation.x` value.
- Reflection tests assert `Mat4f::m` is a 16-float array, `WorldTransform::matrix` has `Mat4f` metadata, every `WorldStatus` enum constant is registered, and transform/phase/pipeline tags are fieldless.
- Hierarchy composition now covers parent rotation and nonuniform scale combined with child translation and scale.

## Focused verification

The verification intentionally compiles Flecs as C17 exactly once and the ECS test/runtime/transform translation units as C++17, then links and executes the focused suite twice from each fresh build directory.

| Gate | Result |
|---|---|
| Fresh MSVC build `msvc_final_review_verify1` | PASS |
| `msvc_final_review_verify1/ecs_tests.exe`, execution 1 | `ALL PASS` |
| `msvc_final_review_verify1/ecs_tests.exe`, execution 2 | `ALL PASS` |
| Fresh MSVC build `msvc_final_review_verify2` | PASS |
| `msvc_final_review_verify2/ecs_tests.exe`, execution 1 | `ALL PASS` |
| `msvc_final_review_verify2/ecs_tests.exe`, execution 2 | `ALL PASS` |
| Strengthened Flecs Task 7 static checker | PASS |
| `git diff --check` | PASS |

## Files changed

- `.superpowers/sdd/flecs-task-7-static-check.ps1`
- `.superpowers/sdd/flecs-final-review-fix-report.md`
- `.superpowers/sdd/progress.md`
- `MatterViewer/Makefile`
- `ExplorerDemo/Makefile`
- `MatterEngine3/src/ecs/ecs_runtime.cpp`
- `MatterEngine3/src/ecs/transform_system.cpp`
- `MatterEngine3/tests/ecs_tests.cpp`
