#ifndef VIEWER_SCENE_MODEL_ADAPTER_H
#define VIEWER_SCENE_MODEL_ADAPTER_H

// scene_model_adapter.{h,cpp} — the SessionBinding-owned scene-graph adapter
// (event-system.md S I.14, E5c). This is the concrete bridge the SessionBinding
// (session_binding.*) (re)builds through its injected BridgeBuildFn on bind /
// world switch, and quiesces (drops the subscriptions) before the old session
// hub dies. The EditorModel is app-scoped and survives world switches; only this
// adapter is rebound against the new session.
//
// On build() (bind / rebind) it primes the EditorModel with an atomic
// (rows, sequence) snapshot from the session's SceneChangeTracker, then
// subscribes IMMEDIATE to the two canonical scene-delta events. Because tracker
// flush and model update are both app-thread-affine, deltas are never queued or
// coalesced here — the property scheduler coalesces the resulting UI
// notifications instead. The recovery invariant: accept only
// `sequence == last_sequence + 1`; on a gap/duplicate, do ONE full re-snapshot
// (logged/trace-flagged). Remove-of-unknown-id inside a correctly-sequenced
// batch is tolerated by EditorModel::apply_remove (not a recovery trigger).

#include <cstdint>
#include <memory>
#include <vector>

#include "matter/event/subscription.h"

namespace matter {
class WorldSession;
namespace evt { class Hub; }
}  // namespace matter

namespace viewer {

class EditorModel;

// Owns nothing. It holds references to the app-scoped EditorModel and to
// main.cpp's session slot, so it must not outlive either. Constructed once and
// reused across world switches: `build()` may be called repeatedly, each time
// against a different session hub, and each call re-primes `last_sequence_`
// from the snapshot it takes.
//
// The subscriptions it creates are NOT stored here — they are pushed into the
// caller's vector and destroyed by SessionBinding::quiesce_bridge before the
// old hub dies, which is what guarantees no callback can fire into a torn-down
// session. App thread only; the reference members make it non-assignable.
class SceneModelAdapter {
public:
    // `session_slot` is main.cpp's owning session pointer; it is re-pointed in
    // place on a world switch BEFORE SessionBinding calls build() again, so the
    // adapter always snapshots/subscribes against the live session.
    SceneModelAdapter(EditorModel& model,
                      std::unique_ptr<matter::WorldSession>& session_slot);

    // Invoked by SessionBinding::rebuild_bridge (via the BridgeBuildFn):
    // snapshot-prime the model, then push the two immediate subscriptions into
    // `out` (their RAII handles are owned by SessionBinding and dropped in
    // quiesce_bridge()).
    void build(matter::evt::Hub& session_hub,
               std::vector<matter::evt::Subscription>& out);

private:
    void full_snapshot();  // atomic (rows, sequence) prime / recovery re-read

    EditorModel& model_;
    std::unique_ptr<matter::WorldSession>& session_;
    // Last scene-delta sequence applied to the model. Reset to 0 by a snapshot
    // taken with no session bound. A delivered event whose sequence is not
    // exactly this + 1 is discarded and triggers one full re-snapshot.
    uint64_t last_sequence_ = 0;
};

}  // namespace viewer

#endif  // VIEWER_SCENE_MODEL_ADAPTER_H
