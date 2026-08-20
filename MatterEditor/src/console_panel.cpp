// ImGui rendering for the Console panel. The ConsoleLog ring-buffer logic
// lives in console_log.cpp (no ImGui dependency) so it can be unit tested
// headlessly; this file only implements draw_console_contents.
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

ImVec4 severity_color(LogSeverity severity) {
    switch (severity) {
        case LogSeverity::Info:    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        case LogSeverity::Warning: return ImVec4(1.0f, 200.0f / 255.0f, 0.0f, 1.0f);
        case LogSeverity::Error:   return ImVec4(1.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

// The single formatter shared by the on-screen rows and the clipboard export,
// so copied text is byte-for-byte what the panel displays.
std::string format_line(const LogEntry& entry) {
    const int total_seconds = static_cast<int>(entry.timestamp);
    const int hh = (total_seconds / 3600) % 24;
    const int mm = (total_seconds / 60) % 60;
    const int ss = total_seconds % 60;
    return std::string("[") +
           (hh < 10 ? "0" : "") + std::to_string(hh) + ":" +
           (mm < 10 ? "0" : "") + std::to_string(mm) + ":" +
           (ss < 10 ? "0" : "") + std::to_string(ss) + "] " +
           severity_label(entry.severity) + "  " + entry.message;
}

// Join the given snapshot indices (already validated) into one clipboard blob.
std::string join_lines(const ConsoleLog::Snapshot& snapshot,
                       const std::set<int>& indices) {
    std::string out;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(snapshot.entries.size())) continue;
        out += format_line(snapshot.entries[idx]);
        out += '\n';
    }
    return out;
}

std::string join_all(const ConsoleLog::Snapshot& snapshot) {
    std::string out;
    for (const LogEntry& entry : snapshot.entries) {
        out += format_line(entry);
        out += '\n';
    }
    return out;
}

} // namespace

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
        state.selection.clear();
        state.sel_anchor = -1;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &state.auto_scroll);

    ImGui::Separator();

    // Snapshot the filtered log once for this frame. Everything below -- render,
    // selection bounds, and clipboard export -- indexes into this same vector.
    ConsoleLog::Snapshot snapshot = log.filtered(
        state.show_info, state.show_warning, state.show_error,
        state.text_filter);
    const int line_count = static_cast<int>(snapshot.entries.size());

    // Drop any selected indices that no longer exist (older lines evicted by
    // the ring buffer, or hidden by a filter change).
    for (auto it = state.selection.begin(); it != state.selection.end();) {
        if (*it >= line_count) it = state.selection.erase(it);
        else ++it;
    }

    // Copy button in the control row: copies the selection, or every visible
    // line when nothing is selected.
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string text = state.selection.empty()
                               ? join_all(snapshot)
                               : join_lines(snapshot, state.selection);
        if (!text.empty()) ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Copy selected lines (or all lines if none selected).\n"
            "Click a line to select; Shift-click for a range; Ctrl-click to toggle.");
    }

    // Scrollable log region. Reserve one line at the bottom for the status
    // footer below.
    const float footer_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0, -footer_h), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Right-click anywhere in the scroll region for the context menu.
    if (ImGui::BeginPopupContextWindow("ConsoleContext")) {
        if (ImGui::MenuItem("Copy", nullptr, false, !state.selection.empty())) {
            std::string text = join_lines(snapshot, state.selection);
            if (!text.empty()) ImGui::SetClipboardText(text.c_str());
        }
        if (ImGui::MenuItem("Copy all", nullptr, false, line_count > 0)) {
            std::string text = join_all(snapshot);
            if (!text.empty()) ImGui::SetClipboardText(text.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select all", nullptr, false, line_count > 0)) {
            state.selection.clear();
            for (int i = 0; i < line_count; ++i) state.selection.insert(i);
        }
        if (ImGui::MenuItem("Clear selection", nullptr, false,
                            !state.selection.empty())) {
            state.selection.clear();
            state.sel_anchor = -1;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear console", nullptr, false, line_count > 0)) {
            log.clear();
            state.selection.clear();
            state.sel_anchor = -1;
        }
        ImGui::EndPopup();
    }

    // Render rows as selectables so lines can be picked and copied. A clipper
    // keeps this O(visible) even with a full 4096-line ring.
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    const bool shift = ImGui::GetIO().KeyShift;
    ImGuiListClipper clipper;
    clipper.Begin(line_count);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogEntry& entry = snapshot.entries[i];
            const bool is_selected = state.selection.count(i) != 0;

            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Text, severity_color(entry.severity));
            // ImGuiSelectableFlags_AllowDoubleClick left off: a single click is
            // the select gesture. Span the full row width so the whole line is
            // a click target regardless of message length.
            if (ImGui::Selectable(format_line(entry).c_str(), is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                if (shift && state.sel_anchor >= 0) {
                    // Range select [anchor, i]; replaces the selection unless
                    // Ctrl is also held (extend).
                    if (!ctrl) state.selection.clear();
                    int lo = state.sel_anchor < i ? state.sel_anchor : i;
                    int hi = state.sel_anchor < i ? i : state.sel_anchor;
                    for (int r = lo; r <= hi; ++r) state.selection.insert(r);
                } else if (ctrl) {
                    // Toggle this row, keep the rest.
                    if (is_selected) state.selection.erase(i);
                    else state.selection.insert(i);
                    state.sel_anchor = i;
                } else {
                    // Plain click: this row becomes the sole selection.
                    state.selection.clear();
                    state.selection.insert(i);
                    state.sel_anchor = i;
                }
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    clipper.End();

    if (state.auto_scroll && state.was_at_bottom) {
        ImGui::SetScrollHereY(1.0f);
    }
    state.was_at_bottom = (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f);

    ImGui::EndChild();

    // Status footer: line count and current selection size. (Script input for
    // Phase 6 will live here later.)
    if (state.selection.empty()) {
        ImGui::TextDisabled("%d line%s", line_count, line_count == 1 ? "" : "s");
    } else {
        ImGui::TextDisabled("%d line%s  |  %d selected",
                            line_count, line_count == 1 ? "" : "s",
                            static_cast<int>(state.selection.size()));
    }
}

} // namespace viewer
