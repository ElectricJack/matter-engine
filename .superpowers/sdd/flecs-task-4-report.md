# Flecs Task 4 Implementation Report

## Status

Implemented and committed Task 4 as `eb504b9` (`feat(ecs): add hierarchical transform propagation`). The focused Visual Studio 2022 C/C++17 suite passes. The required GNU/WSL command remains **BLOCKED BY ENVIRONMENT** because WSL has no installed distribution; it is not reported as passing.

## Implementation

- Added pure header-only `trs_matrix(const LocalTransform&)` math using row-major storage and column-vector algebra. Translation occupies indices 3, 7, and 11. Finite non-zero quaternions are normalized; zero-length and non-finite quaternions use identity rotation.
- Added validated `reparent` rejection for null, dead, cross-world, self-parent, descendant-parent, and already-cyclic ancestor chains before calling Flecs.
- Added `clear_parent`, explicit deferred structural mutation, and dirty propagation over the changed subtree.
- Documented `reparent`/`clear_parent` as the supported hierarchy APIs, raw `ChildOf` mutation as bypassing MatterEngine validation, and parent destruction as retaining Flecs cascade-delete ownership.
- Added `OnSet<LocalTransform>` and `OnRemove(ChildOf, *)` observers that dirty affected subtrees.
- Added cached cascade-ordered transform systems in `FixedPostUpdate` and `FrameUpdate`, tagged for the later fixed/frame pipelines. The phase dependency graph is registered, but no Task 5 runtime, accumulator, or pipeline runner was added.
- Propagation builds the local-transform ancestor chain root-first, writes missing/current ancestor world matrices before descendants through the Flecs stage, and removes `TransformDirty` only after the complete chain can be computed.

## Test Cases

The focused suite covers:

1. Root translation, non-unit quaternion normalization, rotation, and scale produce the expected row-major `Mat4f`.
2. Zero-length and non-finite quaternions fall back to identity rotation.
3. Parent `(10,0,0)` plus child `(0,2,0)` yields `(10,2,0)`.
4. A three-level hierarchy propagates a root local-transform change and dirties/clears the full subtree.
5. Reparenting changes child and grandchild world transforms and dirties both.
6. `clear_parent` preserves the local transform and recomputes the entity as a root.
7. Root-to-grandchild cycle creation returns `false`, does not abort, and leaves hierarchy/world transforms unchanged.
8. Null, dead, cross-world, and self-parent requests return `false`.
9. Direct explicit `(ChildOf, *)` removal is observed and dirties/recomputes the detached subtree.
10. Destroying a parent cascade-deletes its child and grandchild under Flecs ownership semantics.
11. Existing lifecycle, deferred mutation, and reflection cases remain passing.

All float comparisons use epsilon `1e-5f`.

## Test-First Evidence

Tests were added before any production implementation changes.

### Required GNU command (RED and GREEN attempts)

Run before and after implementation:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-ecs'
```

Both attempts exited 1 before Make started:

```text
Windows Subsystem for Linux has no installed distributions.
```

Result: **BLOCKED BY ENVIRONMENT**, not passing.

### Supplemental MSVC RED

The exact compiler/link sequence was executed through the VS 2022 environment after adding tests and before production implementation:

```powershell
New-Item -ItemType Directory -Force 'D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red' | Out-Null
cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c17 /W0 /TC /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs\flecs.c" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\flecs.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\ecs_tests.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\ecs_tests.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\ecs_runtime.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\ecs_runtime.obj" && cl /nologo /std:c++17 /EHsc /W3 /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\include" /I"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\Libraries\flecs" /c "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\src\ecs\transform_system.cpp" /Fo"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\transform_system.obj" && cl /nologo /EHsc "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\ecs_tests.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\ecs_runtime.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\transform_system.obj" "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\flecs.obj" /Fe"D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\ecs_tests.exe" && "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\flecs-ecs-foundation\MatterEngine3\tests\build\msvc_task4_red\ecs_tests.exe"'
```

Exit 1, expected missing-feature failure:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ecs_tests.obj : error LNK2019: unresolved external symbol "bool __cdecl matter::ecs::reparent(struct flecs::entity,struct flecs::entity)" ...
ecs_tests.obj : error LNK2019: unresolved external symbol "void __cdecl matter::ecs::clear_parent(struct flecs::entity)" ...
ecs_tests.exe : fatal error LNK1120: 2 unresolved externals
```

### Supplemental MSVC GREEN and fresh verification

The same command sequence was run after implementation with build directory `msvc_task4_green`, then run fresh immediately before commit with build directory `msvc_task4_verify`. `flecs.c` was compiled with `/TC /std:c17`; all engine and test sources were compiled with `/std:c++17 /EHsc /W3`.

Fresh verification exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

`git diff --check` and `git diff --cached --check` also exited 0 before commit. The cached scope contained exactly the five files listed below.

## Files Changed

