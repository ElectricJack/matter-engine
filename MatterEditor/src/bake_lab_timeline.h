#ifndef VIEWER_BAKE_LAB_TIMELINE_H
#define VIEWER_BAKE_LAB_TIMELINE_H

// MatterEditor/src/bake_lab_timeline.h
//
// The Bake Lab's Timeline tab: a flamegraph over one captured bake trace.
//
// Data comes from bake_trace (MatterEngine3/src/bake_trace.h). The engine's
// Collector owns the live tree; WorldSession::last_bake_trace() deep-copies a
// snapshot of it, and this panel owns that copy outright. Nothing here reads
// live engine memory, so no lock is held while drawing and a bake may run while
// the panel is on screen -- the picture is just a snapshot, not a live view.
//
// Pull-based, deliberately: the trace is only re-fetched when the user presses
// Refresh (or switches source). BakeLab::tick_frame does nothing for this tab.
//
// Times throughout are milliseconds on the collector's clock, relative to the
// start of the bake run -- absolute values are meaningless across sources, only
// differences matter. A span whose end_ms is bake_trace::kOpenEndMs was still
// open when the snapshot was taken; those are drawn dimmed and hatched, and
// their duration is reported "so far", measured to the end of the trace.
//
// Structure of a draw: the tree is flattened once per refresh into flat_ (a
// pre-order list with parent links) and every frame iterates that flat list.
// The Span pointers in flat_ point INTO sources_[active_source_].root, so any
// refresh invalidates them -- which is why a refresh also drops the pin.
//
// Render thread only.

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

    // Re-fetches the trace for the active source and rebuilds everything
    // derived from it. Destroys the previous span tree, so it also drops the
    // pin and refits the view. No-op when `session` is null.
    void refresh_active_source(matter::WorldSession* session);
    // Flattens the active source's span tree into flat_ (pre-order, with parent
    // indices) and recomputes the full time extent. Called once per refresh,
    // never per frame; every FlatSpan::span points into the current tree.
    void rebuild_flat();
    void draw_source_selector(matter::WorldSession* session);
    void draw_flamegraph_canvas();
    void draw_pinned_panel();
    void pin_flat_index(int flat_index);
    void fit_view();

    // Lazily seeded with the single production-session entry on the first
    // refresh or the first draw of the selector; never empty after that.
    std::vector<BakeLabTraceSource> sources_;
    int active_source_ = 0;   // index into sources_.

    // Pre-order flattening of the active source's tree, rebuilt per refresh.
    std::vector<FlatSpan> flat_;
    // Full extent of the flattened trace, in trace-relative milliseconds. Used
    // to fit and to clamp the view, and as the end time for still-open spans.
    // Defaults describe an empty 1 ms window so the divisions below stay safe.
    double full_begin_ms_ = 0.0;
    double full_end_ms_ = 1.0;

    // Visible time window in ms; kept within/around [full_begin_ms_, full_end_ms_].
    double view_begin_ms_ = 0.0;
    double view_end_ms_ = 1.0;
    bool view_initialized_ = false;  // false until the first fit_view().

    PinnedDetail pinned_;
};

} // namespace viewer

#endif // VIEWER_BAKE_LAB_TIMELINE_H
