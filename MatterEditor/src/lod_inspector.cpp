// MatterEditor/src/lod_inspector.cpp
//
// LodInspector's drawing half. lod_inspector.h carries the design context
// (part-workbench.md SS-I.5 / W4) and the contract for all three methods; this
// file is the ImGui.
//
// Everything on screen is queried FRESH from matter::WorldSession every frame —
// part_lod_level_count/part_lod_level_info per row, then
// part_child_summary_count/part_child_summary per child row. Nothing is cached
// between frames, so a bake that publishes new LOD data shows up on the next
// draw with no invalidation step, at the cost of a query per row per frame.
// A query that returns false renders "--" rather than a stale or invented
// number.
//
// The inspector writes only its own two toggles; PartWorkbench copies them onto
// matter::RenderOptions (apply()) before the isolation session's next render.
// Nothing here can affect a bake.
//
// ImGui/main thread only.

#include "lod_inspector.h"

#include "imgui.h"

#include <cstdio>

namespace viewer {

void LodInspector::reset() {
    force_lod_ = -1;
    hide_child_instances_ = false;
}

void LodInspector::apply(matter::RenderOptions& opts) const {
    opts.force_lod = force_lod_;
    opts.hide_child_instances = hide_child_instances_;
}

// Draws the section and, as a side effect, keeps the debug overrides honest:
// the part-hash change check runs BEFORE the null/zero early-outs, so closing
// a part (hash 0) resets the toggles just as switching to a different part
// does. Every early-out below still leaves a visible explanation on screen
// rather than an empty section.
void LodInspector::draw(matter::WorldSession* session, uint64_t part_hash) {
    ImGui::SeparatorText("LOD Inspector");

    if (part_hash != last_part_hash_) {
        // A different root part (new open_part(), or a variation flip that
        // resolved to a different hash): a forced level/hidden-children
        // choice from the previous part must never silently carry over.
        reset();
        last_part_hash_ = part_hash;
    }

    if (!session || part_hash == 0) {
        ImGui::TextDisabled("Open and bake a part above to inspect its LOD ladder.");
        return;
    }

    const uint32_t level_count = session->part_lod_level_count(part_hash);
    if (level_count == 0) {
        ImGui::TextDisabled("No LOD data loaded yet for this part (bake in flight, or not yet published).");
        return;
    }

    ImGui::TextWrapped(
        "View-only debug overrides — never change what bakes (part-workbench.md SS-I.5). "
        "Force a level to inspect it up close regardless of camera distance.");

    // --- Level selector: Auto + one radio per LOD level -------------------
    bool auto_selected = (force_lod_ < 0);
    if (ImGui::RadioButton("Auto (camera distance)", auto_selected)) force_lod_ = -1;

    if (ImGui::BeginTable("lod_inspector_levels", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("View");
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Triangles");
        ImGui::TableSetupColumn("Threshold");
        ImGui::TableHeadersRow();
        for (uint32_t level = 0; level < level_count; ++level) {
            matter::PartLodLevelInfo info{};
            const bool have_info = session->part_lod_level_info(part_hash, level, info);
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(level));
            ImGui::TableSetColumnIndex(0);
            const bool selected = (force_lod_ == static_cast<int>(level));
            if (ImGui::RadioButton("##view", selected)) force_lod_ = static_cast<int>(level);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("LOD %u", level);
            ImGui::TableSetColumnIndex(2);
            if (have_info) ImGui::Text("%u", info.triangle_count);
            else ImGui::TextDisabled("--");
            ImGui::TableSetColumnIndex(3);
            if (have_info) ImGui::Text("%.4f", info.threshold);
            else ImGui::TextDisabled("--");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    // --- Child-instance visibility (W4 achieved granularity: show root only) ---
    ImGui::Checkbox("Show root only (hide child instances)", &hide_child_instances_);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "W4 module-hiding granularity: hides the part's baked child-instance\n"
            "subtrees entirely so only its own root-level mesh renders. Per-module\n"
            "filtering (hide just \"Leaf\") is deferred to a future pass -- see\n"
            "docs/part-workbench.md W4 notes.");
    }

    const uint32_t child_count = session->part_child_summary_count(part_hash);
    if (child_count == 0) {
        ImGui::TextDisabled("No child instances (leaf part).");
        return;
    }

    ImGui::Text("Child instances (%u distinct):", child_count);
    if (ImGui::BeginTable("lod_inspector_children", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Module");
        ImGui::TableSetupColumn("Hash");
        ImGui::TableSetupColumn("Count");
        ImGui::TableHeadersRow();
        for (uint32_t idx = 0; idx < child_count; ++idx) {
            matter::PartChildSummary child{};
            if (!session->part_child_summary(part_hash, idx, child)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (child.module_name && *child.module_name)
                ImGui::TextUnformatted(child.module_name);
            else
                ImGui::TextDisabled("(unnamed)");
            ImGui::TableSetColumnIndex(1);
            char hash_buf[20];
            std::snprintf(hash_buf, sizeof(hash_buf), "%016llx",
                          static_cast<unsigned long long>(child.child_hash));
            ImGui::TextUnformatted(hash_buf);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("x%u", child.instance_count);
        }
        ImGui::EndTable();
    }
}

} // namespace viewer
