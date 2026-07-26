#include "bake_lab.h"

#include "ui.h"  // WorldEntry, ViewerStats — see bake_lab.h's include-cycle note.

#include "imgui.h"

namespace viewer {

namespace {

// One placeholder tab body. Real content arrives in the task noted per tab.
void draw_placeholder_tab(const char* name, const char* coming, ImGuiTabItemFlags flags = 0) {
    if (ImGui::BeginTabItem(name, nullptr, flags)) {
        ImGui::TextDisabled("%s", coming);
        ImGui::EndTabItem();
    }
}

} // namespace

void BakeLab::draw_contents(matter::evt::Hub* app_hub, matter::WorldSession* session,
                            const std::vector<WorldEntry>& worlds,
                            AnimationDebugOverlayOptions& overlay) {
    // main.cpp calls workbench().begin_frame() unconditionally each frame
    // (even while this window is hidden) so wants_viewport() never sticks on
    // a stale true if the Bake Lab window is closed mid-isolation.
    if (ImGui::BeginTabBar("##bake_lab_tabs")) {
        // W2 (part-workbench.md): isolation scene + bake button + Params &
        // Variations panel. Body lives entirely in part_workbench.{h,cpp}.
        // tab_focus_pending_ (set by the lab.focus_tab command handler, e.g.
        // from the Asset Browser's "Open in Workbench") forces this tab
        // selected via ImGuiTabItemFlags_SetSelected.
        if (ImGui::BeginTabItem("Workbench", nullptr,
                                tab_focus_pending_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            workbench_.draw(worlds);
            ImGui::EndTabItem();
        }
        tab_focus_pending_ = false;
        if (ImGui::BeginTabItem("Timeline")) {
            timeline_.draw(session);
            ImGui::EndTabItem();
        }
        // E4c: read-only Events inspector over the app hub + production session
        // hub. The session hub is re-fetched inside EventInspector::draw every
        // frame (session->events()), never cached across a world switch.
        if (ImGui::BeginTabItem("Events")) {
            event_inspector_.draw(app_hub, session);
            ImGui::EndTabItem();
        }
        // D2 (procedural-animation): observational animation tabs over the
        // production session's debug snapshots. Refreshed here, inside the tab,
        // so a closed tab costs nothing.
        if (ImGui::BeginTabItem("Animation")) {
            std::vector<matter::AnimationDebugInstanceSnapshot> snapshots;
            const bool query_ok =
                session ? session->animation_debug_snapshots(snapshots) : true;
            animation_model_.update(snapshots, query_ok);
            draw_animation_panel(animation_model_, overlay);
            ImGui::EndTabItem();
        }
        draw_placeholder_tab("Settle", "Parked (part-workbench.md I.6 / task 5.5)");
        ImGui::EndTabBar();
    }
}

void BakeLab::open_workbench_part(const std::string& project, const std::string& module) {
    // workbench.open_part command handler (lab shell): open the part in the
    // isolation session and raise the Bake Lab window. Tab selection is the
    // separate lab.focus_tab command's job.
    workbench_.open_part(project, module);
    visible = true;
    window_raise_pending_ = true;
}

void BakeLab::focus_workbench_tab() {
    // lab.focus_tab{Workbench} command handler (lab shell): select the
    // Workbench tab on the next draw and raise the window.
    tab_focus_pending_ = true;
    window_raise_pending_ = true;
    visible = true;
}

bool BakeLab::take_window_raise() {
    const bool raise = window_raise_pending_;
    window_raise_pending_ = false;
    return raise;
}

void BakeLab::tick_frame(float wall_budget_ms) {
    // Task 2.2: Timeline is pull-based (Refresh button); the Assets tab
    // (W1) is pull-based too (Refresh button + per-frame mtime probe inside
    // AssetBrowser::draw). Nothing to advance here yet. Later tasks poll
    // BakeJob threads and step the active SteppablePhase under this wall
    // budget (bake-lab.md §II.5 / part-workbench.md W2-W3).
    (void)wall_budget_ms;
}

} // namespace viewer
