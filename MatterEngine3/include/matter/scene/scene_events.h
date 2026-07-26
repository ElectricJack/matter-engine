// matter/scene/scene_events.h — the two canonical, typed scene-row delta
// events published by scene::SceneChangeTracker (E5b).
//
// Design authority: MatterEngine3/docs/event-system.md
//   S I.14 — "the scene graph (app view-model over a session bridge)":
//            the SceneChangeTracker snapshots each dirty live entity once
//            at end of the app-thread ECS tick and publishes ONE of these
//            two events per affected batch. Both are normalized cross-layer
//            model deltas carrying COPIED session->editor data (S I.14:
//            "Flecs remains the source observer mechanism, while the hub
//            carries copied session->editor data").
//   S II.1 E5 — the milestone that adds these.
//
// Sequencing/ordering contract (SceneChangeTracker::flush enforces it,
// S I.14): a single tick with both removals and upserts assigns CONSECUTIVE
// sequence values in a deterministic order — removals first, then upserts;
// ids sorted ascending inside each batch. `sequence` increments once per
// PUBLISHED batch, so applying the emitted batches in sequence order to a
// prior scene_snapshot() reconstructs a fresh snapshot (the recovery
// invariant, S I.14: "SessionBinding snapshots (rows, sequence) ... accepts
// only sequence == last_sequence + 1").
//
// Delivery: the SceneChangeTracker emits these on the per-session evt::Hub
// (S I.13). The E5c SessionBinding adapter subscribes IMMEDIATE (S I.14:
// "this adapter subscribes immediate and never queues or coalesces scene
// deltas") because tracker flush and model update are both app-thread-affine;
// sequence values therefore cannot be skipped by a queue policy. E5b only
// defines and publishes the events — no editor consumer yet.
#pragma once
#include <cstdint>
#include <vector>

#include "matter/event/event_name.h"
#include "matter/scene.h"

namespace matter::scene {

// scene.rows_upserted — the complete current SceneRecord for every affected
// id in this batch (entity add, ChildOf add/remove reparent, display-name
// change, or component add/set/remove). One row per affected id; the row is
// the full current record, never a partial patch (S I.14).
struct SceneRowsUpserted {
    MT_EVENT_NAME("scene.rows_upserted");
    uint64_t sequence = 0;              // increments once per published batch
    std::vector<SceneRecord> rows;      // ids ascending (flush invariant)
};

// scene.rows_removed — every id removed since the last flush, INCLUDING
// descendants removed by a cascade delete (S I.14: "includes descendants
// removed by cascade"). Flecs fires OnRemove per descendant when a parent is
// destroyed, so the tracker captures the whole subtree naturally.
struct SceneRowsRemoved {
    MT_EVENT_NAME("scene.rows_removed");
    uint64_t sequence = 0;
    std::vector<SceneEntityId> ids;     // ids ascending (flush invariant)
};

} // namespace matter::scene
