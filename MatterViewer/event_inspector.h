#ifndef VIEWER_EVENT_INSPECTOR_H
#define VIEWER_EVENT_INSPECTOR_H

// event_inspector.{h,cpp} — the read-only Events inspector (event-system.md
// S I.8 Observability, S I.13 multi-hub), drawn in the Bake Lab window's
// "Events" tab (E4c). Three sub-views over one selected hub:
//
//   Registry  — event types x their ordered subscribers (Hub::registry_snapshot),
//               columns name/phase/priority/lane/delivery-count/subscribe-site,
//               sorted by the (phase,priority,name) ordering key, filterable by
//               name namespace; per-lane drop/reject counters (Hub::lane_counters)
//               highlighted red when nonzero.
//   Trace     — a live-tailing list of recorded emits (Hub::trace_snapshot),
//               event name / emitting thread / timestamp / expandable per-
//               subscriber delivery records, indented by handler-emitted-event
//               nesting depth; pause + name-filter + click-to-pin. Honors the
//               per-hub trace on/off toggle (Hub::trace_enabled).
//   Timeline  — emissions + handler durations rendered as a compact fit-to-width
//               flamegraph (same span/color idiom as bake_lab_timeline; see the
//               reuse note in event_inspector.cpp).
//
// A hub selector (mirroring bake_lab_timeline's source selector) chooses which
// hub all three views reflect: the editor-lifetime App hub and the production
// Session hub. The Session hub is fetched fresh from session->events() every
// draw and NEVER cached across frames — it dies with the session on a world
// switch (S I.13).
//
// CONSTRAINT (E4c): strictly read-only. The inspector never emits events,
// registers commands, or mutates hub state — the single exception is toggling
// the per-hub trace on/off, a legitimate diagnostic control.

#include <cstdint>
#include <string>
#include <vector>

#include "matter/event/event_hub.h"  // TraceRecord / RegistrySnapshotEntry / LaneCounters

namespace matter {
class WorldSession;
namespace evt { class Hub; }
}  // namespace matter

namespace viewer {

class EventInspector {
public:
    // Draws the hub selector + the active sub-view. `app_hub` is the stable
    // editor-lifetime app hub (may be null only in degenerate setups).
    // `session` is the active production session; its hub is reached via
    // session->events() and re-fetched here every frame (safe across a world
    // switch — no hub pointer is retained between frames).
    void draw(matter::evt::Hub* app_hub, matter::WorldSession* session);

private:
    // A hub the selector can point at. This vector is rebuilt every frame from
    // the live app_hub + session->events(); no Hub* is ever stored on the
    // EventInspector itself, so a world switch that destroys the session hub
    // can never leave a dangling pointer here (mirrors bake_lab_timeline's
    // source-vector pattern, adapted to per-frame hub liveness).
    struct HubEntry {
        std::string name;
        matter::evt::Hub* hub = nullptr;
    };

    void draw_hub_selector(const std::vector<HubEntry>& hubs);
    void draw_registry_view(matter::evt::Hub& hub);
    void draw_trace_view(matter::evt::Hub& hub);
    void draw_timeline_view(matter::evt::Hub& hub);

    // --- selection -------------------------------------------------------
    int active_hub_ = 0;

    // --- Registry view state ---------------------------------------------
    char registry_filter_[64] = {0};

    // --- Trace view state ------------------------------------------------
    bool trace_paused_ = false;
    char trace_filter_[64] = {0};
    // While paused, the view shows this frozen copy instead of re-snapshotting.
    std::vector<matter::evt::TraceRecord> frozen_trace_;
    // Click-to-pin: a copied-out record whose details survive snapshot churn.
    bool trace_pin_active_ = false;
    matter::evt::TraceRecord trace_pin_;
};

}  // namespace viewer

#endif  // VIEWER_EVENT_INSPECTOR_H
