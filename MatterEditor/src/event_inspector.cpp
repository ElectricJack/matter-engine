#include "event_inspector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "matter/event/subscription.h"  // phase / lane vocabulary
#include "matter/world_session.h"

namespace viewer {

namespace {

using matter::evt::Hub;
using matter::evt::LaneCounters;
using matter::evt::RegistrySnapshotEntry;
using matter::evt::TraceRecord;
using matter::evt::TraceSubscriberRecord;

const char* phase_name(matter::evt::phase p) {
    switch (p) {
        case matter::evt::phase::First:   return "First";
        case matter::evt::phase::Early:   return "Early";
        case matter::evt::phase::Default: return "Default";
        case matter::evt::phase::Late:    return "Late";
        case matter::evt::phase::Last:    return "Last";
    }
    return "?";
}

// Predefined engine lane ids (subscription.cpp): 1 app, 2 legacy_poll,
// 3 diagnostics; others are make_lane()-minted, shown by id.
std::string lane_label(uint32_t id) {
    switch (id) {
        case 1: return "app";
        case 2: return "legacy_poll";
        case 3: return "diagnostics";
        default: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "lane#%u", id);
            return buf;
        }
    }
}

std::string thread_label(std::thread::id id) {
    std::ostringstream oss;
    oss << id;
    return oss.str();
}

// Self-contained copy of bake_lab_timeline's name->color hash (that helper
// lives in bake_lab_timeline.cpp's anonymous namespace, so it isn't shared;
// duplicating the ~10 lines keeps this file free of a cross-widget dependency
// while matching the flamegraph's visual idiom).
uint32_t hash_name(const char* name) {
    if (!name) return 0;
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    return h;
}

ImU32 color_for_name(const char* name, float alpha) {
    uint32_t h = hash_name(name);
    float hue = (h % 360) / 360.0f;
    ImVec4 c = ImColor::HSV(hue, 0.55f, 0.85f, alpha);
    return ImGui::ColorConvertFloat4ToU32(c);
}

double ms_between(std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

bool name_matches_filter(const char* name, const char* filter) {
    if (!filter || filter[0] == '\0') return true;
    if (!name) return false;
    // Case-insensitive substring: a plain namespace filter ("bake.", "cmd.")
    // matches any event whose name contains it.
    std::string hay(name), needle(filter);
    std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hub selector — mirrors bake_lab_timeline's source selector: a combo over a
// rebuilt-each-frame list, an integer active index clamped into range. The
// crucial difference is that the hub list is rebuilt EVERY frame from live
// pointers (app_hub + session->events()), so a world switch that frees the
// session hub never leaves a stale entry — the freed hub simply isn't in this
// frame's list.
// ---------------------------------------------------------------------------
void EventInspector::draw_hub_selector(const std::vector<HubEntry>& hubs) {
    if (active_hub_ >= static_cast<int>(hubs.size())) active_hub_ = 0;
    if (active_hub_ < 0) active_hub_ = 0;

    const char* preview = hubs.empty() ? "(none)" : hubs[active_hub_].name.c_str();
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("Hub", preview)) {
        for (int i = 0; i < static_cast<int>(hubs.size()); ++i) {
            bool selected = (i == active_hub_);
            if (ImGui::Selectable(hubs[i].name.c_str(), selected)) active_hub_ = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// ---------------------------------------------------------------------------
// Registry view (S I.8 view 1): event types x ordered subscribers.
// ---------------------------------------------------------------------------
void EventInspector::draw_registry_view(Hub& hub) {
    std::vector<RegistrySnapshotEntry> snap = hub.registry_snapshot();
    // registry_snapshot() returns types in unordered-map order; sort by event
    // name so the table is stable. Subscribers within each entry already come
    // sorted by the (phase,priority,name) ordering key.
    std::sort(snap.begin(), snap.end(), [](const RegistrySnapshotEntry& a, const RegistrySnapshotEntry& b) {
        const char* an = a.event_name ? a.event_name : "";
        const char* bn = b.event_name ? b.event_name : "";
        return std::strcmp(an, bn) < 0;
    });

    // Lane drop/reject counters — highlighted red when nonzero (S I.8).
    std::vector<LaneCounters> lanes = hub.lane_counters();
    ImGui::TextUnformatted("Lane drop counters:");
    ImGui::SameLine();
    if (lanes.empty()) {
        ImGui::TextDisabled("(no queued lanes on this hub)");
    } else {
        bool first = true;
        for (const LaneCounters& lc : lanes) {
            if (!first) ImGui::SameLine();
            first = false;
            bool bad = (lc.dropped != 0 || lc.rejected != 0);
            ImVec4 col = bad ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                             : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(col, "%s[drop %llu / rej %llu]", lane_label(lc.lane_id).c_str(),
                               static_cast<unsigned long long>(lc.dropped),
                               static_cast<unsigned long long>(lc.rejected));
        }
    }

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##reg_filter", "filter by name (e.g. bake. / cmd.)", registry_filter_,
                             sizeof(registry_filter_));
    ImGui::SameLine();
    if (ImGui::Button("Clear##reg")) registry_filter_[0] = '\0';

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##registry_table", 6, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Event / Subscriber", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Delivery", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Subscribe site", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();

    int shown = 0;
    for (const RegistrySnapshotEntry& e : snap) {
        if (!name_matches_filter(e.event_name, registry_filter_)) continue;
        ++shown;

        // Group header row for the event type (spans as an emphasized label).
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImU32 hdr = color_for_name(e.event_name, 0.20f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hdr);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color_for_name(e.event_name, 1.0f)), "%s",
                           e.event_name ? e.event_name : "<anonymous>");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextDisabled("%d sub", static_cast<int>(e.subscribers.size()));

        for (const auto& s : e.subscribers) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(14.0f);
            if (s.active)
                ImGui::TextUnformatted(s.name.c_str());
            else
                ImGui::TextDisabled("%s (inactive)", s.name.c_str());
            ImGui::Unindent(14.0f);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(phase_name(s.ph));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", s.priority);
            ImGui::TableSetColumnIndex(3);
            if (s.is_immediate)
                ImGui::TextUnformatted("immediate");
            else
                ImGui::TextUnformatted(lane_label(s.lane_id).c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(s.delivery_count));
            ImGui::TableSetColumnIndex(5);
            if (s.file)
                ImGui::TextDisabled("%s:%d", s.file, s.line);
            else
                ImGui::TextDisabled("-");
        }
    }
    ImGui::EndTable();

    if (shown == 0) {
        ImGui::TextDisabled("No event types match the filter.");
    }
}

