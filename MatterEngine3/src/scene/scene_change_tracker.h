// scene/scene_change_tracker.h — SceneChangeTracker: publishes canonical,
// sequenced scene-row deltas from the ECS (E5b).
//
// Design authority: MatterEngine3/docs/event-system.md S I.14 / S II.1 E5.
//
// The editor-visible scene surfaces have NO existing change source: the cited
// Flecs observers only mark transforms dirty (transform_system.cpp:400) or
// schedule physics reconcile (physics_systems.cpp) — none report creates,
// deletes, names, parents, or component-list changes, and
// WorldSession::graph_generation() is the part-graph snapshot, NOT scene
// hierarchy (S I.14). So this session-owned tracker ADDS that source:
//
//   * Lightweight Flecs observers mark the affected SceneEntityId dirty (or
//     removed) for exactly the editor-visible surfaces — entity add/remove,
//     ChildOf add/remove, display-name identifier changes, and add/set/remove
//     of the components in SceneRecord::component_names. Observers only touch
//     the dirty/removed sets; they never emit (cheap, S I.14).
//   * flush(), called at END of the app-thread ECS tick (WorldSession::tick),
//     snapshots each dirty live entity ONCE and publishes one of the two
//     canonical events (scene_events.h) with the sequence/ordering rules:
//     removals first, then upserts; ids ascending in each batch; consecutive
//     sequence values.
//   * scene_snapshot(rows, seq) is the atomic (rows, sequence) recovery read.
//
// Threading (S I.14 "Threading"): scene queries, SceneService, flush, and the
// app observable model are ALL app-thread-affine because the ECS tick runs in
// the app-thread frame loop. Observers fire synchronously on the mutating
// thread (Flecs immediate mode outside defer) = the app thread. The tracker
// therefore does NOT support cross-thread producers touching Flecs directly;
// those queue a scoped command that the app lane executes (S I.14). The
// dirty/removed sets are still guarded by a mutex so a defensive off-thread
// touch cannot corrupt them, but correctness assumes app-thread affinity.
//
// Lifetime: the observers capture a shared_ptr to the dirty state, NOT `this`.
// A cascade delete during world teardown still fires OnRemove observers; they
// harmlessly mark a still-alive shared state that outlives both the tracker
// and the world. The tracker destructs its observer entities in its
// destructor so no observer outlives it holding a stale world reference.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "flecs.h"
#include "matter/scene.h"
#include "matter/scene/scene_events.h"

namespace matter::evt { class Hub; }

namespace matter::scene {

class SceneChangeTracker {
public:
    // Installs the lightweight Flecs observers on `world` and binds the
    // publish target to `hub` (the per-session hub). Both references must
    // outlive the tracker (WorldSession::Impl declares the tracker AFTER the
    // ecs_runtime and hub so it destructs first — S I.13 close order).
    SceneChangeTracker(flecs::world& world, evt::Hub& hub);
    ~SceneChangeTracker();

    SceneChangeTracker(const SceneChangeTracker&) = delete;
    SceneChangeTracker& operator=(const SceneChangeTracker&) = delete;

    // Call at END of the app-thread ECS tick. Snapshots every dirty live
    // entity once and publishes the canonical batches (removals first, then
    // upserts; ids ascending; consecutive sequence values). No-op when
    // nothing is dirty (emits nothing, advances no sequence). Returns the
    // number of batches published (0, 1, or 2) — for tests/introspection.
    int flush();

    // Atomic (rows, sequence) recovery read (S I.14). `out_rows` is every
    // live scene entity's current record, ids ascending; `out_sequence` is
    // the last published batch's sequence (0 before any batch). Applying the
    // emitted batches (in sequence order) to a prior snapshot reconstructs
    // this — the recovery invariant. App-thread affinity.
    void scene_snapshot(std::vector<SceneRecord>& out_rows, uint64_t& out_sequence) const;

    // The last published batch sequence (0 before any publish). Same value
    // scene_snapshot() reports; exposed for tests/introspection.
    uint64_t sequence() const;

private:
    // Shared dirty state — captured by shared_ptr into every observer lambda
    // so it outlives the tracker AND the world (see class comment).
    struct DirtyState {
        mutable std::mutex mu;
        // sceneId -> the Flecs entity handle at mark time. Snapshotted at
        // flush; a handle that is no longer alive is skipped (it was removed).
        std::unordered_map<uint64_t, flecs::entity_t> dirty;
        std::unordered_set<uint64_t> removed;  // sceneId removed since last flush
    };

    static SceneRecord snapshot_entity(flecs::entity e);

    flecs::world& world_;
    evt::Hub& hub_;
    std::shared_ptr<DirtyState> state_;
    std::vector<flecs::entity> observers_;  // destructed in ~SceneChangeTracker
    uint64_t sequence_ = 0;                  // last PUBLISHED batch sequence
};

} // namespace matter::scene
