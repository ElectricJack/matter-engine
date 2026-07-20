#include "toolbar_panel.h"

#include "imgui.h"

namespace viewer {

namespace {

const char* mode_label(matter::scene::SimulationMode mode) {
    switch (mode) {
        case matter::scene::SimulationMode::Play:  return "PLAYING";
        case matter::scene::SimulationMode::Pause: return "PAUSED";
        case matter::scene::SimulationMode::Edit:
        default:                                   return "EDIT";
    }
}

} // namespace

ToolbarActions draw_toolbar_contents(ToolbarState& state,
                                     matter::scene::SimulationMode mode) {
    using matter::scene::SimulationMode;
    ToolbarActions actions;

    const bool is_edit = mode == SimulationMode::Edit;
    const bool is_play = mode == SimulationMode::Play;
    const bool is_pause = mode == SimulationMode::Pause;

    // Left-aligned transport buttons.
    {
        const bool play_enabled = is_edit || is_pause;
        if (!play_enabled) ImGui::BeginDisabled(true);
        if (ImGui::Button("Play")) actions.play_clicked = true;
        if (!play_enabled) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        const bool pause_enabled = is_play;
        if (!pause_enabled) ImGui::BeginDisabled(true);
        if (ImGui::Button("Pause")) actions.pause_clicked = true;
        if (!pause_enabled) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        const bool step_enabled = is_pause;
        if (!step_enabled) ImGui::BeginDisabled(true);
        if (ImGui::Button("Step")) actions.step_clicked = true;
        if (!step_enabled) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        const bool stop_enabled = is_play || is_pause;
        if (!stop_enabled) ImGui::BeginDisabled(true);
        if (ImGui::Button("Stop")) actions.stop_clicked = true;
        if (!stop_enabled) ImGui::EndDisabled();
    }

    // Center: mode label.
    ImGui::SameLine();
    const char* label = mode_label(mode);
    const float avail_width = ImGui::GetContentRegionAvail().x;
    const float label_width = ImGui::CalcTextSize(label).x;
    const float center_offset = (avail_width - label_width) * 0.5f;
    if (center_offset > 0.0f) {
        ImGui::SameLine(0.0f, center_offset);
    } else {
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(label);

    // Right: FPS placeholder (mode label for now).
    ImGui::SameLine(ImGui::GetWindowWidth() - 100.0f);
    ImGui::TextUnformatted(label);

    // Space toggles Play/Pause, except while text input is active.
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (is_play) {
            actions.pause_clicked = true;
        } else if (is_edit || is_pause) {
            actions.play_clicked = true;
        }
    }
    (void)state;
    return actions;
}

void draw_viewport_border_tint(matter::scene::SimulationMode mode) {
    using matter::scene::SimulationMode;
    if (mode == SimulationMode::Edit) return;

    ImVec4 color;
    if (mode == SimulationMode::Play) {
        color = ImVec4(0.0f / 255.0f, 200.0f / 255.0f, 0.0f / 255.0f, 180.0f / 255.0f);
    } else {
        color = ImVec4(200.0f / 255.0f, 160.0f / 255.0f, 0.0f / 255.0f, 180.0f / 255.0f);
    }
    const ImU32 col32 = ImGui::ColorConvertFloat4ToU32(color);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float thickness = 3.0f;
    const float inset = thickness * 0.5f;
    ImGui::GetForegroundDrawList()->AddRect(
        ImVec2(inset, inset), ImVec2(display.x - inset, display.y - inset),
        col32, 0.0f, 0, thickness);
}

} // namespace viewer
