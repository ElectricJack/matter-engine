# Sector Streaming and ExplorerDemo Consolidation Design

**Status:** Implemented and manually accepted on 2026-07-18

## Goal

Move the infinite-world sector functionality into the MatterEngine3 ECS runtime,
make streaming an optional capability attached dynamically to one anchor entity,
add MatterViewer controls for camera-follow and detached gizmo inspection, then
delete ExplorerDemo and its support surface.

## Scope

Phase 3 includes:

- a runtime-only `SectorStreaming` ECS activation component;
- exactly one active streaming anchor per client `WorldSession`;
- streaming driven by the anchor entity's resolved `WorldTransform`;
- generation-safe attach, update, detach, reload, and publication flow;
- a private, session-owned streaming coordinator that reuses the existing worker;
- MatterViewer ImGui controls and an ImGuizmo translation gizmo;
- migration of essential infinite-world coverage to fast engine and Viewer tests;
- deletion of ExplorerDemo, packaging, smoke tools, and build wiring.

Phase 3 does not add ECS cameras, lights, render proxies, persistence, networking,
gameplay scripting, a new streaming thread, or Windows autoremesher support. The
legacy Linux Viewer renderer is not retired in this phase.

## Decisions

| Topic | Decision |
|---|---|
| Component role | `SectorStreaming` is activation-only; the procedural world definition owns sector size, rings, rungs, and bake policy |
| Persistence | Runtime-only; no serialization or DSL authoring changes |
| Cardinality | Exactly one active component per client session |
| Duplicate attachment | Reject the second attachment; preserve the first owner; require explicit remove/re-add to retry |
| Spatial input | Anchor entity `WorldTransform`, never a render camera |
| Missing transform | Remain pending until a resolved `WorldTransform` exists |
| Threads | Reuse the session worker and app/GPU queue; create no new thread |
| Viewer modes | Anchor follows editor camera, or detaches and moves independently through a translation gizmo |
| Gizmo | Vendor ImGuizmo and integrate it with existing ImGui/Vulkan Viewer UI |
| Automated acceptance | Prefer fast CPU, ECS, coordinator, and Viewer-logic tests; avoid screenshot and long-flight automation |
| Visual acceptance | User-performed MatterViewer checklist |

## Public ECS Contract

Add a public, reflected runtime component in a dedicated streaming header:

```cpp
namespace matter::streaming {

struct SectorStreaming {};

enum class SectorStreamingErrorCode : uint8_t {
    None,
    UnsupportedWorld,
    OwnerAlreadyClaimed
};

struct SectorStreamingError {
    SectorStreamingErrorCode code = SectorStreamingErrorCode::None;
    flecs::entity_t active_owner = 0;
};

} // namespace matter::streaming
```

The component contains no pointers, worker state, ring configuration, provider
objects, mutexes, or rendering handles. It is reflected for editor/debug inspection
but is not serialized.

The error component is recoverable. A rejected duplicate keeps its component and
error but does not become a standby owner. Removing the active owner's component
does not promote another entity automatically. The rejected entity must explicitly
remove and re-add `SectorStreaming` to retry.

Attaching to a world without a procedural streaming profile sets
`UnsupportedWorld`. Loading the world itself still succeeds and streams zero sectors.

## ECS Integration

Register a streaming module and a private context reference, following the physics
runtime pattern without exposing private handles publicly.

Observers perform intent capture only:

- `OnAdd`/`OnSet<SectorStreaming>` attempts to claim the session owner;
- `OnRemove<SectorStreaming>` and anchor destruction enqueue detach intent;
- no observer mutates `SectorStreamer`, `WorldState`, `PartStore`, Vulkan state, or
  worker-owned data directly.

A frame-pipeline system runs after derived transform propagation. For the active
owner it reads `WorldTransform.matrix.m[3]` and `matrix.m[11]`, validates the full
generational entity ID, and publishes a coalesced X/Z snapshot to the coordinator.
`LocalTransform` is never used as the streaming position because parent transforms
must affect the anchor.

The ECS world stores only the active owner ID and a private coordinator reference.
The `WorldSession` remains the lifetime owner.

## Session Streaming Coordinator

