#include "bake_lab.h"

#include "imgui.h"

namespace viewer {

namespace {

// One placeholder tab body. Real content arrives in the task noted per tab.
void draw_placeholder_tab(const char* name, const char* coming) {
    if (ImGui::BeginTabItem(name)) {
        ImGui::TextDisabled("%s", coming);
        ImGui::EndTabItem();
    }
}

} // namespace

void BakeLab::draw_contents() {
    if (ImGui::BeginTabBar("##bake_lab_tabs")) {
        draw_placeholder_tab("Timeline", "Coming in task 2.2 (flamegraph)");
        draw_placeholder_tab("Part Lab", "Coming in tasks 3.x (part-scope bake lab)");
        draw_placeholder_tab("Settle", "Coming in task 5.5 (settle lab)");
        draw_placeholder_tab("Variants", "Coming in tasks 4.x (variant table)");
        ImGui::EndTabBar();
    }
}

void BakeLab::tick_frame(float wall_budget_ms) {
    // Task 2.1: nothing to advance yet. Later tasks poll BakeJob threads and
    // step the active SteppablePhase under this wall budget (bake-lab.md §II.5).
    (void)wall_budget_ms;
}

} // namespace viewer