- `MatterEngine3/include/matter/ecs.h`
- `MatterEngine3/src/ecs/ecs_runtime.cpp`
- `MatterEngine3/src/ecs/transform_math.h`
- `MatterEngine3/src/ecs/transform_system.cpp`
- `MatterEngine3/tests/ecs_tests.cpp`

## Self-Review

- Confirmed TRS signs and column scaling against the expected 90-degree Z rotation matrix and the existing row-major/column-vector convention.
- Confirmed ancestor validation occurs before `child_of`, so the cycle case cannot reach a Flecs assertion.
- Confirmed raw direct removal observer timing with the test: the detached entity and its current descendant are dirty before progression.
- Confirmed cascade destruction does not attempt to preserve descendants and completes without assertion.
- Confirmed systems use cached optional-parent cascade terms and stage-backed structural writes; the fixed system clears dirty tags before the frame system can repeat work.
- Confirmed no scheduler runtime, accumulator, `WorldSession`, physics, sectors, UI, scripting, persistence, or networking implementation was added.
- Corrected the public documentation during review to say raw `ChildOf` edits bypass MatterEngine validation without incorrectly claiming direct removal bypasses the registered dirty observer.
- No Critical or Important issues found in the completed self-review.

## Concerns

- The required GNU Make/g++/gcc gate cannot be verified until a WSL distribution or another supported GNU environment is available. The MSVC evidence is supplemental and does not replace that blocked gate.
- Direct raw `ChildOf` additions/reparents remain unsupported and bypass MatterEngine cycle validation by design; callers must use `reparent`/`clear_parent`.

## Review Fixes

Task 4 review identified three issues in commit `eb504b9`. Focused regressions were added before production fixes.

### Fixes

- Added an internal world-owned `HierarchyValidationState` singleton whose `pending_parents` map contains hierarchy mutations queued but not yet merged. `reparent` walks this effective hierarchy, so `reparent(second, first)` followed by `reparent(first, second)` inside one outer defer rejects the second call before Flecs receives a cycle. `(ChildOf, *)` `OnAdd`/`OnRemove` observers clear committed pending entries.
- Changed the dirty query term to read access and added the explicit stage access term `.write<TransformDirty>()`. This creates the required merge before the later frame-phase system reads the tag.
- Normalized child and parent worlds with public pinned `ecs_get_world` for equality. Mutation still uses `child.world()`, preserving the originating stage/command queue.

### Review RED

The exact complete MSVC compiler/link chain shown under **Supplemental MSVC RED** above was rerun with every `msvc_task4_red` build-directory occurrence replaced by `msvc_task4_review_red`. The three regressions were present and production was unchanged. Compilation/linking completed, then the executable exited 1 before the harness flushed output when Flecs received the deferred cycle:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
```

To capture the two non-crashing failures independently, only the deferred-cycle test invocation was temporarily omitted and the same exact chain was run with build directory `msvc_task4_review_red_nocycle`. After correcting the test-only stage construction to the pinned `readonly_begin()` + `get_stage()` + `entity.mut(stage)` API, the behavioral RED was:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
FAIL: fixed propagation merges dirty removal before frame propagation
FAIL: same-real-world different-stage reparent succeeds
FAIL: staged reparent commits through the originating stage
3 FAILURE(S)
```

The deferred-cycle invocation was restored before production changes.

### Review GREEN

The same exact C17/C++17 compiler/link chain was run with build directory `msvc_task4_review_green` after all three minimal fixes. Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

### Review Fresh Verification

Immediately before the fix commit, the complete chain was run from a fresh `msvc_task4_review_verify` directory. `flecs.c` used `/TC /std:c17 /W0`; all C++ translation units used `/std:c++17 /EHsc /W3`. Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

The required GNU command was also rerun exactly:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-ecs'
```

It remains **BLOCKED BY ENVIRONMENT**, exiting 1 before Make with:

```text
Windows Subsystem for Linux has no installed distributions.
```

`git diff --check` exited 0 during fix self-review. The regression suite retains the true cross-world rejection in addition to the new same-real-world/different-stage acceptance case.

## Second Review Fix: Merge-Time Observer Reentrancy

The second review found that indiscriminate pending-parent erasure in the engine's early `ChildOf` observers exposed stale/intermediate committed ancestry to later user observers in the same Flecs event sequence.

### Regressions Added First

- A child initially parented to B queues supported reparenting to C. A later user `OnRemove(A,B)` observer attempts supported `reparent(C,A)` and must be rejected using pending `A -> C`.
- A child queues two replacements, first to C and finally to D. A later user observer on the intermediate `OnAdd(A,C)` attempts `reparent(D,A)` and must be rejected using final pending `A -> D`.
- A child queues explicit parent removal. A later user `OnRemove(A,B)` observer attempts `reparent(B,A)`, which must be accepted because pending `A -> root` supersedes the stale old edge during the callback.

### Second Review RED

The exact complete MSVC compiler/link chain documented above was run with build directory `msvc_task4_review2_red` after adding all three tests and before changing production. Compilation/linking completed, then the executable exited 1 during merge before the harness flushed because the reverse edge accepted by the later replacement `OnRemove` observer formed a cycle:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
```

