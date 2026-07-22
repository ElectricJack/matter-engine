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

void BakeLab::draw_contents(matter::WorldSession* session,
                            const std::vector<WorldEntry>& worlds,
                            WorkbenchHandoff& handoff) {
    // main.cpp calls workbench().begin_frame() unconditionally each frame
    // (even while this window is hidden) so wants_viewport() never sticks on
    // a stale true if the Bake Lab window is closed mid-isolation.
    if (ImGui::BeginTabBar("##bake_lab_tabs")) {
        // Open-in-Workbench handoff (see WorkbenchHandoff in ui.h): the
        // standalone Asset Browser pane's "Open in Workbench" action records
        // handoff.pending_module/pending_project and sets
        // handoff.focus_requested. Open the part in the isolation session
        // here (unconditional so it fires even if the tab body is skipped
        // this frame), then clear the pending state. focus_workbench_tab_
        // forces the Workbench tab selected this same frame via
        // ImGuiTabItemFlags_SetSelected; Ui::draw_bake_lab_panel separately
        // uses handoff.focus_requested to raise the whole window.
        if (!handoff.pending_module.empty()) {
            workbench_.open_part(handoff.pending_project, handoff.pending_module);
            handoff.pending_module.clear();
            handoff.pending_project.clear();
            if (handoff.focus_requested) focus_workbench_tab_ = true;
            handoff.focus_requested = false;
        }
        // W2 (part-workbench.md): isolation scene + bake button + Params &
        // Variations panel. Body lives entirely in part_workbench.{h,cpp}.
        if (ImGui::BeginTabItem("Workbench", nullptr,
                                focus_workbench_tab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            workbench_.draw(worlds);
            ImGui::EndTabItem();
        }
        focus_workbench_tab_ = false;
        if (ImGui::BeginTabItem("Timeline")) {
            timeline_.draw(session);
            ImGui::EndTabItem();
        }
        draw_placeholder_tab("Settle", "Parked (part-workbench.md I.6 / task 5.5)");
        ImGui::EndTabBar();
    }
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
