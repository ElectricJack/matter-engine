# ECS Sector Streaming Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make procedural sector streaming an optional runtime ECS capability in MatterEngine3, control it through a movable MatterViewer anchor, and retire ExplorerDemo.

**Architecture:** A public activation/error/status contract stays data-only in Flecs while a private session-owned coordinator serializes every `SectorStreamer` mutation on the existing worker. A post-transform frame system submits the active anchor's resolved X/Z position; app/GPU publication reports generation-tagged results back to the worker before residency changes. MatterViewer owns only editor interaction state and writes `LocalTransform` on an ordinary ECS entity.

**Tech Stack:** C++17, Flecs 4.1.6, existing MatterEngine3 worker and app/GPU queue, existing `SectorStreamer`, Dear ImGui, vendored ImGuizmo, GNU Make/PowerShell build checks, targeted headless tests.

## Global Constraints

- `SectorStreaming` is runtime-only and activation-only; world definitions own sector size, rings, rungs, hysteresis, inflight limits, and bake policy.
- Exactly one active streaming owner exists per client `WorldSession`; rejected duplicates are never promoted automatically.
- The resolved `WorldTransform` X/Z of the full generational anchor entity ID is the only spatial input; cameras and `LocalTransform` are not engine streaming inputs.
- Missing `WorldTransform` is pending, while unsupported worlds and duplicate owners receive reflected recoverable errors without failing world installation.
- Flecs observers enqueue plain intents only. Every `SectorStreamer` mutation occurs on the existing session worker; no streaming thread is added.
- Publication becomes resident only after store, world, culler, and renderer publication succeeds. Failure remains retryable and cannot create phantom residency.
- Detach, destruction, reload, regenerate, replacement, and shutdown invalidate generations and complete FIFO app/GPU eviction before private state is destroyed.
- Closed-world `set_bake_focus()` remains available for bake/refinement ordering but never drives infinite-world streaming.
- MatterViewer copies editor-camera position to a following anchor; it never binds engine streaming to the camera and never moves the editor camera when the anchor detaches.
- The Viewer gizmo is translation-only XYZ and must arbitrate ImGui/ImGuizmo capture against camera controls.
- Do not port ExplorerDemo cinematics, menus, raylib HUD, water hack, packaging, timing, or long-flight UI.
- Automated acceptance is limited to fast CPU/ECS/coordinator/Viewer-logic tests, static closure, and available compilation. Do not add screenshot baselines, cinematic timing, or long-running flight/performance gates.

---

## File Map

| File | Responsibility |
|---|---|
| `MatterEngine3/include/matter/streaming.h` | Public reflected activation, error, lifecycle, and status values; no private handles. |
| `MatterEngine3/src/streaming/sector_streaming_coordinator.h/.cpp` | Worker-owned lifecycle state machine, generation validation, coalesced anchor input, request/ack bookkeeping, immutable snapshots. |
| `MatterEngine3/src/ecs/streaming_systems.h/.cpp` | Private Flecs context reference, attach/remove intent capture, one-owner enforcement, post-transform anchor sampling. |
| `MatterEngine3/src/ecs/ecs_runtime.h/.cpp` | Runtime construction/destruction and coordinator/context wiring seam. |
| `MatterEngine3/src/matter_engine.cpp` | Existing worker/app publication integration, profile installation, shared eviction helper, reload/regenerate/shutdown barriers. |
| `MatterViewer/streaming_anchor_controller.h/.cpp` | Pure editor follow/detach/selection/gizmo translation logic. |
| `Libraries/ImGuizmo/*` | Pinned ImGuizmo source and license. |
| `MatterViewer/main.cpp`, `MatterViewer/ui.h/.cpp` | Sector Streaming panel, gizmo rendering, camera-input arbitration, status display. |
| `MatterEngine3/tests/sector_streaming_coordinator_tests.cpp` | Deterministic fake-endpoint lifecycle tests. |
| `MatterEngine3/tests/ecs_tests.cpp` | Public reflection and headless ECS ownership/transform tests. |
| `MatterEngine3/tests/viewer_logic_tests.cpp` | Viewer anchor and gizmo/input tests. |
| `.superpowers/sdd/sector-streaming-phase3-static-check.ps1` | Source/archive closure, public opacity, Explorer removal, and no camera coupling gate. |

### Task 1: Public Streaming Contract and Pure Streamer Terminology

