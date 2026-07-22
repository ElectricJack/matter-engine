#ifndef VIEWER_BAKE_LAB_H
#define VIEWER_BAKE_LAB_H

#include <string>
#include <vector>

#include "bake_lab_timeline.h"
#include "asset_browser.h"
#include "part_workbench.h"

namespace matter { class WorldSession; }

namespace viewer {

struct WorldEntry;
struct ViewerStats;

// Bake Lab window (part-workbench.md, superseding bake-lab.md SS-II.5): one
// dockable "Bake Lab" window with a tab bar - Assets, Workbench, Timeline.
// W1 adds the Assets tab (AssetBrowser); W2 fills the Workbench tab with the
// isolation scene + bake button + Params & Variations panel (PartWorkbench).
// The old "Variants" tab is cut per part-workbench.md I.6 (variant table
// cancelled). "Settle" stays a parked placeholder (part-workbench.md I.6:
// settle-lab UI 5.5-5.7 parked). Timeline is unchanged from task 2.2.
class BakeLab {
public:
    // Per-frame wall budget handed to tick_frame by the main loop. Job polling
    // and steppable-phase advancement (SS-II.5 transport) will spend against it
    // in later tasks; today it is accepted and ignored.
    static constexpr float kDefaultTickBudgetMs = 5.0f;

    // Window visibility. The viewer's other panels are drawn unconditionally,
    // so the Lab defaults to visible; the window's close button clears it.
    bool visible = true;

    // Set by the Assets tab's "Open in Workbench" action (AssetBrowser::draw):
    // pending_workbench_module names the part and pending_workbench_project is
    // the real project dir the part lives in (Project.path == WorldEntry::
    // project_dir). The Workbench tab (W2) consumes both — calling
    // PartWorkbench::open_part(project, module) — and clears them once the part
    // is opened in the isolation scene. focus_workbench_tab_ (private) forces
    // the Workbench tab selected the same frame the action fires.
    std::string pending_workbench_module;
    std::string pending_workbench_project;

    // Draws the tab bar + tab bodies. Caller (Ui::draw_bake_lab_panel) owns
    // the ImGui::Begin/End pair, mirroring the Console panel pattern.
    // `session` is the active world session (may be null before a world is
    // open); the Timeline tab uses it for last_bake_trace(). `worlds`/`stats`
    // are threaded to the Assets tab so its "Load" button can reuse the exact
    // world-switch path Ui::draw_worlds_panel uses (stats.world_switch_requested)
    // and to the Workbench tab's part picker (part-workbench.md W2).
    // `shared_lib_root` is the engine-wide shared-lib dir (see asset_browser.h)
    // so the Assets tab's ScriptHost matches the viewer session's hashing.
    void draw_contents(matter::WorldSession* session,
                       const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                       const std::string& shared_lib_root);

    // Per-frame hook, called once per frame from main.cpp's loop next to
    // session tick/pump. Intentionally a no-op still: later tasks poll
    // BakeJob worker threads and advance the active tab's SteppablePhase here,
    // stopping when wall_budget_ms is spent.
    void tick_frame(float wall_budget_ms);

    // W2: the isolation session lives here so main.cpp can tick/pump it every
    // frame and decide which session's render() to call this frame (see
    // part_workbench.h's architecture note on modal isolation).
    PartWorkbench& workbench() { return workbench_; }

private:
    BakeLabTimeline timeline_;
    AssetBrowser asset_browser_;
    PartWorkbench workbench_;
    bool focus_workbench_tab_ = false;
};

} // namespace viewer

#endif // VIEWER_BAKE_LAB_H
