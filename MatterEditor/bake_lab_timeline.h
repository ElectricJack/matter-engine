#ifndef VIEWER_BAKE_LAB_TIMELINE_H
#define VIEWER_BAKE_LAB_TIMELINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "bake_trace.h"

namespace matter { class WorldSession; }

namespace viewer {

// One captured trace the Timeline can display. Task 2.2 only ever populates
// a single fixed entry ("Production session (last bake)"), refreshed from
// WorldSession::last_bake_trace(). The vector-of-named-snapshots shape is
// deliberate: later tasks (Lab job traces, bake-lab.md §II.2/§II.3) append
// more entries here without any change to the storage or selector UI.
struct BakeLabTraceSource {
    std::string name;
    bake_trace::Span root;   // kRootName; root.children.empty() => nothing captured yet
    bool captured = false;   // true once at least one Refresh has run
};

// Timeline tab (bake-lab.md §II.2): source selector + flamegraph rendered
// with ImGui draw-list primitives, zoom/pan, hover tooltip, click-to-pin.
// Diff mode (§II.2, task 2.3) is explicitly out of scope here.
class BakeLabTimeline {
public:
    // Draws the source selector, flamegraph canvas, and pinned-span detail
    // section. `session` may be null (no world open yet); the Refresh
    // button is disabled in that case.
    void draw(matter::WorldSession* session);

private:
    // A single span flattened for O(1) iteration during draw/hit-test,
    // built once per refresh (not per frame). `span` points into
    // sources_[active_source_].root's tree, valid until the next refresh
    // (which rebuilds flat_ and clears any pin referencing the old tree).
    struct FlatSpan {
        const bake_trace::Span* span = nullptr;
        int depth = 0;
        int parent_index = -1;   // index into flat_, -1 for the root
    };

    // Copied-out detail for the pinned span so it survives independent of
    // flat_/sources_ churn (e.g. if a future source is removed).
    struct PinnedDetail {
        bool active = false;
        std::vector<std::string> path;   // root -> ... -> span, inclusive
        double begin_ms = 0.0;
        double end_ms = 0.0;
        bool open = false;
        std::vector<bake_trace::Counter> counters;
    };

    void refresh_active_source(matter::WorldSession* session);
    void rebuild_flat();
    void draw_source_selector(matter::WorldSession* session);
    void draw_flamegraph_canvas();
    void draw_pinned_panel();
    void pin_flat_index(int flat_index);
    void fit_view();

    std::vector<BakeLabTraceSource> sources_;
    int active_source_ = 0;

    std::vector<FlatSpan> flat_;
    double full_begin_ms_ = 0.0;
    double full_end_ms_ = 1.0;

    // Visible time window in ms; kept within/around [full_begin_ms_, full_end_ms_].
    double view_begin_ms_ = 0.0;
    double view_end_ms_ = 1.0;
    bool view_initialized_ = false;

    PinnedDetail pinned_;
};

} // namespace viewer

#endif // VIEWER_BAKE_LAB_TIMELINE_H