**Files:**
- Create: `MatterEngine3/include/matter/streaming.h`
- Modify: `MatterEngine3/include/matter/ecs.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/src/sector_streamer.h`
- Modify: `MatterEngine3/src/sector_streamer.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`
- Modify: `MatterEngine3/tests/sector_streamer_tests.cpp`

**Interfaces:**
- Produces: `matter::streaming::SectorStreaming`, `SectorStreamingErrorCode`, `SectorStreamingError`, `SectorStreamingState`, `SectorStreamingStatus`, and `StreamingUpdate` phase.
- Produces: unchanged `matter_stream::SectorStreamer::update(float anchor_x, float anchor_z)` behavior.

- [ ] **Step 1: Add failing reflection and default-value tests.**

Add to `ecs_tests.cpp`:

```cpp
#include "matter/streaming.h"

static void test_streaming_contract(flecs::world& world) {
    auto anchor = world.entity("streaming-contract")
        .set<matter::streaming::SectorStreaming>({});
    CHECK(anchor.has<matter::streaming::SectorStreaming>(),
          "SectorStreaming is attachable at runtime");
    const auto error = matter::streaming::SectorStreamingError{};
    CHECK(error.code == matter::streaming::SectorStreamingErrorCode::None,
          "streaming error defaults to None");
    CHECK(error.active_owner == 0, "streaming error defaults to no owner");
    const auto status = matter::streaming::SectorStreamingStatus{};
    CHECK(status.state == matter::streaming::SectorStreamingState::Detached,
          "streaming status defaults detached");
}
```

Add a streamer assertion that a `clear()` followed by a late `on_published()` returns false and leaves both counts at zero.

- [ ] **Step 2: Run focused tests and confirm the missing contract fails.**

Run: `make -C MatterEngine3/tests run-ecs run-sectorstream`

Expected: `run-ecs` fails to compile because `matter/streaming.h` and its types do not exist; the existing streamer suite still passes before its terminology-only edit.

- [ ] **Step 3: Add the exact public data contract and reflection.**

Create `streaming.h`:

```cpp
#pragma once
#include <cstdint>
#include "flecs.h"

namespace matter::streaming {
struct SectorStreaming {};
enum class SectorStreamingErrorCode : uint8_t {
    None, UnsupportedWorld, OwnerAlreadyClaimed
};
struct SectorStreamingError {
    SectorStreamingErrorCode code = SectorStreamingErrorCode::None;
    flecs::entity_t active_owner = 0;
};
enum class SectorStreamingState : uint8_t {
    Detached, PendingProfile, PendingTransform, Active, Detaching
};
struct SectorStreamingStatus {
    SectorStreamingState state = SectorStreamingState::Detached;
    uint64_t generation = 0;
    uint32_t resident_sectors = 0;
    uint32_t inflight_sectors = 0;
};
struct StreamingUpdate {};
struct StreamingModule { explicit StreamingModule(flecs::world& world); };
} // namespace matter::streaming
```

Register every enum constant/member in `StreamingModule`, make `StreamingUpdate` depend on `ecs::FrameUpdate`, and add `ecs::FramePipelineSystem` to systems using it. Import `StreamingModule` from `Runtime`. Keep these components runtime-only by adding no serializer/DSL code.

- [ ] **Step 4: Rename camera-local identifiers and comments in `SectorStreamer`.**

Keep the ABI `update(float, float)` but rename parameters/members to `anchor_x`, `anchor_z`, `last_anchor_x_`, and `last_anchor_z_`. Do not change rings, selection order, hysteresis, cooldown, or key packing.

- [ ] **Step 5: Run focused tests.**

Run: `make -C MatterEngine3/tests run-ecs run-sectorstream`

Expected: both targets print their check summaries with zero failures.

- [ ] **Step 6: Commit.**

```bash
git add MatterEngine3/include/matter/streaming.h MatterEngine3/include/matter/ecs.h MatterEngine3/src/ecs/ecs_runtime.cpp MatterEngine3/src/sector_streamer.h MatterEngine3/src/sector_streamer.cpp MatterEngine3/tests/ecs_tests.cpp MatterEngine3/tests/sector_streamer_tests.cpp
git commit -m "feat(streaming): add runtime ECS contract"
```

### Task 2: Worker-Only Streaming Coordinator