Extract a focused private coordinator from `WorldSession::Impl`. It owns:

- the current streaming generation and lifecycle state;
- the existing `SectorStreamer` desired-set algorithm;
- procedural field/profile references produced by world installation;
- resident-sector bookkeeping;
- immutable status snapshots for ECS/UI reads;
- plain attach, anchor-update, detach, reload, and publication-result messages.

The coordinator reuses the existing session worker and app/GPU job queue. Flecs
callbacks and the Viewer never access `SectorStreamer` directly.

All `SectorStreamer` mutations occur on the worker. App/GPU publication returns a
plain acknowledgement to the worker rather than calling `on_published()` from the
app thread. Frame statistics read a copied snapshot, removing unsynchronized
cross-thread reads of the streamer.

## Lifecycle State Machine

### Attach

1. Claim the full owner entity ID if no owner exists.
2. If the world profile is not ready, retain a pending attachment.
3. If `WorldTransform` is missing, retain a pending attachment.
4. When profile and transform are both ready, create the private generation and run
   the first desired-set update.

### Anchor update

The post-transform ECS system submits the latest X/Z position. Repeated positions
are coalesced. The worker applies the newest snapshot before generating requests.
Rendering and editor cameras are not consulted.

### Publication

1. The worker bakes a request tagged with owner ID and generation.
2. The app/GPU job loads the part and applies all store, world, culler, and renderer
   mutations.
3. Only after successful publication does the app side acknowledge success.
4. The worker then calls `SectorStreamer::on_published()`.
5. Failure acknowledges `on_failed()` and preserves retry/cooldown behavior.

This ordering fixes the existing phantom-resident path where a failed part load can
leave the streamer satisfied without a resident sector.

### Detach

1. Invalidate the generation so late work cannot publish.
2. Call `SectorStreamer::clear()` on the worker.
3. Send the resulting evictions through a FIFO app/GPU barrier.
4. Remove world instances and release culler, renderer, store, and transient assets.
5. Destroy the private generation and publish a zero-residency snapshot.

Component removal, anchor destruction, session shutdown, and session replacement use
the same detach machinery.

### Reload and regenerate

Stop and evict the old private generation, rebuild procedural world/profile data,
then restart exactly once only if the same full owner entity is alive and still has
`SectorStreaming`. The ECS attachment and entity survive reload/regenerate. Removal
during reload prevents the publish tail from recreating streaming.

## SectorStreamer Core

Keep the existing pure desired-set behavior:

- concentric rung selection and hysteresis;
- nearest-first hole and rung-change requests;
- bounded inflight work;
- publish-then-evict replacement;
- stale publication rejection;
- failure cooldown;
- deterministic clear and eviction.

Rename camera-specific parameters and comments to anchor/focus terminology. Do not
change the ring policy in the ECS component. The procedural world profile supplies
the policy.

Extract the duplicated sector eviction logic from `matter_engine.cpp` into one
app-thread helper. Keep closed-world `set_bake_focus()` behavior for bake/refinement
ordering; it no longer drives infinite-world sector streaming.

## MatterViewer UX

Add a Sector Streaming panel to the existing ImGui interface:

- create/select a runtime anchor entity;
- attach or remove `SectorStreaming`;
- toggle `Follow editor camera`;
- detach while preserving the anchor's current transform;
- frame the anchor;
- regenerate with an explicit seed;
- display owner ID, pending/active/detaching state, generation, resident/inflight
  counts, and recoverable errors.

When following, MatterViewer copies the editor camera world position into the anchor
entity's `LocalTransform` each frame. The camera does not follow the anchor. When
detached, the editor camera remains independently controllable and observes streaming
from an external viewpoint.

Vendor ImGuizmo. Show a translation-only XYZ gizmo for the selected detached anchor.
Rotation and scale editing are disabled because the streamer consumes X/Z position
only. Gizmo changes write the anchor `LocalTransform` through ECS. ImGui capture and
camera-controller input must not compete with gizmo interaction.

Expose `resident_sectors` and streaming status in MatterViewer. Do not port the
Explorer staged cinematic, pause menu, raylib HUD drawing, water-plane hack, or demo
packaging UI.

## Verification Strategy

Automated verification emphasizes fast targeted tests.

