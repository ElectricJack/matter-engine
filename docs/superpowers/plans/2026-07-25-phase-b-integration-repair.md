# Phase B Integration Repair Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Repair four independently reproduced Phase B/C integration failures while preserving the established controller, checkpoint, build-closure, and authored-animation ownership contracts.

**Architecture:** Trace each failure from its externally asserted contract back through the narrowest producer/consumer boundary. Add or strengthen a focused regression before changing production behavior, then make only the confirmed owner responsible for the fix. Verify each repair independently before running the combined Phase B/C gates.

**Tech Stack:** C++17, EnTT ECS, Matter animation runtime, GNU Make test targets, Ozz-backed animation assets, Vulkan renderer tests.

---

### Task 1: Root-relative world-query controller target

**Files:**
- Modify: `MatterEngine3/tests/animation_simulation_tests.cpp`
- Modify if confirmed owner: `MatterEngine3/src/animation/animation_controllers.cpp`
- Inspect: `MatterEngine3/src/animation/animation_targets.cpp`
- Inspect: `MatterEngine3/src/animation/animation_systems.cpp`

**Step 1: Reproduce the focused failure**

Run: `make -j1 run-animation-simulation`

Expected: only `world query hit becomes the correctly root-relative controller-owned target` fails.

**Step 2: Trace the coordinate contract**

Read the native gait controller query-result path, the target resolver, and fixed-tick ordering. Compare the query hit's world transform, the controller root world transform, the written `AnimationTargetState::desired`, and the existing `resolve_world_target` expected value.

**Step 3: Strengthen the focused regression**

Update `animation_simulation_tests.cpp` so the scaled/translated-root scenario separately proves the query result is world-space and the controller-owned target is root-relative. Keep the assertion independent of later evaluator output.

**Step 4: Confirm the regression fails**

Run: `make -j1 run-animation-simulation`

Expected: the new coordinate-boundary assertion fails before the production fix.

**Step 5: Implement the narrow owner fix**

If the controller currently writes the world-space hit directly, convert the accepted query result through the target resolver using the controller root's current world transform before publishing `desired`. Do not alter query-service world-space semantics or evaluator target semantics.

**Step 6: Verify the focused suite**

Run: `make -j1 run-animation-simulation`

Expected: all animation simulation tests pass.

### Task 2: Checkpoint desired-target replay

**Files:**
- Modify: `MatterEngine3/tests/animation_simulation_tests.cpp`
- Modify if confirmed owner: `MatterEngine3/src/animation/animation_systems.cpp`
- Inspect: `MatterEngine3/src/animation/animation_controllers.cpp`
- Inspect: `MatterEngine3/src/animation/animation_store.cpp`

**Step 1: Reproduce the checkpoint mismatch**

Run the Phase B acceptance gate target and record the tick-4001 desired/evaluated target hashes for same-runtime and clean-runtime restore.

Expected: both restored runs diverge from the uninterrupted run only in desired/evaluated target state.

**Step 2: Trace capture/restore and the first resumed fixed tick**

Compare checkpoint capture order, controller runtime-state capture, target-state capture, restore order, fixed-work registration, query admission/results, and native-controller publication on the first tick after restore. Determine whether replay is missing serialized state or re-running an already-consumed controller/query transition.

**Step 3: Add a minimal replay regression**

Extend `animation_simulation_tests.cpp` with a short capture/restore test that records the controller-owned desired target immediately before capture and after the first resumed fixed tick in both the same and clean runtime. Assert equality with the uninterrupted timeline.

**Step 4: Confirm the regression fails**

Run: `make -j1 run-animation-simulation`

Expected: the new first-resumed-tick replay assertion reproduces the target mismatch.

**Step 5: Implement the confirmed state/order fix**

Change only the responsible checkpoint or fixed-tick boundary in `animation_systems.cpp`: restore every controller/query transition input needed by the next tick, or prevent duplicate consumption if capture occurs after that transition. Preserve checkpoint validation and transactional restore behavior.

**Step 6: Verify focused and gate suites**

Run: `make -j1 run-animation-simulation`

Run the Phase B acceptance gate target.

Expected: simulation passes and tick-4001 target desired/evaluated hashes match in uninterrupted, same-runtime, and clean-runtime runs.

### Task 3: Skin test link closure

