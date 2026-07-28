// Dialog half of the in-editor issue reporter (ImGui). The writer half lives
// in issue_reporter.cpp, which stays ImGui-free — same split as
// console_log.cpp / console_panel.cpp.
//
// Two surfaces live here: the full-screen selection overlay you drag a box on
// after F9 (drawn over the frozen frame, so an animating scene cannot slide out
// from under the selection), and the report window that accumulates the shots.
#include "issue_reporter.h"

#include <algorithm>
#include <cstring>

#include "imgui.h"

namespace viewer {

namespace {

ImVec2 to_framebuffer(const ImVec2& point, uint32_t frame_width,
                      uint32_t frame_height) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float sx = display.x > 0 ? static_cast<float>(frame_width) / display.x : 1.0f;
    const float sy = display.y > 0 ? static_cast<float>(frame_height) / display.y : 1.0f;
    return ImVec2(point.x * sx, point.y * sy);
}

ShotRect rect_from_corners(const ImVec2& a, const ImVec2& b) {
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    return ShotRect{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                    static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
}

const char* region_label(ShotRegion region) {
    switch (region) {
        case ShotRegion::Viewport: return "viewport";
        case ShotRegion::Custom:   return "region";
        case ShotRegion::FullWindow:
        default:                   return "full";
    }
}

// Commits the drag: main.cpp's capture step reads pending_rect/pending_region
// off the state and writes the PNG out of the frozen buffer.
void commit_region(IssueReporterState& state, const ShotRect& rect) {
    state.pending_region = rect.empty() ? ShotRegion::FullWindow
                                        : ShotRegion::Custom;
    state.pending_rect = rect;
    state.selection_committed = true;
    state.phase = ReporterPhase::Editing;
    state.window_open = true;
}

// The frozen frame plus the rubber band. Mouse state comes straight off the IO
// rather than through a window, so the drag works anywhere on screen.
void draw_selection_overlay(IssueReporterState& state, uint32_t frame_width,
                            uint32_t frame_height) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 screen = io.DisplaySize;

    // The captured frame, redrawn over the live one. Without this you would be
    // dragging over a moving scene while cropping a still.
    if (state.frozen_preview) {
        draw->AddImage(reinterpret_cast<ImTextureID>(state.frozen_preview),
                       ImVec2(0, 0), screen);
    }
    // Dim everything, then punch the selection back to full brightness so the
    // chosen region is what stands out.
    draw->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 110));

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.phase = state.shots.empty() ? ReporterPhase::Idle
                                          : ReporterPhase::Editing;
        state.window_open = !state.shots.empty();
        state.drag_active = false;
        state.status = "capture discarded";
        state.status_is_error = false;
        return;
    }
    // Enter (or a click with no drag) keeps the whole frame — the fast path
    // when the defect is the entire screen, or you just want it all.
    const bool take_everything =
        ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (take_everything) {
        commit_region(state, ShotRect{});
        return;
    }

    if (!state.drag_active && io.MouseClicked[0]) {
        state.drag_active = true;
        state.drag_start[0] = io.MousePos.x;
        state.drag_start[1] = io.MousePos.y;
    }
    if (state.drag_active) {
        state.drag_end[0] = io.MousePos.x;
        state.drag_end[1] = io.MousePos.y;
        const ImVec2 a(state.drag_start[0], state.drag_start[1]);
        const ImVec2 b(state.drag_end[0], state.drag_end[1]);
        if (state.frozen_preview) {
            // Re-show the selected part undimmed by drawing that sub-rect of
            // the frozen image again on top.
            const ImVec2 tl(std::min(a.x, b.x), std::min(a.y, b.y));
            const ImVec2 br(std::max(a.x, b.x), std::max(a.y, b.y));
            const ImVec2 uv0(screen.x > 0 ? tl.x / screen.x : 0.0f,
                             screen.y > 0 ? tl.y / screen.y : 0.0f);
            const ImVec2 uv1(screen.x > 0 ? br.x / screen.x : 1.0f,
                             screen.y > 0 ? br.y / screen.y : 1.0f);
            draw->AddImage(reinterpret_cast<ImTextureID>(state.frozen_preview),
                           tl, br, uv0, uv1);
        }
        draw->AddRect(a, b, IM_COL32(90, 170, 255, 230), 0.0f, 0, 2.0f);
        const ShotRect px = rect_from_corners(
            to_framebuffer(a, frame_width, frame_height),
            to_framebuffer(b, frame_width, frame_height));
        char size[64];
        std::snprintf(size, sizeof(size), "%d x %d", px.w, px.h);
        draw->AddText(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y) - 18.0f),
                      IM_COL32(235, 235, 240, 255), size);
    }

    const char* hint =
        state.drag_active
            ? "Release to keep this region     Esc cancels"
            : "Drag a box over what's wrong     Enter keeps the whole screen     Esc cancels";
    const ImVec2 size = ImGui::CalcTextSize(hint);
    const ImVec2 at(screen.x * 0.5f - size.x * 0.5f, 24.0f);
    draw->AddRectFilled(ImVec2(at.x - 12, at.y - 7),
                        ImVec2(at.x + size.x + 12, at.y + size.y + 7),
                        IM_COL32(18, 18, 22, 235), 4.0f);
    draw->AddText(at, IM_COL32(235, 235, 240, 255), hint);

    if (state.drag_active && io.MouseReleased[0]) {
        state.drag_active = false;
        const ShotRect rect = rect_from_corners(
            to_framebuffer(ImVec2(state.drag_start[0], state.drag_start[1]),
                           frame_width, frame_height),
            to_framebuffer(ImVec2(state.drag_end[0], state.drag_end[1]),
                           frame_width, frame_height));
        // A click without a drag means "the whole thing", not a 2x3 crop.
        commit_region(state, (rect.w < 8 || rect.h < 8) ? ShotRect{} : rect);
    }
}