### Pure CPU tests

- rename camera terminology while preserving existing `SectorStreamer` behavior;
- anchor traversal produces bounded residency and deterministic eviction;
- clear rejects late publication;
- failed publication remains retryable;
- no component/profile integration is needed for these tests.

### Headless ECS and coordinator tests

- no component means zero streaming;
- dynamic attach starts after profile and `WorldTransform` become ready;
- component removal and anchor destruction evict to baseline;
- duplicate attachment rejects the second and preserves the first;
- explicit remove/re-add retries ownership;
- moving only a camera has no effect;
- parent movement changes the anchor through derived `WorldTransform`;
- detach with inflight work rejects late acknowledgements;
- reload/regenerate preserve attachment and restart one generation;
- removal during reload prevents restart;
- failed store/publication acknowledgement retries rather than creating phantom
  residency;
- session replacement invalidates old full entity IDs.

Use fake worker/publication endpoints for coordinator lifecycle tests so they remain
CPU-only and deterministic.

### MatterViewer logic tests

- follow mode copies editor-camera position to anchor transform;
- detach preserves the current anchor transform;
- session replacement or dead anchor clears Viewer-side selection/follow state;
- gizmo matrices produce the expected ECS translation;
- gizmo capture suppresses camera motion.

### Lightweight build gates

- static source/archive/build closure;
- streaming, ECS, coordinator, and Viewer-logic unit suites;
- MatterViewer translation-unit and available product compilation;
- at most one short existing startup smoke, without screenshot comparison.

Do not add automated screenshot baselines, cinematic timing, long endless-flight
smokes, or cold/warm performance gates in Phase 3.

### Manual in-app acceptance

The user verifies in MatterViewer:

1. create and attach an anchor;
2. follow the editor camera and observe sector movement;
3. detach and move the anchor with ImGuizmo while the camera stays independent;
4. remove and re-add streaming and observe eviction/recovery;
5. reload/regenerate and confirm the anchor survives correctly;
6. confirm no obvious residency leak or stale publication.

## ExplorerDemo Retirement

After the targeted engine/Viewer gates pass, delete the tracked `ExplorerDemo/`
tree, including its Makefile, README, raylib app, camera rig, HUD, menu, staged
camera, package script, flight smoke, and timing tool.

Also remove:

- Explorer project and smoke entries from `build-all.sh`;
- Explorer binary/cache/shader/dist entries from `.gitignore`;
- temporary Explorer source/link expectations from current static build checkers.

Do not rewrite historical design documents or plans merely because they mention
ExplorerDemo.

The old flight acceptance is replaced by fast coordinator/streamer traversal tests
and the manual MatterViewer checklist. If end-user packaging is needed later, it is a
MatterViewer packaging project rather than a reason to retain ExplorerDemo.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Late publish after detach/reload | Full owner ID plus generation on every request and acknowledgement |
| Worker/app data race | Worker-only streamer mutation and immutable status snapshots |
| Phantom residency on load failure | Acknowledge `on_published()` only after successful store/world/GPU publication |
| Component removal during shutdown | Explicit detach before worker shutdown; teardown observers fail closed |
| Parent motion ignored | Sample resolved `WorldTransform`, never `LocalTransform` |
| Duplicate owner ambiguity | Reject second owner with reflected error; no automatic promotion |
| Gizmo/camera input conflict | Translation-only mode plus ImGui/ImGuizmo capture arbitration |
| Scope expansion from Explorer UI | Retire demo presentation features; port only engine behavior and useful status/control surfaces |

## Completion Criteria

1. A procedural world streams zero sectors without `SectorStreaming`.
2. Exactly one runtime anchor can activate streaming per client session.
3. Anchor `WorldTransform`, independent of render cameras, controls desired sectors.
4. Attach, detach, reload, regenerate, shutdown, and late publication are generation-safe.
5. Publication failures retry and cannot create phantom residency.
6. MatterViewer supports camera-follow and detached ImGuizmo translation workflows.
7. Fast targeted CPU/ECS/Viewer tests and lightweight build gates pass.
8. The manual in-app checklist is ready for user execution.
9. ExplorerDemo and its active build/package/smoke surface are deleted.
