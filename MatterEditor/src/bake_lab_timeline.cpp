#include "bake_lab_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "matter/world_session.h"

namespace viewer {

namespace {

constexpr float kRowHeight = 20.0f;
constexpr float kRowGap = 1.0f;
constexpr double kMinSpanMs = 0.001;  // smallest allowed zoomed-in view width

// FNV-1a over the (stable, interned) span name pointer's bytes... spans share
// interned name pointers from bake_trace_names.h, but hashing the string
// contents (not the pointer) keeps the same name always mapping to the same
// color even across sources/collectors.
uint32_t hash_name(const char* name) {
    if (!name) return 0;
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    return h;
}

ImU32 color_for_name(const char* name, bool open, float alpha) {
    uint32_t h = hash_name(name);
    float hue = (h % 360) / 360.0f;
    float sat = open ? 0.30f : 0.55f;
    float val = open ? 0.55f : 0.85f;
    ImVec4 c = ImColor::HSV(hue, sat, val, alpha);
    return ImGui::ColorConvertFloat4ToU32(c);
}

void format_ms(char* buf, size_t buf_size, double ms) {
    if (ms < 1000.0)
        std::snprintf(buf, buf_size, "%.2f ms", ms);
    else
        std::snprintf(buf, buf_size, "%.3f s", ms / 1000.0);
}

}  // namespace

void BakeLabTimeline::refresh_active_source(matter::WorldSession* session) {
    if (!session) return;
    if (sources_.empty()) {
        sources_.push_back(BakeLabTraceSource{"Production session (last bake)", {}, false});
        active_source_ = 0;
    }
    BakeLabTraceSource& src = sources_[active_source_];
    session->last_bake_trace(src.root);
    src.captured = true;

    // Any pointers into the old tree (flat_, an active pin) are now stale.
    pinned_.active = false;
    rebuild_flat();
    fit_view();
}

void BakeLabTimeline::rebuild_flat() {
    flat_.clear();
    if (sources_.empty()) {
        full_begin_ms_ = 0.0;
        full_end_ms_ = 1.0;
        return;
    }
    const BakeLabTraceSource& src = sources_[active_source_];
    if (!src.captured || src.root.children.empty()) {
        full_begin_ms_ = 0.0;
        full_end_ms_ = std::max(1.0, src.root.end_ms);
        return;
    }

    // Single pass, explicit stack (recursion would be fine too, but this
    // avoids relying on the compiler to flatten it and keeps the traversal
    // trivially bounded by span count).
    struct StackEntry { const bake_trace::Span* span; int depth; int parent_index; };
    std::vector<StackEntry> stack;
    stack.push_back({&src.root, 0, -1});
    while (!stack.empty()) {
        StackEntry e = stack.back();
        stack.pop_back();
        int my_index = static_cast<int>(flat_.size());
        flat_.push_back({e.span, e.depth, e.parent_index});
        // Push children in reverse so they pop in original order (cosmetic;
        // draw order among siblings doesn't affect correctness).
        for (auto it = e.span->children.rbegin(); it != e.span->children.rend(); ++it)
            stack.push_back({&(*it), e.depth + 1, my_index});
    }

    full_begin_ms_ = src.root.begin_ms;
    full_end_ms_ = std::max(full_begin_ms_ + kMinSpanMs, src.root.end_ms);
}

void BakeLabTimeline::fit_view() {
    view_begin_ms_ = full_begin_ms_;
    view_end_ms_ = full_end_ms_;
    view_initialized_ = true;
}

void BakeLabTimeline::pin_flat_index(int flat_index) {
    if (flat_index < 0 || flat_index >= static_cast<int>(flat_.size())) return;

    // Walk parent_index chain to build the root -> ... -> span path, then
    // reverse. Copy everything out so the pin survives a later refresh.
    std::vector<std::string> path;
    int i = flat_index;
    while (i != -1) {
        path.push_back(flat_[i].span->name ? flat_[i].span->name : "?");
        i = flat_[i].parent_index;
    }
    std::reverse(path.begin(), path.end());

    const bake_trace::Span* s = flat_[flat_index].span;
    pinned_.active = true;
    pinned_.path = std::move(path);
    pinned_.begin_ms = s->begin_ms;
    pinned_.end_ms = s->end_ms;
    pinned_.open = (s->end_ms == bake_trace::kOpenEndMs);
    pinned_.counters = s->counters;
}

void BakeLabTimeline::draw_source_selector(matter::WorldSession* session) {
    if (sources_.empty())
        sources_.push_back(BakeLabTraceSource{"Production session (last bake)", {}, false});

    if (ImGui::BeginCombo("Source", sources_[active_source_].name.c_str())) {
        for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
            bool selected = (i == active_source_);
            if (ImGui::Selectable(sources_[i].name.c_str(), selected)) {
                active_source_ = i;
                pinned_.active = false;
                rebuild_flat();
                fit_view();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(session == nullptr);
    if (ImGui::Button("Refresh")) refresh_active_source(session);
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Fit")) fit_view();

    const BakeLabTraceSource& src = sources_[active_source_];
    if (!src.captured) {
        ImGui::TextDisabled("No bake trace captured yet — click Refresh after a bake runs.");
    } else if (src.root.children.empty()) {
        ImGui::TextDisabled("No bake trace yet - reload the world to bake.");
    } else {
        ImGui::Text("%d spans, %.2f ms total", static_cast<int>(flat_.size()),
                    full_end_ms_ - full_begin_ms_);
    }
}

void BakeLabTimeline::draw_flamegraph_canvas() {
    if (flat_.empty()) return;

    if (!view_initialized_) fit_view();

    ImGuiIO& io = ImGui::GetIO();
    float content_h = ImGui::GetContentRegionAvail().y;
    // Leave room for the pinned-details section below.
    float canvas_h = std::max(2.0f * kRowHeight, content_h * 0.65f);

    ImGui::BeginChild("##flamegraph_canvas", ImVec2(0, canvas_h), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.x < 1.0f) canvas_size.x = 1.0f;
    if (canvas_size.y < 1.0f) canvas_size.y = 1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Interaction surface covering the whole canvas.
    ImGui::InvisibleButton("##flamegraph_interact", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    double view_span = view_end_ms_ - view_begin_ms_;
    if (view_span < kMinSpanMs) view_span = kMinSpanMs;

    auto time_to_x = [&](double t) -> float {
        return canvas_pos.x + static_cast<float>((t - view_begin_ms_) / view_span) * canvas_size.x;
    };
    auto x_to_time = [&](float x) -> double {
        return view_begin_ms_ + (static_cast<double>(x - canvas_pos.x) / canvas_size.x) * view_span;
    };

    // Zoom around the cursor's time position.
    if (hovered && io.MouseWheel != 0.0f) {
        double t_cursor = x_to_time(io.MousePos.x);
        double factor = std::pow(1.15, static_cast<double>(io.MouseWheel));
        double new_span = view_span / factor;
        // Clamp: never zoom in tighter than kMinSpanMs, never zoom out past
        // ~4x the full trace extent (there's nothing useful past that).
        double full_span = std::max(kMinSpanMs, full_end_ms_ - full_begin_ms_);
        new_span = std::clamp(new_span, kMinSpanMs, full_span * 4.0);
        double t_frac = (view_span > 0.0) ? (t_cursor - view_begin_ms_) / view_span : 0.5;
        view_begin_ms_ = t_cursor - t_frac * new_span;
        view_end_ms_ = view_begin_ms_ + new_span;
        view_span = new_span;
    }

    // Pan: middle-drag anywhere on the canvas, or left-drag (only registers
    // once the drag threshold is exceeded, so a plain left click still
    // reaches the click-to-pin path below). `panned` reflects the whole
    // gesture (drag threshold exceeded at any point since mouse-down), not
    // just whether this specific frame moved — otherwise releasing the
    // button on a paused-but-still-dragging frame would wrongly fall
    // through to click-to-pin.
    bool panned = false;
    for (ImGuiMouseButton btn : {ImGuiMouseButton_Middle, ImGuiMouseButton_Left}) {
        if (active && ImGui::IsMouseDragging(btn, 2.0f)) {
            panned = true;
            ImVec2 delta = ImGui::GetMouseDragDelta(btn, 2.0f);
            if (delta.x != 0.0f) {
                double delta_ms = -static_cast<double>(delta.x) / canvas_size.x * view_span;
                view_begin_ms_ += delta_ms;
                view_end_ms_ += delta_ms;
            }
            ImGui::ResetMouseDragDelta(btn);
        }
    }

    // Keep the view from drifting arbitrarily far from the data (loose
    // clamp: a couple of full-extents of headroom on either side).
    {
        double full_span = std::max(kMinSpanMs, full_end_ms_ - full_begin_ms_);
        double lo = full_begin_ms_ - full_span * 2.0;
        double hi = full_end_ms_ + full_span * 2.0;
        if (view_begin_ms_ < lo) { view_end_ms_ += (lo - view_begin_ms_); view_begin_ms_ = lo; }
        if (view_end_ms_ > hi) { view_begin_ms_ -= (view_end_ms_ - hi); view_end_ms_ = hi; }
    }

    ImVec2 canvas_max(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);
    dl->PushClipRect(canvas_pos, canvas_max, true);
    dl->AddRectFilled(canvas_pos, canvas_max, ImGui::GetColorU32(ImGuiCol_ChildBg));

    int hovered_flat_index = -1;
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    double px_per_ms = canvas_size.x / view_span;

    for (int i = 0; i < static_cast<int>(flat_.size()); ++i) {
        const FlatSpan& fs = flat_[i];
        const bake_trace::Span* s = fs.span;
        bool is_open = (s->end_ms == bake_trace::kOpenEndMs);
        double end_ms = is_open ? full_end_ms_ : s->end_ms;

        // Skip rects that would round to sub-pixel width or fall fully
        // outside the visible window (cheap bounds check before any drawing).
        double width_ms = end_ms - s->begin_ms;
        if (width_ms * px_per_ms < 0.5) continue;
        if (end_ms < view_begin_ms_ || s->begin_ms > view_end_ms_) continue;

        float x0 = time_to_x(s->begin_ms);
        float x1 = time_to_x(end_ms);
        x0 = std::max(x0, canvas_pos.x - 1.0f);
        x1 = std::min(x1, canvas_max.x + 1.0f);
        if (x1 <= x0) continue;

        float y0 = canvas_pos.y + fs.depth * (kRowHeight + kRowGap);
        float y1 = y0 + kRowHeight;
        if (y0 > canvas_max.y) continue;  // too deep to be visible; still counted, not drawn

        ImVec2 p0(x0, y0), p1(x1, y1);
        ImU32 fill = color_for_name(s->name, is_open, is_open ? 0.55f : 1.0f);
        dl->AddRectFilled(p0, p1, fill, 2.0f);
        if (is_open) {
            // Distinct treatment for still-open spans: sparse diagonal
            // hatching over the (already dimmer) fill.
            for (float x = x0 - (y1 - y0); x < x1; x += 6.0f) {
                float lx0 = std::max(x0, x), ly0 = y1;
                float lx1 = std::min(x1, x + (y1 - y0)), ly1 = y0;
                if (lx1 > lx0) dl->AddLine(ImVec2(lx0, ly0), ImVec2(lx1, ly1),
                                           ImGui::GetColorU32(ImGuiCol_Text, 0.15f));
            }
        }
        ImU32 border = ImGui::GetColorU32(ImGuiCol_Border, is_open ? 0.5f : 1.0f);
        dl->AddRect(p0, p1, border, 2.0f);

        // Label: name (+ ms) when there's room; clipped to the rect via
        // AddText's cpu_fine_clip_rect so we never measure-then-truncate.
        float avail_w = x1 - x0 - 4.0f;
        if (avail_w > 8.0f) {
            char label[160];
            char ms_buf[32];
            format_ms(ms_buf, sizeof(ms_buf), is_open ? (full_end_ms_ - s->begin_ms) : width_ms);
            if (is_open)
                std::snprintf(label, sizeof(label), "%s (open, %s)", s->name ? s->name : "?", ms_buf);
            else
                std::snprintf(label, sizeof(label), "%s (%s)", s->name ? s->name : "?", ms_buf);
            ImVec4 clip(x0 + 2.0f, y0, x1 - 2.0f, y1);
            ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddText(font, font_size, ImVec2(x0 + 2.0f, y0 + (kRowHeight - font_size) * 0.5f),
                       text_col, label, nullptr, 0.0f, &clip);
        }

        if (hovered && io.MousePos.x >= p0.x && io.MousePos.x <= p1.x &&
            io.MousePos.y >= p0.y && io.MousePos.y <= p1.y) {
            hovered_flat_index = i;
        }
    }

    dl->PopClipRect();

    if (hovered_flat_index >= 0) {
        const FlatSpan& fs = flat_[hovered_flat_index];
        const bake_trace::Span* s = fs.span;
        bool is_open = (s->end_ms == bake_trace::kOpenEndMs);

        std::vector<std::string> path;
        int idx = hovered_flat_index;
        while (idx != -1) {
            path.push_back(flat_[idx].span->name ? flat_[idx].span->name : "?");
            idx = flat_[idx].parent_index;
        }
        std::reverse(path.begin(), path.end());

        ImGui::BeginTooltip();
        std::string path_str;
        for (size_t k = 0; k < path.size(); ++k) {
            if (k) path_str += " -> ";
            path_str += path[k];
        }
        ImGui::TextUnformatted(path_str.c_str());
        ImGui::Separator();
        char begin_buf[32], end_buf[32], dur_buf[32];
        format_ms(begin_buf, sizeof(begin_buf), s->begin_ms);
        if (is_open) {
            ImGui::Text("begin: %s   end: (open)", begin_buf);
            format_ms(dur_buf, sizeof(dur_buf), full_end_ms_ - s->begin_ms);
            ImGui::Text("duration so far: %s", dur_buf);
        } else {
            format_ms(end_buf, sizeof(end_buf), s->end_ms);
            format_ms(dur_buf, sizeof(dur_buf), s->end_ms - s->begin_ms);
            ImGui::Text("begin: %s   end: %s", begin_buf, end_buf);
            ImGui::Text("duration: %s", dur_buf);
        }
        if (!s->counters.empty()) {
            ImGui::Separator();
            for (const auto& c : s->counters)
                ImGui::Text("%s = %g", c.name ? c.name : "?", c.value);
        }
        ImGui::EndTooltip();
    }

    // Click (not drag) pins the hovered span. `panned` guards against a
    // left-drag-to-pan also registering as a click once the button is
    // released; the drag-distance threshold on IsMouseDragging above already
    // keeps small jitter from being treated as a pan.
    if (hovered && !panned && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && hovered_flat_index >= 0) {
        pin_flat_index(hovered_flat_index);
    }

    ImGui::EndChild();
}

void BakeLabTimeline::draw_pinned_panel() {
    ImGui::SeparatorText("Pinned span");
    if (!pinned_.active) {
        ImGui::TextDisabled("Click a span in the flamegraph to pin its details here.");
        return;
    }
    if (ImGui::Button("Clear") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                   ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        pinned_.active = false;
        return;
    }
    std::string path_str;
    for (size_t k = 0; k < pinned_.path.size(); ++k) {
        if (k) path_str += " -> ";
        path_str += pinned_.path[k];
    }
    ImGui::TextWrapped("%s", path_str.c_str());

    char begin_buf[32], dur_buf[32];
    format_ms(begin_buf, sizeof(begin_buf), pinned_.begin_ms);
    ImGui::Text("begin: %s", begin_buf);
    if (pinned_.open) {
        ImGui::Text("end: (open)");
        format_ms(dur_buf, sizeof(dur_buf), full_end_ms_ - pinned_.begin_ms);
        ImGui::Text("duration so far: %s", dur_buf);
    } else {
        char end_buf[32];
        format_ms(end_buf, sizeof(end_buf), pinned_.end_ms);
        format_ms(dur_buf, sizeof(dur_buf), pinned_.end_ms - pinned_.begin_ms);
        ImGui::Text("end: %s", end_buf);
        ImGui::Text("duration: %s", dur_buf);
    }
    if (!pinned_.counters.empty()) {
        ImGui::Separator();
        for (const auto& c : pinned_.counters)
            ImGui::Text("%s = %g", c.name ? c.name : "?", c.value);
    }
}

void BakeLabTimeline::draw(matter::WorldSession* session) {
    draw_source_selector(session);
    ImGui::Separator();
    if (flat_.empty()) {
        ImGui::TextDisabled("Nothing to draw yet.");
        return;
    }
    draw_flamegraph_canvas();
    draw_pinned_panel();
}

} // namespace viewer
