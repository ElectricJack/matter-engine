// MatterEditor/src/scene_model_adapter.cpp
//
// Implementation of the SessionBinding-owned scene-graph adapter. The
// contract, the sequencing invariant and the event-system references are in
// scene_model_adapter.h.
//
// Both subscriptions are registered `matter::evt::immediate`, so their
// callbacks run inline at publish rather than being queued or coalesced. That
// is only sound because the tracker flush and the model update are both
// app-thread-affine (see the header); nothing here takes a lock.
//
// The recovery path — a delivered sequence that is not exactly
// `last_sequence_ + 1` — discards the incoming delta entirely and re-primes
// from a fresh atomic snapshot, which resynchronises the rows and the sequence
// counter in one step. Upsert and remove implement the identical rule; they
// are two copies rather than one helper only because they carry different
// payloads.

#include "scene_model_adapter.h"

#include <cstdio>

#include "editor_model.h"
#include "matter/event/event_hub.h"
#include "matter/scene/scene_events.h"
#include "matter/world_session.h"
#include "scene/scene_change_tracker.h"  // scene_snapshot() (world_session.h fwd-decls it)

namespace viewer {

SceneModelAdapter::SceneModelAdapter(EditorModel& model,
                                     std::unique_ptr<matter::WorldSession>& session_slot)
    : model_(model), session_(session_slot) {}

// Prime (or re-prime) the model from the session's atomic (rows, sequence)
// pair. With no session bound this CLEARS the model and resets the sequence to
// 0, which is what lets a later bind start counting from the new session's
// first delta. Called on bind/rebind and as the gap-recovery path; it rebuilds
// the entire row set, so it is O(scene) and is not something to run per frame.
void SceneModelAdapter::full_snapshot() {
    if (!session_) {
        model_.apply_snapshot({});
        last_sequence_ = 0;
        return;
    }
    std::vector<matter::scene::SceneRecord> rows;
    uint64_t seq = 0;
    session_->scene_change_tracker().scene_snapshot(rows, seq);
    model_.apply_snapshot(rows);
    last_sequence_ = seq;
}

void SceneModelAdapter::build(matter::evt::Hub& session_hub,
                              std::vector<matter::evt::Subscription>& out) {
    // Bind/rebind: prime the app-scoped model from the new session's atomic
    // (rows, sequence) snapshot before any delta can be delivered.
    full_snapshot();

    // Immediate subscriptions (S I.14): tracker flush + model update are both
    // app-thread-affine, so scene deltas are neither queued nor coalesced here.
    out.push_back(session_hub.must_subscribe<matter::scene::SceneRowsUpserted>(
        "editor.scene_rows_upserted", matter::evt::immediate,
        [this](const matter::scene::SceneRowsUpserted& ev) {
            if (ev.sequence != last_sequence_ + 1) {
                std::fprintf(stderr,
                             "[scene-adapter] upsert sequence gap (have %llu, got %llu) -> "
                             "full resnapshot\n",
                             static_cast<unsigned long long>(last_sequence_),
                             static_cast<unsigned long long>(ev.sequence));
                full_snapshot();
                return;
            }
            model_.apply_upsert(ev.rows);
            last_sequence_ = ev.sequence;
        }));

    out.push_back(session_hub.must_subscribe<matter::scene::SceneRowsRemoved>(
        "editor.scene_rows_removed", matter::evt::immediate,
        [this](const matter::scene::SceneRowsRemoved& ev) {
            if (ev.sequence != last_sequence_ + 1) {
                std::fprintf(stderr,
                             "[scene-adapter] remove sequence gap (have %llu, got %llu) -> "
                             "full resnapshot\n",
                             static_cast<unsigned long long>(last_sequence_),
                             static_cast<unsigned long long>(ev.sequence));
                full_snapshot();
                return;
            }
            model_.apply_remove(ev.ids);
            last_sequence_ = ev.sequence;
        }));
}

}  // namespace viewer
