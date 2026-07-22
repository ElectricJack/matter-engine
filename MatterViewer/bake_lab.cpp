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
    if (ImGui::BeginTabBar("##bake_lab_tabs")) {
        if (ImGui::BeginTabItem("Assets")) {
            asset_browser_.draw(worlds, stats, shared_lib_root,
                                pending_workbench_module, focus_workbench_tab_);
            ImGui::EndTabItem();
        }
        // "Open in Workbench" (Assets tab) sets focus_workbench_tab_; consumed
        // here (one frame after it's set) via ImGuiTabItemFlags_SetSelected,
        // then cleared so it doesn't keep forcing the tab open.
        draw_placeholder_tab("Workbench",
                             "Coming in W2+ (part-workbench.md): isolation scene, "
                             "bake button, params & variations",
                             focus_workbench_tab_ ? ImGuiTabItemFlags_SetSelected : 0);
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
