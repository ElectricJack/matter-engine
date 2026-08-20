// ImGui rendering for the Console panel. The ConsoleLog ring-buffer logic
// lives in console_log.cpp (no ImGui dependency) so it can be unit tested
// headlessly; this file only implements draw_console_contents.
//
// Everything here runs on the ImGui (main) thread, inside the caller's
// Begin/End pair. Ui::draw_console_panel (ui.cpp) is the only caller; it also
// claims `console.filters` as this panel's Tunables home so the group is not
// duplicated in the Tunables window. Severity row colours are hard-coded here
// rather than taken from the ImGui style.
#include "console_panel.h"

#include <string>

#include "imgui.h"

namespace viewer {
namespace {

const char* severity_label(LogSeverity severity) {
    switch (severity) {
        case LogSeverity::Info:    return "INFO ";
        case LogSeverity::Warning: return "WARN ";
        case LogSeverity::Error:   return "ERROR";
    }
    return "?????";
}

} // namespace

// One frame of the Console panel's contents.
//
// Takes a fresh filtered snapshot of `log` on every call (ConsoleLog::filtered
// — O(retained entries), allocating, and it holds the log's mutex) and builds
// one std::string per visible row, so cost scales with what passes the filter
// rather than with what is on screen. Mutates `state` (the widgets write it
// directly; was_at_bottom is recomputed at the end from the scroll position)
// and can mutate `log` — the Clear button calls log.clear().
void draw_console_contents(ConsolePanelState& state, ConsoleLog& log) {
    // Filter / control row.
    ImGui::Checkbox("Info", &state.show_info);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &state.show_warning);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &state.show_error);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("Filter", state.text_filter, sizeof(state.text_filter));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        log.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &state.auto_scroll);

    ImGui::Separator();

    // Scrollable log region. Reserve one line at the bottom for a future
    // script input footer (Phase 6).
    const float footer_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0, -footer_h), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ConsoleLog::Snapshot snapshot = log.filtered(
        state.show_info, state.show_warning, state.show_error,
        state.text_filter);

    for (const LogEntry& entry : snapshot.entries) {
        // entry.timestamp is ELAPSED seconds since app start, not a wall
        // clock; this formats it as hh:mm:ss, which therefore wraps back to
        // 00:00:00 after 24 hours of uptime.
        const int total_seconds = static_cast<int>(entry.timestamp);
        const int hh = (total_seconds / 3600) % 24;
        const int mm = (total_seconds / 60) % 60;
        const int ss = total_seconds % 60;

        ImVec4 color;
        switch (entry.severity) {
            case LogSeverity::Info:    color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break;
            case LogSeverity::Warning: color = ImVec4(1.0f, 200.0f / 255.0f, 0.0f, 1.0f); break;
            case LogSeverity::Error:   color = ImVec4(1.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f); break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(
            (std::string("[") +
             (hh < 10 ? "0" : "") + std::to_string(hh) + ":" +
             (mm < 10 ? "0" : "") + std::to_string(mm) + ":" +
             (ss < 10 ? "0" : "") + std::to_string(ss) + "] " +
             severity_label(entry.severity) + "  " + entry.message)
                .c_str());
        ImGui::PopStyleColor();
    }

    if (state.auto_scroll && state.was_at_bottom) {
        ImGui::SetScrollHereY(1.0f);
    }
    state.was_at_bottom = (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f);

    ImGui::EndChild();

    // Reserved footer row (empty for Phase 6 script input). The scroll
    // region above already left footer_h of space for this row.
    ImGui::TextDisabled("(script input reserved for Phase 6)");
}

} // namespace viewer
