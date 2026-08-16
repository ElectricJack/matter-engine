#include "scene_model_adapter.h"

#include <cstdio>

#include "editor_model.h"
#include "matter/event/event_hub.h"
#include "matter/log.h"
#include "matter/scene/scene_events.h"
#include "matter/world_session.h"
#include "scene/scene_change_tracker.h"  // scene_snapshot() (world_session.h fwd-decls it)

namespace viewer {

SceneModelAdapter::SceneModelAdapter(EditorModel& model,
                                     std::unique_ptr<matter::WorldSession>& session_slot)
    : model_(model), session_(session_slot) {}

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
                MATTER_LOGW("scene-adapter",
                            "upsert sequence gap (have %llu, got %llu) -> "
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
                MATTER_LOGW("scene-adapter",
                            "remove sequence gap (have %llu, got %llu) -> "
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