**Files:**
- Create: `MatterEngine3/src/streaming/sector_streaming_coordinator.h`
- Create: `MatterEngine3/src/streaming/sector_streaming_coordinator.cpp`
- Create: `MatterEngine3/tests/sector_streaming_coordinator_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterEngine3/Makefile`

**Interfaces:**
- Consumes: public `SectorStreamingState`, `SectorStreamingStatus`; private `matter_stream::Config`, `SectorRequest`, `Eviction`.
- Produces:

```cpp
namespace matter::streaming::detail {
struct AnchorSample { flecs::entity_t owner; uint64_t generation; float x; float z; };
struct TaggedRequest { flecs::entity_t owner; uint64_t generation; matter_stream::SectorRequest sector; };
struct TaggedEviction { flecs::entity_t owner; uint64_t generation; matter_stream::Eviction sector; };
struct Snapshot { flecs::entity_t owner; SectorStreamingStatus status; };
class Coordinator {
public:
  bool attach(flecs::entity_t owner);
  void set_profile(const matter_stream::Config* profile);
  void submit_anchor(flecs::entity_t owner, float x, float z);
  void detach(flecs::entity_t owner);
  void restart_if_attached();
  void worker_step();
  bool next_request(TaggedRequest& out);
  std::vector<TaggedEviction> take_evictions();
  void acknowledge(const TaggedRequest& request, bool published);
  Snapshot snapshot() const;
};
}
```

- [ ] **Step 1: Create deterministic failing coordinator tests.**

Cover these exact cases in `sector_streaming_coordinator_tests.cpp`: unattached/profile-ready produces zero requests; attach waits for profile; attach waits for anchor; duplicate `attach(second)` returns false and preserves first; repeated anchor samples coalesce to the last X/Z before `worker_step`; `detach` emits all resident evictions and rejects late acknowledgements; failed acknowledgement leaves zero residency and becomes requestable after configured cooldown; `restart_if_attached` increments generation once and retains owner; detach before restart prevents recreation.

Use a tiny profile for speed:

```cpp
static matter_stream::Config tiny_profile() {
    matter_stream::Config value;
    value.sector_size = 16.0f;
    value.rings = {{24.0f, 1}};
    value.hysteresis = 4.0f;
    value.max_inflight = 2;
    value.fail_cooldown_updates = 2;
    return value;
}
```

- [ ] **Step 2: Add and run the new test target to verify failure.**

Add `SECTOR_STREAMING_COORDINATOR_TARGET`, its two coordinator/streamer sources, object list, rule, `run-sectorcoord`, phony entry, and clean entry to `MatterEngine3/tests/Makefile`.

Run: `make -C MatterEngine3/tests run-sectorcoord`

Expected: compilation fails because `Coordinator` is absent.

- [ ] **Step 3: Implement the coordinator state machine.**

Use one mutex only for `snapshot_` publication and app-to-worker intent inboxes. `worker_step()` swaps/coalesces inbox data, creates/destroys the private `SectorStreamer`, calls `update`, and refreshes a copied snapshot. `acknowledge()` must enqueue an acknowledgement; only `worker_step()` may call `on_published()` or `on_failed()`. Every request and acknowledgement compares both full owner ID and generation. `detach()` invalidates generation before a later `worker_step()` calls `clear()` and drains evictions.

- [ ] **Step 4: Run coordinator and streamer tests.**

Run: `make -C MatterEngine3/tests run-sectorcoord run-sectorstream`

Expected: both targets pass; coordinator cases report zero failures.

- [ ] **Step 5: Close native archive/source graphs.**

Add `src/streaming/sector_streaming_coordinator.o` to `ME3_OBJ`, its compile dependency rule, and the source anywhere a direct-source Runtime build requires it. Do not add a thread or public coordinator header.

- [ ] **Step 6: Commit.**

```bash
git add MatterEngine3/src/streaming MatterEngine3/tests/sector_streaming_coordinator_tests.cpp MatterEngine3/tests/Makefile MatterEngine3/Makefile
git commit -m "feat(streaming): add session coordinator"
```

### Task 3: Flecs Ownership and Post-Transform Bridge

**Files:**
- Create: `MatterEngine3/src/ecs/streaming_systems.h`
- Create: `MatterEngine3/src/ecs/streaming_systems.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`

**Interfaces:**
- Consumes: `Coordinator::attach`, `submit_anchor`, `detach`, and `snapshot` from Task 2.
- Produces:

```cpp
namespace matter::streaming::detail {
struct StreamingContextRef { Coordinator* value = nullptr; };
void register_streaming_systems(flecs::world& world);
void publish_streaming_snapshot(flecs::world& world, const Snapshot& snapshot);
}
```

- [ ] **Step 1: Add failing headless ECS behavior tests.**

In `ecs_tests.cpp`, use a fake/tiny coordinator and Runtime world to assert: no component means owner zero; first add claims owner; second add retains its component and gets `OwnerAlreadyClaimed` with the first full ID; removing/re-adding second after first removal claims it; active owner without `WorldTransform` stays `PendingTransform`; setting a parent and moving only the parent changes the submitted resolved X/Z after `tick`; changing an unrelated `CameraDesc` value submits nothing; destroying the owner enqueues detach.

- [ ] **Step 2: Run the ECS target and verify failure.**

Run: `make -C MatterEngine3/tests run-ecs`

Expected: tests fail because the streaming context and observers are not registered.

- [ ] **Step 3: Register intent-only observers and the sampling system.**

`OnAdd`/`OnSet<SectorStreaming>` calls only `Coordinator::attach`; on rejection set `SectorStreamingError{OwnerAlreadyClaimed, snapshot.owner}`. `OnRemove` and destruction call only `Coordinator::detach`. The `StreamingUpdate` system reads the singleton context and active owner, validates `world.is_alive(owner)`, reads resolved `WorldTransform`, extracts translation from `matrix.m[3]` and `matrix.m[11]`, and calls `submit_anchor`. Do not query camera types or mutate `SectorStreamer`, render/store/GPU objects, or worker data.

- [ ] **Step 4: Wire Runtime lifetime safely.**

Add `std::unique_ptr<streaming::detail::Coordinator> streaming_` after `physics_`, construct it before publishing `StreamingContextRef`, and null the context before destruction. `StreamingUpdate` must run after the existing frame transform propagation. Update every Runtime-bearing source union in engine/tests/Viewer with the new focused files exactly once.

- [ ] **Step 5: Run ECS, physics, and coordinator tests.**

Run: `make -C MatterEngine3/tests run-ecs run-physics run-sectorcoord`

Expected: all pass, demonstrating physics phase ordering was not disturbed.

- [ ] **Step 6: Commit.**

```bash
git add MatterEngine3/src/ecs MatterEngine3/src/streaming MatterEngine3/tests/ecs_tests.cpp MatterEngine3/tests/Makefile MatterEngine3/Makefile MatterViewer/Makefile
git commit -m "feat(streaming): bridge ECS anchors to coordinator"
```

### Task 4: WorldSession Profile, Publication, and Lifecycle Integration

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/streaming/sector_streaming_coordinator.h`
- Modify: `MatterEngine3/src/streaming/sector_streaming_coordinator.cpp`
- Modify: `MatterEngine3/tests/sector_streaming_coordinator_tests.cpp`
- Modify: `MatterEngine3/tests/world_stream_tests.cpp`

**Interfaces:**
- Produces: `WorldSession::streaming_status() const -> streaming::SectorStreamingStatus`.
- Produces private Runtime accessors `streaming_coordinator()` and const overload for `WorldSession::Impl` only.
- Consumes tagged requests/evictions and returns publication results through `Coordinator::acknowledge`.

- [ ] **Step 1: Add failing lifecycle/publication tests.**

Extend the coordinator fake-endpoint suite to model app publication order:

```cpp
auto request = next_request(coordinator);
CHECK(coordinator.snapshot().status.resident_sectors == 0,
      "request is not resident before app publication");
coordinator.acknowledge(request, false);
coordinator.worker_step();
CHECK(coordinator.snapshot().status.resident_sectors == 0,
      "failed publication cannot create phantom residency");
