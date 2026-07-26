#ifndef VIEWER_LOD_INSPECTOR_H
#define VIEWER_LOD_INSPECTOR_H

// LOD Inspector (part-workbench.md SS-I.5, W4 "inspection tier"): a section
// of the Workbench tab (hosted by PartWorkbench) showing the loaded part's
// LOD ladder — rows are LOD levels (tri count + threshold from the baked
// LoadedPart), the child list groups the part's baked child-instance table
// by hash (module name + placement count, e.g. "Leaf x340" when the part
// graph can name it).
//
// Two view-only debug toggles drive matter::RenderOptions on the isolation
// session's NEXT render (part-workbench.md I.5 "Visibility (view-side
// only)... Never affects baking"):
//   - force_lod: view LOD level k up close regardless of camera distance.
//   - hide_child_instances: "show root only" (the W4 module-hiding
//     granularity achieved — see part_workbench.cpp/matter_engine.cpp
//     comments for why the full per-module mask is deferred).
//
// NO authoring here: no bake-inclusion checkboxes, no per-LOD params (W5).
// Reads data exclusively through matter::WorldSession's W4 query methods
// (part_lod_level_count/info, part_child_summary_count/info) — never
// fabricated, never touches PartStore directly (Lab code lives outside
// MatterEngine3's kernel boundary).

#include <cstdint>

#include "matter/world_session.h"

namespace viewer {

class LodInspector {
public:
    // Draws the grid + toggles for `part_hash` (the isolation session's
    // currently-loaded root part). Renders a disabled placeholder if
    // `session` is null, `part_hash` is 0, or the part has no loaded LOD
    // data yet (bake in flight / not yet published). Auto-resets the debug
    // overrides to defaults when `part_hash` differs from the previously
    // drawn one, so a forced level never silently leaks onto a different
    // part or a re-baked variation with a new resolved hash.
    void draw(matter::WorldSession* session, uint64_t part_hash);

    // Copies the current toggle state onto `opts` (force_lod,
    // hide_child_instances). Called by PartWorkbench once per frame, BEFORE
    // the isolation session's render(), so the toggles set in the most
    // recent draw() apply to its next render. Defaults (-1 / false) leave
    // `opts` untouched relative to its prior values for those two fields —
    // i.e. this always overwrites them with the inspector's current state,
    // which is exactly -1/false until the user interacts with the grid.
    void apply(matter::RenderOptions& opts) const;

    // Resets both toggles to their defaults (Auto level selection, all
    // children visible). Safe to call any time; also called internally by
    // draw() on a part-hash change. PartWorkbench additionally calls this
    // explicitly from open_part()/close() so a stale override never flashes
    // for even one frame while a new part's first draw() hasn't run yet.
    void reset();

private:
    int      force_lod_ = -1;              // -1 = Auto (camera-based selection)
    bool     hide_child_instances_ = false; // W4 "show root only" granularity
    uint64_t last_part_hash_ = 0;           // drives the auto-reset in draw()
};

} // namespace viewer

#endif // VIEWER_LOD_INSPECTOR_H