// ---------------------------------------------------------------------------
// Trace view (S I.8 view 2): live-tailing recorded emits with nesting.
// ---------------------------------------------------------------------------
void EventInspector::draw_trace_view(Hub& hub) {
    bool tracing = hub.trace_enabled();
    // The ONLY mutation the inspector performs: toggling the per-hub trace
    // on/off, a legitimate diagnostic control (E4c constraint).
    if (ImGui::Checkbox("Tracing enabled", &tracing)) hub.set_trace_enabled(tracing);
    ImGui::SameLine();
    ImGui::Checkbox("Pause", &trace_paused_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##trace_filter", "filter by event name", trace_filter_,
                             sizeof(trace_filter_));
    ImGui::SameLine();
    if (ImGui::Button("Clear##trace")) trace_filter_[0] = '\0';

    if (!hub.trace_enabled()) {
        ImGui::TextDisabled(
            "Tracing is off for this hub — enable it above to record emits into the ring buffer.");
        return;
    }

    // Live-tail unless paused; while paused, keep showing the frozen copy.
    if (!trace_paused_) frozen_trace_ = hub.trace_snapshot();
    const std::vector<TraceRecord>& records = frozen_trace_;

    ImGui::Text("%d records in ring buffer%s", static_cast<int>(records.size()),
                trace_paused_ ? " (paused)" : "");

    // Split: scrolling record list on the left/top, pinned detail below.
    float avail_h = ImGui::GetContentRegionAvail().y;
    float list_h = trace_pin_active_ ? std::max(80.0f, avail_h * 0.62f) : avail_h;

    if (ImGui::BeginChild("##trace_list", ImVec2(0, list_h), true)) {
        if (records.empty()) {
            ImGui::TextDisabled("No emits recorded yet.");
        } else {
            // t0 = earliest emit in the snapshot, so timestamps read as a
            // stable relative offset within the visible buffer.
            std::chrono::steady_clock::time_point t0 = records.front().emit_time;
            for (const TraceRecord& r : records)
                if (r.emit_time < t0) t0 = r.emit_time;

            for (size_t i = 0; i < records.size(); ++i) {
                const TraceRecord& r = records[i];
                if (!name_matches_filter(r.event_name.c_str(), trace_filter_)) continue;

                ImGui::PushID(static_cast<int>(i));
                // Nesting indentation: handler-emitted events carry a higher
                // nesting_depth, so cascades step to the right (S I.6/I.8).
                float indent = 14.0f * static_cast<float>(r.nesting_depth);
                if (indent > 0.0f) ImGui::Indent(indent);

                double emit_off = ms_between(t0, r.emit_time);
                double latency = ms_between(r.emit_time, r.delivery_time);
                char header[256];
                std::snprintf(header, sizeof(header), "%s  @%.3f ms  thr %s  (%d subs)%s",
                              r.event_name.empty() ? "<unknown>" : r.event_name.c_str(), emit_off,
                              thread_label(r.thread).c_str(), static_cast<int>(r.subscribers.size()),
                              latency > 0.001 ? "  [queued]" : "");

                ImGui::PushStyleColor(ImGuiCol_Text, color_for_name(r.event_name.c_str(), 1.0f));
                bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGui::PopStyleColor();

                // Click-to-pin: copy the whole record out so its detail
                // survives ring-buffer churn / world switches (the tree node
                // also expands on the same click, which is fine — a clicked
                // emit both expands inline and pins its detail below).
                if (ImGui::IsItemClicked()) {
                    trace_pin_ = r;
                    trace_pin_active_ = true;
                }

                if (open) {
                    if (latency > 0.001)
                        ImGui::TextDisabled("queue latency: %.3f ms", latency);
                    for (const TraceSubscriberRecord& s : r.subscribers) {
                        ImGui::BulletText("%s  [%s]  %.4f ms", s.name.c_str(),
                                          s.is_immediate ? "immediate" : lane_label(s.lane_id).c_str(),
                                          s.handler_ms);
                    }
                    if (r.subscribers.empty()) ImGui::TextDisabled("(no active subscribers)");
                    ImGui::TreePop();
                }

                if (indent > 0.0f) ImGui::Unindent(indent);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    if (trace_pin_active_) {
        ImGui::SeparatorText("Pinned emit");
        if (ImGui::Button("Unpin")) {
            trace_pin_active_ = false;
        } else {
            ImGui::Text("%s", trace_pin_.event_name.empty() ? "<unknown>"
                                                            : trace_pin_.event_name.c_str());
            ImGui::Text("thread: %s   nesting depth: %d", thread_label(trace_pin_.thread).c_str(),
                        trace_pin_.nesting_depth);
            double lat = ms_between(trace_pin_.emit_time, trace_pin_.delivery_time);
            ImGui::Text("queue latency: %.3f ms   subscribers: %d", lat,
                        static_cast<int>(trace_pin_.subscribers.size()));
            for (const TraceSubscriberRecord& s : trace_pin_.subscribers) {
                ImGui::BulletText("%s  [%s]  %.4f ms", s.name.c_str(),
                                  s.is_immediate ? "immediate" : lane_label(s.lane_id).c_str(),
                                  s.handler_ms);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Timeline view (S I.8 view 3): emissions + handler durations.
//
// Reuse note: bake_lab_timeline's flamegraph is tightly coupled to a
// bake_trace::Span tree pulled from WorldSession::last_bake_trace() through
// private methods, so it can't be handed an event trace without refactoring
// task-2.2 code. Per the E4c brief ("a correct, readable timeline beats a
// perfect flamegraph"), this view keeps the SHAPE (emit spans as a top row,
// per-subscriber handler bars nested beneath, hashed-name colors, hover
// tooltip) but renders a self-contained fit-to-width flamegraph — no zoom/pan.
// The reused idiom is the visual language (color_for_name, row-of-rects draw
// list, hover tooltip); the data source and layout are event-trace specific.
// ---------------------------------------------------------------------------
void EventInspector::draw_timeline_view(Hub& hub) {
    if (!hub.trace_enabled()) {
        ImGui::TextDisabled("Tracing is off for this hub — enable it in the Trace tab to populate "
                            "the timeline.");
        return;
    }
    std::vector<TraceRecord> records = hub.trace_snapshot();
    if (records.empty()) {
        ImGui::TextDisabled("No emits recorded yet.");
        return;
    }

    // One drawable bar. depth 0 = emit row, depth 1 = subscriber handler row.
    struct Bar {
        double begin_ms;
        double end_ms;
        int depth;
        const char* name;
        double value_ms;  // duration for the tooltip
    };
    std::vector<Bar> bars;

    std::chrono::steady_clock::time_point t0 = records.front().emit_time;
    for (const TraceRecord& r : records)
        if (r.emit_time < t0) t0 = r.emit_time;

    double total_end = 1.0;
    int max_depth = 0;
    for (const TraceRecord& r : records) {
        double emit_off = ms_between(t0, r.emit_time);
        double deliver_off = ms_between(t0, r.delivery_time);
        // Subscriber handlers run sequentially at delivery; lay them end to
        // end from the delivery time, each width = handler_ms.
        double cursor = deliver_off;
        for (const TraceSubscriberRecord& s : r.subscribers) {
            double w = std::max(0.0, s.handler_ms);
            bars.push_back(Bar{cursor, cursor + w, r.nesting_depth + 1, s.name.c_str(), w});
            cursor += w;
            max_depth = std::max(max_depth, r.nesting_depth + 1);
        }
        double emit_end = std::max(cursor, emit_off + 0.001);
        bars.push_back(Bar{emit_off, emit_end,
                           r.nesting_depth, r.event_name.empty() ? "<unknown>" : r.event_name.c_str(),
                           emit_end - emit_off});
        max_depth = std::max(max_depth, r.nesting_depth);
        total_end = std::max(total_end, emit_end);
    }

    ImGui::Text("%d emits, %.3f ms span", static_cast<int>(records.size()), total_end);

    const float row_h = 18.0f;
    const float row_gap = 1.0f;
    float canvas_h = (max_depth + 1) * (row_h + row_gap) + 4.0f;
    canvas_h = std::min(canvas_h, ImGui::GetContentRegionAvail().y);
    canvas_h = std::max(canvas_h, row_h + 4.0f);

    ImGui::BeginChild("##evt_timeline", ImVec2(0, canvas_h), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float canvas_w = ImGui::GetContentRegionAvail().x;
    if (canvas_w < 1.0f) canvas_w = 1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    double px_per_ms = canvas_w / total_end;
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();

    int hovered = -1;
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        const Bar& b = bars[i];
        float x0 = origin.x + static_cast<float>(b.begin_ms * px_per_ms);
        float x1 = origin.x + static_cast<float>(b.end_ms * px_per_ms);
        if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;
        float y0 = origin.y + b.depth * (row_h + row_gap);
        float y1 = y0 + row_h;
        ImVec2 p0(x0, y0), p1(x1, y1);
        dl->AddRectFilled(p0, p1, color_for_name(b.name, 1.0f), 2.0f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), 2.0f);
        if (x1 - x0 > 24.0f) {
            ImVec4 clip(x0 + 2.0f, y0, x1 - 2.0f, y1);
            dl->AddText(font, font_size, ImVec2(x0 + 3.0f, y0 + (row_h - font_size) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), b.name, nullptr, 0.0f, &clip);
        }
        if (io.MousePos.x >= p0.x && io.MousePos.x <= p1.x && io.MousePos.y >= p0.y &&
            io.MousePos.y <= p1.y) {
            hovered = i;
        }
    }
    // Reserve the layout area so the child scrolls correctly.
    ImGui::Dummy(ImVec2(canvas_w, (max_depth + 1) * (row_h + row_gap)));

    if (hovered >= 0 && ImGui::IsWindowHovered()) {
        const Bar& b = bars[hovered];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(b.name ? b.name : "?");
        ImGui::Separator();
        ImGui::Text("%s", b.depth == 0 ? "emit span" : "handler");
        ImGui::Text("begin: %.4f ms   duration: %.4f ms", b.begin_ms, b.value_ms);
        ImGui::EndTooltip();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
void EventInspector::draw(matter::evt::Hub* app_hub, matter::WorldSession* session) {
    // Rebuild the hub list every frame from LIVE pointers — this is the safety
    // property the E4c brief calls out: session->events() is fetched fresh here
    // and never cached on the inspector, so a world switch that destroys the
    // production session hub simply drops it from this frame's list rather than
    // leaving a dangling pointer behind.
    std::vector<HubEntry> hubs;
    if (app_hub) hubs.push_back(HubEntry{"App hub", app_hub});
    if (session) hubs.push_back(HubEntry{"Session hub (production)", &session->events()});

    if (hubs.empty()) {
        ImGui::TextDisabled("No hub available to inspect (no world open).");
        return;
    }

    draw_hub_selector(hubs);
    if (active_hub_ >= static_cast<int>(hubs.size())) active_hub_ = 0;
    Hub& hub = *hubs[active_hub_].hub;

    ImGui::Separator();
    if (ImGui::BeginTabBar("##event_inspector_views")) {
        if (ImGui::BeginTabItem("Registry")) {
            draw_registry_view(hub);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Trace")) {
            draw_trace_view(hub);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Timeline")) {
            draw_timeline_view(hub);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace viewer