```

Extend `world_stream_tests.cpp` with fast configuration assertions: opening a procedural world without an ECS activation publishes zero resident sectors; adding activation starts only after ready/profile/transform; closed-world activation reports `UnsupportedWorld` but the session remains usable; reload/regenerate preserve the entity/component; removal during reload leaves zero residency.

- [ ] **Step 2: Run focused tests and verify the old automatic behavior fails.**

Run: `make -C MatterEngine3/tests run-sectorcoord run-worldstream`

Expected: world-stream assertions fail because `install_world()` still creates/updates a streamer automatically and publication is acknowledged too early.

- [ ] **Step 3: Replace automatic streamer ownership in `WorldSession::Impl`.**

Remove the `Impl`-owned automatic `SectorStreamer` and camera/focus sampling. At successful procedural world profile installation, pass a copied `matter_stream::Config` to the Runtime coordinator; for closed-world install pass no profile. The existing worker loop calls `worker_step()`, drains `TaggedRequest`, bakes with owner/generation tags, and queues app jobs. Preserve `set_bake_focus()` solely in the closed-world bake sorting path.

- [ ] **Step 4: Fix publication and consolidate eviction.**

Extract one app-thread helper that removes sector instances and releases culler, renderer, store, and transient assets. For publication, call `PartStore::get_or_load` and perform every world/GPU mutation first; only then enqueue `acknowledge(request, true)`. Catch/handle existing load/publication failure paths by enqueueing `acknowledge(request, false)`. Never call `SectorStreamer::on_published`, `on_failed`, `resident_count`, or `inflight_count` on the app thread.

- [ ] **Step 5: Make detach/reload/regenerate/shutdown generation-safe.**

Before profile replacement or worker shutdown: invalidate, run worker clear, enqueue FIFO evictions, pump/complete the app barrier, then release the old private generation. After reload/regenerate, call `restart_if_attached()` only when the same full owner is alive and still has `SectorStreaming`. Publish the copied coordinator snapshot into ECS status and `FrameStats::resident_sectors`; expose the same data from `streaming_status()`.

- [ ] **Step 6: Run fast lifecycle gates.**

Run: `make -C MatterEngine3/tests run-sectorcoord run-ecs run-worldstream`

Expected: zero failures; no GPU window or screenshot is opened.

- [ ] **Step 7: Commit.**

```bash
git add MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/ecs/ecs_runtime.h MatterEngine3/src/streaming MatterEngine3/tests/sector_streaming_coordinator_tests.cpp MatterEngine3/tests/world_stream_tests.cpp
git commit -m "feat(streaming): integrate session lifecycle"
```

### Task 5: MatterViewer Anchor Controller

**Files:**
- Create: `MatterViewer/streaming_anchor_controller.h`
- Create: `MatterViewer/streaming_anchor_controller.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterViewer/Makefile`

**Interfaces:**
- Produces:

```cpp
namespace matter_viewer {
struct StreamingAnchorState {
    flecs::entity_t selected = 0;
    bool follow_editor_camera = true;
};
void validate_anchor(StreamingAnchorState&, flecs::world&);
void follow_camera(StreamingAnchorState&, flecs::world&, const float camera_position[3]);
void detach_follow(StreamingAnchorState&, flecs::world&);
bool apply_gizmo_translation(StreamingAnchorState&, flecs::world&, const float matrix[16]);
bool camera_input_allowed(bool imgui_capture, bool gizmo_using);
}
```

- [ ] **Step 1: Add failing pure Viewer-logic tests.**

Test that follow copies camera XYZ to selected anchor `LocalTransform`; detach changes only the follow flag and preserves translation; a dead anchor or a new Flecs world clears selection/follow state; gizmo matrix translation writes exactly XYZ while preserving rotation/scale; input is allowed only when both `imgui_capture` and `gizmo_using` are false.

- [ ] **Step 2: Run Viewer logic and verify failure.**

Run: `make -C MatterEngine3/tests run-viewer-logic`

Expected: compile failure because `streaming_anchor_controller.h` is absent.

- [ ] **Step 3: Implement the controller without rendering dependencies.**

Use full generational IDs and `world.is_alive`. `follow_camera` must set the selected entity's `LocalTransform.translation` and add `TransformDirty`; it must not call `set_bake_focus` or mutate any camera. `apply_gizmo_translation` reads column-major indices 12/13/14 and preserves rotation/scale.

- [ ] **Step 4: Add sources to focused and application graphs.**

Add `../../MatterViewer/streaming_anchor_controller.cpp` to `VIEWER_LOGIC_CPP` and the local source to Viewer application/Windows object unions exactly once.

- [ ] **Step 5: Run Viewer logic.**

Run: `make -C MatterEngine3/tests run-viewer-logic`

Expected: all Viewer logic checks pass.

- [ ] **Step 6: Commit.**

```bash
git add MatterViewer/streaming_anchor_controller.h MatterViewer/streaming_anchor_controller.cpp MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/tests/Makefile MatterViewer/Makefile
git commit -m "feat(viewer): add streaming anchor controller"
```

### Task 6: ImGuizmo and Sector Streaming Panel

**Files:**
- Create: `Libraries/ImGuizmo/ImGuizmo.h`
- Create: `Libraries/ImGuizmo/ImGuizmo.cpp`
- Create: `Libraries/ImGuizmo/LICENSE`
- Modify: `MatterViewer/Makefile`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/main.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: Task 5 controller and `WorldSession::streaming_status()`.
- Produces: an ImGui panel and translation-only `ImGuizmo::Manipulate(..., ImGuizmo::TRANSLATE, ImGuizmo::WORLD, ...)` interaction.

- [ ] **Step 1: Vendor the pinned ImGuizmo files and license.**

Copy one upstream release/commit as a coherent set into `Libraries/ImGuizmo`; record the upstream commit hash in a comment beside `IMGUIZMO_PATH` in `MatterViewer/Makefile`. Do not modify third-party source except for compilation compatibility that is covered by the Viewer translation-unit gate.

- [ ] **Step 2: Add a failing panel-state logic assertion.**

Extend `viewer_logic_tests.cpp` to create/select an anchor, attach/remove `SectorStreaming`, detach follow, and verify the controller preserves its transform and camera-input suppression while a synthetic gizmo is active.

- [ ] **Step 3: Wire ImGuizmo into both Viewer build graphs.**

Add include path `-I$(IMGUIZMO_PATH)` and `$(IMGUIZMO_PATH)/ImGuizmo.cpp` once to Linux and Windows source/object unions. Preserve unique flattened basenames and existing ImGui Vulkan backend selection.

- [ ] **Step 4: Implement the Sector Streaming panel.**

The panel must provide: Create/Select Anchor, Attach/Remove Streaming, Follow editor camera, Frame Anchor, explicit numeric seed plus Regenerate, owner ID, state text, generation, resident count, inflight count, and recoverable error text. `Create` initializes `LocalTransform`; attach sets the empty public component; remove removes it. Unsupported and duplicate errors remain visible until remove/re-add or successful claim.

- [ ] **Step 5: Implement detached translation gizmo and input arbitration.**

When selected, alive, and not following, build the anchor model matrix and call ImGuizmo in translation/world mode only. Write successful manipulation through `apply_gizmo_translation`. Suppress camera controller motion when `ImGui::GetIO().WantCaptureMouse`, `WantCaptureKeyboard`, `ImGuizmo::IsOver()`, or `ImGuizmo::IsUsing()` applies. Following copies camera position to the anchor every frame; detaching never changes the camera.

- [ ] **Step 6: Remove Viewer streaming dependence on bake focus.**

Delete the per-frame `set_bake_focus(camera_position)` call from `MatterViewer/main.cpp`. Do not remove the public API or its closed-world implementation.

- [ ] **Step 7: Run fast Viewer gates.**

Run: `make -C MatterEngine3/tests run-viewer-logic`

Run: `powershell -ExecutionPolicy Bypass -File MatterViewer/tools/check_vulkan_viewer.ps1`

Expected: Viewer logic passes; the available Windows translation/build checker reports success or an explicitly documented unavailable compiler/dependency gate, without launching a screenshot smoke.

- [ ] **Step 8: Commit.**

```bash
git add Libraries/ImGuizmo MatterViewer MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(viewer): add sector streaming editor tools"
```

### Task 7: ExplorerDemo Retirement and Build Closure

**Files:**
- Delete: `ExplorerDemo/Makefile`
- Delete: `ExplorerDemo/README.md`
- Delete: `ExplorerDemo/camera_rig.cpp`
- Delete: `ExplorerDemo/camera_rig.h`
- Delete: `ExplorerDemo/hud.cpp`
- Delete: `ExplorerDemo/hud.h`
- Delete: `ExplorerDemo/main.cpp`
- Delete: `ExplorerDemo/menu.cpp`
- Delete: `ExplorerDemo/menu.h`
- Delete: `ExplorerDemo/staged_camera.cpp`
- Delete: `ExplorerDemo/staged_camera.h`
- Delete: `ExplorerDemo/tools/flight_smoke.sh`
- Delete: `ExplorerDemo/tools/package_explorer.sh`
- Delete: `ExplorerDemo/tools/time_to_flying.sh`
- Modify: `build-all.sh`
- Modify: `.gitignore`
- Modify: `.superpowers/sdd/box3d-phase2-static-check.ps1`
- Create: `.superpowers/sdd/sector-streaming-phase3-static-check.ps1`

**Interfaces:**
- Consumes: all migrated engine and Viewer behavior from Tasks 1-6.
- Produces: one supported application surface, MatterViewer.

- [ ] **Step 1: Create a failing Phase 3 static checker.**

The PowerShell checker must fail unless: all public streaming types exist and contain no coordinator/streamer/render/worker pointer fields; coordinator and streaming ECS sources occur exactly once in every Runtime-bearing engine/test/Viewer source graph; ImGuizmo source/license/build entries exist; `MatterViewer/main.cpp` contains no streaming `set_bake_focus` call; no tracked `ExplorerDemo/` files remain; `build-all.sh`, `.gitignore`, and active static checkers contain no Explorer build/runtime expectations; Windows flattened source basenames remain unique.

Run: `powershell -ExecutionPolicy Bypass -File .superpowers/sdd/sector-streaming-phase3-static-check.ps1`

Expected: FAIL because ExplorerDemo is still tracked and active wiring remains.

- [ ] **Step 2: Remove ExplorerDemo and active support entries.**

Delete the complete tracked file list above with `apply_patch`. Remove `ExplorerDemo` from `SIMPLE_PROJECTS`, delete its warm-cache smoke block, remove Explorer binary/cache/shader/dist lines from `.gitignore`, and update the Box3D checker to validate Viewer without reading Explorer files. Do not rewrite historical docs/specs/plans that mention ExplorerDemo.

- [ ] **Step 3: Run static and focused behavior gates.**

Run: `powershell -ExecutionPolicy Bypass -File .superpowers/sdd/box3d-phase2-static-check.ps1`

Run: `powershell -ExecutionPolicy Bypass -File .superpowers/sdd/sector-streaming-phase3-static-check.ps1`

Run: `make -C MatterEngine3/tests run-sectorstream run-sectorcoord run-ecs run-physics run-viewer-logic`

Expected: both checkers PASS and all five test targets report zero failures.

- [ ] **Step 4: Commit.**

```bash
git add -A ExplorerDemo build-all.sh .gitignore .superpowers/sdd
git commit -m "chore: retire ExplorerDemo"
```

### Task 8: Final Verification and Manual Acceptance Handoff

**Files:**
- Create: `docs/superpowers/specs/2026-07-18-sector-streaming-manual-acceptance.md`
- Modify only if verification finds defects: files already named in Tasks 1-7.

**Interfaces:**
- Produces: reproducible fast verification evidence and a short user-run in-app checklist.

- [ ] **Step 1: Write the manual acceptance checklist.**

Document these six actions with expected visible results: create/attach anchor; follow camera and observe resident sectors move; detach and move anchor with XYZ translation gizmo while camera stays independent; remove and re-add and see residency return to zero then recover; regenerate a seed and confirm the same entity/component survives; repeat detach/move while watching resident/inflight counts for stale growth.

- [ ] **Step 2: Run the complete fast gate.**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .superpowers/sdd/box3d-phase2-static-check.ps1
powershell -ExecutionPolicy Bypass -File .superpowers/sdd/sector-streaming-phase3-static-check.ps1
```

Run:

```bash
make -C MatterEngine3/tests run-sectorstream run-sectorcoord run-ecs run-physics run-viewer-logic
```

Expected: every checker and test target passes. Do not substitute screenshot or long-flight automation.

- [ ] **Step 3: Run available product compilation.**

Run: `powershell -ExecutionPolicy Bypass -File MatterViewer/tools/check_vulkan_viewer.ps1`

Run: `git diff --check`

Expected: available compilation/static checks pass and `git diff --check` prints nothing. If GNU/MinGW/GPU prerequisites are absent, record the exact unavailable gate; do not claim it passed.

- [ ] **Step 4: Perform final review against completion criteria.**

Inspect the branch diff and confirm: opt-in zero-sector default; one owner; resolved transform input; generation-safe lifecycle; failure retry without phantom residency; Viewer follow/detached gizmo; manual checklist; no tracked ExplorerDemo. Fix any finding with the smallest targeted test first, rerun the affected focused gate, then rerun Step 2.

- [ ] **Step 5: Commit verification artifacts and any fixes.**

```bash
git add docs/superpowers/specs/2026-07-18-sector-streaming-manual-acceptance.md
git add -u
git commit -m "test(streaming): close phase 3 acceptance"
```
