#ifndef VIEWER_BAKE_LAB_H
#define VIEWER_BAKE_LAB_H

#include <string>
#include <vector>

#include "bake_lab_timeline.h"
#include "event_inspector.h"
#include "part_workbench.h"
#include "animation_panel.h"

namespace matter {
class WorldSession;
namespace evt { class Hub; }
}  // namespace matter

namespace viewer {

struct WorldEntry;

// Bake Lab window (part-workbench.md, superseding bake-lab.md SS-II.5): one
// dockable "Bake Lab" window with a tab bar - Workbench, Timeline. The former
// Assets tab (AssetBrowser) was promoted to its own standalone top-level
// pane (Ui::draw_asset_browser_panel) so it's usable outside Bake Lab too;
// its "Open in Workbench" action now reaches this window through the
// workbench.open_part / lab.focus_tab commands (event-system.md S I.11, E4b) —
// see open_workbench_part / focus_workbench_tab below. W2 fills the Workbench
// tab with the isolation scene +
// bake button + Params & Variations panel (PartWorkbench). The old "Variants"
// tab is cut per part-workbench.md I.6 (variant table cancelled). "Settle"
// stays a parked placeholder (part-workbench.md I.6: settle-lab UI 5.5-5.7
// parked). Timeline is unchanged from task 2.2.
class BakeLab {
public:
    // Per-frame wall budget handed to tick_frame by the main loop. Job polling
    // and steppable-phase advancement (SS-II.5 transport) will spend against it
    // in later tasks; today it is accepted and ignored.
    static constexpr float kDefaultTickBudgetMs = 5.0f;

    // Window visibility. The viewer's other panels are drawn unconditionally,
    // so the Lab defaults to visible; the window's close button clears it.
    bool visible = true;

    // Draws the tab bar + tab bodies. Caller (Ui::draw_bake_lab_panel) owns
    // the ImGui::Begin/End pair, mirroring the Console panel pattern.
    // `session` is the active world session (may be null before a world is
    // open); the Timeline tab uses it for last_bake_trace(). `worlds` is
    // threaded to the Workbench tab's part picker (part-workbench.md W2). A
    // pending tab-focus (set by the lab.focus_tab command handler) forces the
    // Workbench tab selected this frame via ImGuiTabItemFlags_SetSelected.
    // `app_hub` is the editor-lifetime app hub (event-system.md S I.13); it and
    // `session` are handed to the read-only Events inspector tab (E4c), which
    // introspects the app hub + the production session hub (session->events()).
    // `overlay` is the viewer's animation overlay option set (ViewerStats), so
    // the Animation tab's Render section toggles exactly what the viewport
    // draws rather than keeping a second copy that can drift.
    void draw_contents(matter::evt::Hub* app_hub, matter::WorldSession* session,
                       const std::vector<WorldEntry>& worlds,
                       AnimationDebugOverlayOptions& overlay);

    // --- command handlers (event-system.md S I.11, E4b) --------------------
    // The lab shell's poll-site logic, invoked by the workbench.open_part /
    // lab.focus_tab command handlers (registered in main.cpp against the app
    // registry). open_workbench_part opens the part in the isolation session
    // and raises the Bake Lab window; focus_workbench_tab selects the Workbench
    // tab (next draw) and raises the window. take_window_raise is polled+cleared
    // by Ui::draw_bake_lab_panel to issue exactly one ImGui::SetNextWindowFocus.
    void open_workbench_part(const std::string& project, const std::string& module);
    void focus_workbench_tab();
    bool take_window_raise();

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
    EventInspector event_inspector_;
    PartWorkbench workbench_;
    // Refreshed only while the Animation tab is drawn: animation_debug_snapshots
    // is an observational copy, and there is no reason to pay for it every frame
    // on a tab nobody is looking at.
    AnimationPanelModel animation_model_;
    // Set by the command handlers above; consumed in draw_contents (tab focus)
    // and take_window_raise (window raise). Replaces the old focus_workbench_tab_
    // polled flag + the WorkbenchHandoff channel.
    bool tab_focus_pending_ = false;
    bool window_raise_pending_ = false;
};

} // namespace viewer

#endif // VIEWER_BAKE_LAB_H