// One row of the shot list: thumbnail, editable caption, remove.
void draw_shot_row(IssueReporterState& state, size_t index, bool& removed) {
    IssueShot& shot = state.shots[index];
    ImGui::PushID(static_cast<int>(index));

    if (shot.preview && shot.preview_width > 0) {
        // Fit into a fixed row height so a tall crop and a wide one line up.
        const float max_h = 64.0f;
        const float aspect = static_cast<float>(shot.preview_width) /
                             static_cast<float>(std::max(1u, shot.preview_height));
        const ImVec2 size(max_h * aspect, max_h);
        ImGui::Image(reinterpret_cast<ImTextureID>(shot.preview), size);
        if (ImGui::IsItemHovered()) {
            // Hover for a big version — a 64px row is enough to tell shots
            // apart, not enough to check one.
            ImGui::BeginTooltip();
            const float big = 420.0f;
            ImGui::Image(reinterpret_cast<ImTextureID>(shot.preview),
                         ImVec2(big, big / std::max(0.01f, aspect)));
            ImGui::EndTooltip();
        }
    } else {
        ImGui::Dummy(ImVec2(96, 64));
    }
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextDisabled("%s  %s  %ux%u", shot.file.c_str(),
                        region_label(shot.region), shot.width, shot.height);
    char caption[256];
    std::snprintf(caption, sizeof(caption), "%s", shot.caption.c_str());
    ImGui::SetNextItemWidth(-70);
    if (ImGui::InputTextWithHint("##caption", "what this shot shows", caption,
                                 sizeof(caption)))
        shot.caption = caption;
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) removed = true;
    ImGui::EndGroup();

    ImGui::PopID();
}

} // namespace

void begin_region_capture(IssueReporterState& state) {
    // Hide the window before the readback so the dialog is not in its own
    // evidence; two settle frames covers this frame plus the hidden one.
    state.window_open = false;
    state.phase = ReporterPhase::AwaitingCapture;
    state.capture_is_region_pick = true;
    state.capture_settle = 2;
    state.pending_region = ShotRegion::FullWindow;
    state.pending_rect = ShotRect{};
}

void begin_viewport_capture(IssueReporterState& state) {
    state.window_open = false;
    state.phase = ReporterPhase::AwaitingCapture;
    state.capture_is_region_pick = false;
    state.capture_settle = 2;
    state.pending_region = ShotRegion::Viewport;
    state.pending_rect = ShotRect{};
}

bool issue_reporter_wants_mouse(const IssueReporterState& state) {
    return state.phase == ReporterPhase::SelectingRegion;
}

bool draw_issue_reporter(IssueReporterState& state, uint32_t frame_width,
                         uint32_t frame_height) {
    if (state.phase == ReporterPhase::AwaitingCapture) return false;
    if (state.phase == ReporterPhase::SelectingRegion) {
        draw_selection_overlay(state, frame_width, frame_height);
        return false;
    }
    if (!state.window_open) return false;

    bool submitted = false;
    // No position or size override: the window restores wherever it was left,
    // docked or floating, from imgui.ini. FirstUseEver only seeds a brand-new
    // profile.
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Report Issue", &state.window_open)) {
        ImGui::End();
        return false;
    }

    ImGui::TextDisabled("F9 region capture      F10 viewport capture");
    ImGui::Separator();

    ImGui::TextUnformatted("What's wrong (and what you expected)");
    ImGui::InputTextMultiline("##note", state.note, sizeof(state.note),
                              ImVec2(-1, 110));

    ImGui::Separator();
    ImGui::Text("Shots (%zu)", state.shots.size());
    if (state.shots.empty()) {
        ImGui::TextDisabled("None yet — F9 or F10 to capture.");
    } else {
        ImGui::BeginChild("shots", ImVec2(-1, 210), true);
        size_t remove_at = state.shots.size();
        for (size_t i = 0; i < state.shots.size(); ++i) {
            bool removed = false;
            draw_shot_row(state, i, removed);
            if (removed) remove_at = i;
            if (i + 1 < state.shots.size()) ImGui::Separator();
        }
        ImGui::EndChild();
        if (remove_at < state.shots.size()) {
            // The PNG and its preview texture stay put: nothing references an
            // un-listed file, and freeing a descriptor mid-frame would pull it
            // out from under this frame's draw list. main.cpp reclaims them
            // when the report is filed or discarded.
            state.shots.erase(state.shots.begin() +
                              static_cast<long>(remove_at));
        }
    }

    ImGui::Separator();
    const bool can_file = !state.shots.empty() || state.note[0] != '\0';
    if (!can_file) ImGui::BeginDisabled();
    if (ImGui::Button("File report")) submitted = true;
    if (!can_file) ImGui::EndDisabled();
    if (!can_file) {
        ImGui::SameLine();
        ImGui::TextDisabled("(capture something, or write a note)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        // main.cpp owns the preview textures, so it clears the state; setting
        // the phase is how this asks for that.
        state.phase = ReporterPhase::Idle;
        state.window_open = false;
    }

    if (!state.status.empty()) {
        ImGui::Spacing();
        if (state.status_is_error)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s",
                               state.status.c_str());
        else
            ImGui::TextDisabled("%s", state.status.c_str());
    }

    ImGui::End();
    return submitted;
}

} // namespace viewer