**Files:**
- Modify: `MatterEngine3/tests/Makefile`
- Verify: `MatterEngine3/include/matter/animation.h`
- Verify: `MatterEngine3/src/animation/animation_store.cpp`
- Verify: `MatterEngine3/src/animation/animation_systems.cpp`

**Step 1: Reproduce the link failure**

Run: `make -j1 run-animation-skin`

Expected: linker reports undefined `AnimationService::sample_fixed_controls()` and `sample_frame_controls()`.

**Step 2: Compare target source closures**

Compare the animation-skin target's source list with targets that successfully link `animation_systems.cpp` and `animation_store.cpp`. Confirm the declarations, definitions, and call sites are correct and the failing target simply omits the defining translation unit or its required closure.

**Step 3: Repair the test target closure**

Update `MatterEngine3/tests/Makefile` to include the existing animation service implementation and any directly required translation units/libraries in the animation-skin test target. Do not duplicate method definitions or add test-only stubs.

**Step 4: Verify the focused suite**

Run: `make -j1 run-animation-skin`

Expected: target links and all skin bridge tests pass.

### Task 4: Phase C authored gait decoder contract

**Files:**
- Modify: `MatterEngine3/tests/animation_phase_c_acceptance_tests.cpp`
- Modify if confirmed owner: `MatterEngine3/src/animation/animation_runtime_asset.cpp`
- Inspect: authored gallery animation asset used by the acceptance test

**Step 1: Reproduce the C4 assertion**

Run the Phase C acceptance target and record the decoded graph root/type, retained clips, speed parameter mapping, and native gait controller.

Expected: `C4 decoder retains authored speed blend, idle/walk clips, and native gait controller` fails.

**Step 2: Trace authored data through decoding**

Compare the authored gallery graph, including its additive overlay wrapper, with the decoded runtime graph. Determine whether the decoder drops authored nodes/clips/controller metadata or the acceptance assertion assumes the pre-overlay graph root shape.

**Step 3: Add the precise structural regression**

Update `animation_phase_c_acceptance_tests.cpp` to assert the durable authored contract: the decoded graph contains the speed-driven idle/walk blend, both clips remain addressable, and the native gait controller remains configured. If decoding truly drops one of these, retain the failing assertion and fix the decoder instead.

**Step 4: Implement only a confirmed decoder fix**

If the runtime decoder loses nested graph/controller data, update `animation_runtime_asset.cpp` to retain that authored structure without flattening the additive graph. If the data is already retained, change only the stale root-shape assertion.

**Step 5: Verify the focused suite**

Run the Phase C acceptance target.

Expected: all Phase C assertions pass, including the additive overlay and nested speed-blend contract.

### Task 5: Vulkan skin descriptor retry after grown-buffer upload failure

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.c`
- Modify: Vulkan smoke regression covering animation skin faults

**Step 1: Trace allocation, descriptor publication, and fallible uploads**

Inspect the skin source, influence, palette, work-item, and output-buffer ensure/upload sequence. Confirm which `ensure_buffer` calls can replace handles before an upload failure returns through retained-geometry fallback.

**Step 2: Add a same-slot retry regression**

Force growth of every skin resource named above, inject the first upload failure, then recycle the same frame slot with identical sizes and the fault cleared. Assert successful skin readback and zero validation/device errors.

**Step 3: Confirm the regression fails**

Run the animation-skin-faults Vulkan smoke mode.

Expected: retry exposes stale descriptors before the production fix.

**Step 4: Refresh descriptors before fallible upload**

After all successful buffer ensures, refresh the frame slot's skin descriptor bindings before any upload that may downgrade/return. Preserve retained fallback and one-shot fault behavior.

**Step 5: Verify focused Vulkan smoke**

Run the focused Vulkan scene-renderer tests and animation-skin-faults smoke mode.

Expected: retry readback succeeds with zero validation/device errors.

### Task 6: Combined verification and commit

**Files:**
- Review all files modified by Tasks 1-4.

**Step 1: Run focused suites**

Run the animation simulation, animation skin, Phase B acceptance, and Phase C acceptance targets serially.

Expected: all pass.

**Step 2: Review ownership and workspace scope**

Run: `git diff --check`

Run: `git status --short`

Confirm no unrelated dirty files are included and no concurrent-agent files were modified.

**Step 3: Commit only this repair**

Stage only files changed by this plan and commit with a message describing the integration repairs.

**Step 4: Report evidence**

Report each root cause, owning fix, focused test outcome, gate outcome, and commit hash to the parent agent.