The two crashing replacement tests were temporarily omitted only to isolate explicit-removal behavior. The same chain with build directory `msvc_task4_review2_red_clear` exited 1 with:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
FAIL: OnRemove callback sees pending clear instead of stale old parent
FAIL: reentrant parent move commits without a cycle
2 FAILURE(S)
```

All test invocations were restored before production changes.

### Minimal Fix and Pinned Event Semantics

- `OnRemove(ChildOf, *)` no longer clears nonzero pending targets.
- `OnAdd(ChildOf, *)` erases an entry only when `entity.target(ChildOf)` equals the pending target, preserving final state across nonmatching intermediate additions.
- Pending explicit removal (`target == 0`) queues an internal `PendingParentRemovalCleanup` tag from the early engine `OnRemove` observer. Under pinned Flecs v4.1.6, that deferred tag's `OnAdd` runs after the current `ChildOf` observer callback sequence. Its handler rechecks that the pending target is still zero before erasing it, then removes the cleanup tag. The explicit-clear regression proves later user `OnRemove` observers still see pending-root ancestry.
- Cleanup is world-owned ECS state; no global storage and no user observer suppression were introduced.

### Second Review GREEN

The complete compiler/link chain was run with build directory `msvc_task4_review2_green`. Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

### Second Review Fresh Verification

The complete chain was rerun from the fresh `msvc_task4_review2_verify` directory immediately before commit. `flecs.c` was C17 (`/TC /std:c17 /W0`); tests and engine sources were C++17 (`/std:c++17 /EHsc /W3`). Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

`git diff --check` also exited 0. The exact required GNU command was rerun and remains **BLOCKED BY ENVIRONMENT** with `Windows Subsystem for Linux has no installed distributions.`; it is not reported as passing.

## Third Review Resolution: One Outstanding Hierarchy Mutation

The attempted generation-token cleanup design was discarded after pinned Flecs 4.1.6 proved unable to safely apply a replacement-plus-clear sequence for the same child from inside a `ChildOf` observer. The approved bounded contract now permits at most one outstanding hierarchy mutation per child until its pending entry commits or clears. `reparent(child, ...)` returns `false` without queueing Flecs work when the child already has a pending mutation; `clear_parent(child)` similarly returns without queueing work. Callers must submit one final desired parent or root after the current merge. Task 5 will provide the engine post-merge hierarchy command queue.

The abandoned experiment left no generation counters, cleanup request entities, debug output, or ABA crash regression in production or tests. The existing world-owned pending-parent map remains the single source of deferred hierarchy validation state, and the public header documents the one-outstanding rule and the distinct `reparent`/`clear_parent` rejection semantics.

### Approved Contract Regression Added First

The replacement regression directly removes a child's raw `ChildOf` relationship to enter a user `OnRemove` observer without a preexisting supported mutation. Inside that observer it verifies:

1. `reparent(child, callback_parent)` is accepted as the first supported mutation.
2. Immediate `clear_parent(child)` is ignored while that reparent is pending.
3. A second `reparent(child, rejected_parent)` returns `false` while the first mutation is pending.
4. The child commits to `callback_parent` without a Flecs failure.
5. A later single post-merge reparent succeeds and commits to `rejected_parent`, proving the guard clears.

This regression replaces the previous intermediate-`OnAdd` test expectation that two same-child replacements could be outstanding, which is intentionally unsupported by the approved contract.

### Approved Contract RED

Before production changes, the exact complete MSVC compiler/link sequence documented under **Supplemental MSVC RED** was rerun with every `msvc_task4_red` build-directory occurrence replaced by `msvc_task4_contract_red`. Compilation and linking succeeded, then the executable exited 1 with the two expected missing-guard failures:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
FAIL: second reparent is rejected while the child mutation is pending
FAIL: pending clear is ignored and the first reparent commits
2 FAILURE(S)
```

### Approved Contract GREEN and Fresh Verification

After adding only the two pending-entry API guards and the public documentation, the same full C17/C++17 compiler/link chain ran with build directory `msvc_task4_contract_green`. Exit 0:

```text
flecs.c
ecs_tests.cpp
ecs_runtime.cpp
transform_system.cpp
ALL PASS
```

The complete chain was then rerun from the fresh `msvc_task4_contract_verify` directory immediately before commit. `flecs.c` used `/TC /std:c17 /W0`; tests and engine sources used `/std:c++17 /EHsc /W3`. Exit 0 with the same `ALL PASS` output.

The required GNU command was also rerun exactly:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.worktrees/flecs-ecs-foundation" && make -C MatterEngine3/tests run-ecs'
```

It remains **BLOCKED BY ENVIRONMENT**, exiting 1 before Make because Windows Subsystem for Linux has no installed distributions. It is not reported as passing.
