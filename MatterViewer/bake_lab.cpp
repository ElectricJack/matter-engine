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
                            const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                            const std::string& shared_lib_root) {
    // main.cpp calls workbench().begin_frame() unconditionally each frame
    // (even while this window is hidden) so wants_viewport() never sticks on
    // a stale true if the Bake Lab window is closed mid-isolation.
    if (ImGui::BeginTabBar("##bake_lab_tabs")) {
        if (ImGui::BeginTabItem("Assets")) {
            asset_browser_.draw(worlds, stats, shared_lib_root,
                                pending_workbench_module, pending_workbench_project,
                                focus_workbench_tab_);
            ImGui::EndTabItem();
        }
        // W1->W2 handoff: the Assets tab's "Open in Workbench" action records
        // pending_workbench_module/project and sets focus_workbench_tab_. Open
        // the part in the isolation session here (unconditional so it fires
        // even if the tab body is skipped), then clear the pending state. The
        // focus flag forces the Workbench tab selected this same frame via
        // ImGuiTabItemFlags_SetSelected.
        if (!pending_workbench_module.empty()) {
            workbench_.open_part(pending_workbench_project, pending_workbench_module);
            pending_workbench_module.clear();
            pending_workbench_project.clear();
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
