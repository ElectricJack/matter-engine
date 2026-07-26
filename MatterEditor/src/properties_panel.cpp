// Phase 5 Task 7 — Properties inspector panel implementation.
// Task 9 — Baked root properties: read-only info card for BakedRoot
// selections, sourced from the part_graph_snapshot::Snapshot.
// Task 8 — Specialized editors: component-specific controls (part picker,
// physics actions, sector streaming) rendered inline after the
// auto-generated fields of the matching CollapsingHeader.
#include "properties_panel.h"
#include "os_open.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace viewer {

using matter::scene::SceneEntityId;

namespace {

// ---------------------------------------------------------------------------
// Quaternion <-> Euler (degrees) helpers for QuaternionEditor. XYZ intrinsic
// Tait-Bryan angles. Recomputed from the live/cached quaternion every frame,
// so this is a display-only proxy (no persistent euler state) — simple and
// avoids drift, at the cost of the usual gimbal-adjacent re-normalization
// near +-90 degrees pitch. Acceptable for a first pass per the Task 7 spec.
// ---------------------------------------------------------------------------

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

matter::Float3 quat_to_euler_degrees(const matter::Quaternion& q) {
    const float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
    const float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float roll = std::atan2(sinr_cosp, cosr_cosp);

    const float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    float pitch;
    if (std::fabs(sinp) >= 1.0f)
        pitch = std::copysign(kPi / 2.0f, sinp);
    else
        pitch = std::asin(sinp);

    const float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float yaw = std::atan2(siny_cosp, cosy_cosp);

    return {roll * kRadToDeg, pitch * kRadToDeg, yaw * kRadToDeg};
}

matter::Quaternion euler_degrees_to_quat(const matter::Float3& euler_deg) {
    const float roll = euler_deg.x * kDegToRad;
    const float pitch = euler_deg.y * kDegToRad;
    const float yaw = euler_deg.z * kDegToRad;

    const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);

    matter::Quaternion q;
    q.w = cr * cp * cy + sr * sp * sy;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    return q;
}

// ---------------------------------------------------------------------------
// Cache key: component.field@primary_entity_id — the primary (first) id of
// the current selection namespaces the cache entry so switching selection
// doesn't briefly show a stale value from a previously selected entity of a
// different kind.
// ---------------------------------------------------------------------------

std::string make_cache_key(const char* component, const char* field, uint64_t primary_id) {
    std::string key;
    key.reserve(64);
    key += component ? component : "";
    key += '.';
    key += field ? field : "";
    key += '@';
    key += std::to_string(primary_id);
    return key;
}

// Known enum field -> option label table. Only RigidBody.type exists today;
// anything else falls back to a numeric drag box in draw_enum_field.
bool enum_options_for(const char* component, const char* field,
                      std::vector<const char*>& out) {
    if (component && field && std::strcmp(component, "RigidBody") == 0 &&
        std::strcmp(field, "type") == 0) {
        out = {"Static", "Kinematic", "Dynamic"};
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-WidgetKind field renderers. Each reads (when `live`) the primary
// entity's value plus a mixed-value flag across the rest of the selection,
// caches it, then draws the ImGui widget from the cache. Editing always
// fans the new value out to every selected entity.
// ---------------------------------------------------------------------------

void draw_float_field(PropertiesPanelState& state, const FieldCommands& fields,
                      const std::vector<SceneEntityId>& ids, const char* component,
                      const FieldWidget& fw, bool is_slider, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        float value = 0.0f;
        if (!fields.get_float || !fields.get_float(ids[0], component, fw.name, value)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                float v = 0.0f;
                if (fields.get_float(ids[k], component, fw.name, v) &&
                    std::fabs(v - value) > 1e-4f) mixed = true;
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.f = value;
        }
    }
    if (!cache.valid) return;

    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (mixed)" : "%s", fw.name);
    float value = cache.f;
    const bool changed = is_slider
        ? ImGui::SliderFloat(label, &value, fw.range_min, fw.range_max)
        : ImGui::DragFloat(label, &value, 0.05f);
    if (changed) {
        cache.f = value;
        cache.mixed = false;
        if (fields.set_float)
            for (auto id : ids) fields.set_float(id, component, fw.name, value);
    }
}

void draw_int_field(PropertiesPanelState& state, const FieldCommands& fields,
                    const std::vector<SceneEntityId>& ids, const char* component,
                    const FieldWidget& fw, bool is_slider, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        int value = 0;
        if (!fields.get_int || !fields.get_int(ids[0], component, fw.name, value)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                int v = 0;
                if (fields.get_int(ids[k], component, fw.name, v) && v != value) mixed = true;
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.i = value;
        }
    }
    if (!cache.valid) return;

    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (mixed)" : "%s", fw.name);
    int value = cache.i;
    const bool changed = is_slider
        ? ImGui::SliderInt(label, &value, static_cast<int>(fw.range_min), static_cast<int>(fw.range_max))
        : ImGui::DragInt(label, &value);
    if (changed) {
        cache.i = value;
        cache.mixed = false;
        if (fields.set_int)
            for (auto id : ids) fields.set_int(id, component, fw.name, value);
    }
}

void draw_enum_field(PropertiesPanelState& state, const FieldCommands& fields,
                     const std::vector<SceneEntityId>& ids, const char* component,
                     const FieldWidget& fw, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        int value = 0;
        if (!fields.get_int || !fields.get_int(ids[0], component, fw.name, value)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                int v = 0;
                if (fields.get_int(ids[k], component, fw.name, v) && v != value) mixed = true;
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.i = value;
        }
    }
    if (!cache.valid) return;

    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (mixed)" : "%s", fw.name);
    int value = cache.i;
    std::vector<const char*> options;
    bool changed;
    if (enum_options_for(component, fw.name, options)) {
        changed = ImGui::Combo(label, &value, options.data(), static_cast<int>(options.size()));
    } else {
        changed = ImGui::DragInt(label, &value, 1.0f,
                                 static_cast<int>(fw.range_min), static_cast<int>(fw.range_max));
    }
    if (changed) {
        cache.i = value;
        cache.mixed = false;
        if (fields.set_int)
            for (auto id : ids) fields.set_int(id, component, fw.name, value);
    }
}

void draw_uint_field(PropertiesPanelState& state, const FieldCommands& fields,
                     const std::vector<SceneEntityId>& ids, const char* component,
                     const FieldWidget& fw, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        uint32_t value = 0;
        if (!fields.get_uint || !fields.get_uint(ids[0], component, fw.name, value)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                uint32_t v = 0;
                if (fields.get_uint(ids[k], component, fw.name, v) && v != value) mixed = true;
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.u = value;
        }
    }
    if (!cache.valid) return;

    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (mixed)" : "%s", fw.name);
    uint32_t value = cache.u;
    const bool changed = ImGui::DragScalar(label, ImGuiDataType_U32, &value, 1.0f);
    if (changed) {
        cache.u = value;
        cache.mixed = false;
        if (fields.set_uint)
            for (auto id : ids) fields.set_uint(id, component, fw.name, value);
    }
}

void draw_bool_field(PropertiesPanelState& state, const FieldCommands& fields,
                     const std::vector<SceneEntityId>& ids, const char* component,
                     const FieldWidget& fw, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        bool value = false;
        if (!fields.get_bool || !fields.get_bool(ids[0], component, fw.name, value)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                bool v = false;
                if (fields.get_bool(ids[k], component, fw.name, v) && v != value) mixed = true;
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.b = value;
        }
    }
    if (!cache.valid) return;

    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (mixed)" : "%s", fw.name);
    bool value = cache.b;
    const bool changed = ImGui::Checkbox(label, &value);
    if (changed) {
        cache.b = value;
        cache.mixed = false;
        if (fields.set_bool)
            for (auto id : ids) fields.set_bool(id, component, fw.name, value);
    }
}

void draw_float3_field(PropertiesPanelState& state, const FieldCommands& fields,
                       const std::vector<SceneEntityId>& ids, const char* component,
                       const FieldWidget& fw, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        matter::Float3 value{};
        if (!fields.get_float3 || !fields.get_float3(ids[0], component, fw.name, value)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                matter::Float3 v{};
                if (fields.get_float3(ids[k], component, fw.name, v) &&
                    (std::fabs(v.x - value.x) > 1e-4f || std::fabs(v.y - value.y) > 1e-4f ||
                     std::fabs(v.z - value.z) > 1e-4f)) {
                    mixed = true;
                }
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.f3 = value;
        }
    }
    if (!cache.valid) return;

    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (mixed)" : "%s", fw.name);
    matter::Float3 value = cache.f3;
    const bool changed = ImGui::DragFloat3(label, &value.x, 0.05f);
    if (changed) {
        cache.f3 = value;
        cache.mixed = false;
        if (fields.set_float3)
            for (auto id : ids) fields.set_float3(id, component, fw.name, value);
    }
}

void draw_quat_field(PropertiesPanelState& state, const FieldCommands& fields,
                     const std::vector<SceneEntityId>& ids, const char* component,
                     const FieldWidget& fw, bool live) {
    CachedFieldValue& cache = state.cache[make_cache_key(component, fw.name, ids[0].value)];
    if (live) {
        matter::Quaternion q{};
        if (!fields.get_quat || !fields.get_quat(ids[0], component, fw.name, q)) {
            cache.valid = false;
        } else {
            bool mixed = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                matter::Quaternion v{};
                if (fields.get_quat(ids[k], component, fw.name, v) &&
                    (std::fabs(v.x - q.x) > 1e-4f || std::fabs(v.y - q.y) > 1e-4f ||
                     std::fabs(v.z - q.z) > 1e-4f || std::fabs(v.w - q.w) > 1e-4f)) {
                    mixed = true;
                }
            }
            cache.valid = true;
            cache.mixed = mixed;
            cache.q = q;
        }
    }
    if (!cache.valid) return;

    matter::Float3 euler = quat_to_euler_degrees(cache.q);
    char label[160];
    std::snprintf(label, sizeof(label), cache.mixed ? "%s (deg, mixed)" : "%s (deg)", fw.name);
    const bool changed = ImGui::DragFloat3(label, &euler.x, 0.5f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("raw quat: x=%.3f y=%.3f z=%.3f w=%.3f",
                          cache.q.x, cache.q.y, cache.q.z, cache.q.w);
    }
    if (changed) {
        const matter::Quaternion new_q = euler_degrees_to_quat(euler);
        cache.q = new_q;
        cache.mixed = false;
        if (fields.set_quat)
            for (auto id : ids) fields.set_quat(id, component, fw.name, new_q);
    }
}

void draw_field(PropertiesPanelState& state, const FieldCommands& fields,
                const std::vector<SceneEntityId>& ids, const char* component,
                const FieldWidget& fw, bool live) {
    ImGui::PushID(fw.name);
    switch (fw.kind) {
        case WidgetKind::FloatSlider:
            draw_float_field(state, fields, ids, component, fw, true, live);
            break;
        case WidgetKind::FloatDrag:
            draw_float_field(state, fields, ids, component, fw, false, live);
            break;
        case WidgetKind::IntSlider:
            draw_int_field(state, fields, ids, component, fw, true, live);
            break;
        case WidgetKind::IntDrag:
            draw_int_field(state, fields, ids, component, fw, false, live);
            break;
        case WidgetKind::UIntDrag:
            draw_uint_field(state, fields, ids, component, fw, live);
            break;
        case WidgetKind::Checkbox:
            draw_bool_field(state, fields, ids, component, fw, live);
            break;
        case WidgetKind::EnumDropdown:
            draw_enum_field(state, fields, ids, component, fw, live);
            break;
        case WidgetKind::Float3Editor:
            draw_float3_field(state, fields, ids, component, fw, live);
            break;
        case WidgetKind::QuaternionEditor:
            draw_quat_field(state, fields, ids, component, fw, live);
            break;
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Task 8 — Specialized editors. Rendered inline, after a component's
// auto-generated fields, inside the same CollapsingHeader. Only reached for
// the three ComponentKinds where SpecializedEditors::has_specialized_editor()
// is true. Multi-select: every action button fans out to every id in `ids`.
// ---------------------------------------------------------------------------

void draw_part_instance_editor(SpecializedEditors& specialized, const FieldCommands& fields,
                               const std::vector<SceneEntityId>& ids) {
    PartEditorCommands& part_cmds = specialized.part_commands();
    PartPickerState& picker = specialized.part_picker_state();

    std::vector<std::pair<uint64_t, std::string>> available;
    if (part_cmds.list_available_parts) available = part_cmds.list_available_parts();

    uint32_t current_hash = 0;
    const bool have_hash = fields.get_uint &&
        fields.get_uint(ids[0], "PartInstance", "part_hash", current_hash);

    std::string current_name = "(unknown)";
    if (have_hash) {
        current_name.clear();
        for (const auto& p : available) {
            if (p.first == static_cast<uint64_t>(current_hash)) {
                current_name = p.second;
                break;
            }
        }
        if (current_name.empty()) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%08X", current_hash);
            current_name = buf;
        }
    }

    ImGui::Text("Part: %s", current_name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick Part...")) {
        picker.picker_open = true;
        ImGui::OpenPopup("PartPickerPopup");
    }

    if (ImGui::BeginPopup("PartPickerPopup")) {
        static char filter[128] = "";
        ImGui::InputTextWithHint("##part_filter", "Search parts...", filter, sizeof(filter));
        ImGui::Separator();
        if (available.empty()) {
            ImGui::TextDisabled("No parts available");
        }
        for (const auto& part : available) {
            if (filter[0] != '\0' &&
                part.second.find(filter) == std::string::npos) continue;
            const bool selected = have_hash && part.first == static_cast<uint64_t>(current_hash);
            char label[192];
            std::snprintf(label, sizeof(label), "%s##part_%llu", part.second.c_str(),
                         static_cast<unsigned long long>(part.first));
            if (ImGui::Selectable(label, selected)) {
                picker.selected_hash = part.first;
                if (part_cmds.assign_part)
                    for (auto id : ids) part_cmds.assign_part(id, part.first);
                picker.picker_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    } else {
        picker.picker_open = false;
    }
}

void draw_rigidbody_editor(SpecializedEditors& specialized,
                           const std::vector<SceneEntityId>& ids,
                           const matter::Float3& camera_position) {
    PhysicsEditorCommands& phys = specialized.physics_commands();

    ImGui::Spacing();
    ImGui::TextDisabled("Actions");

    if (ImGui::Button("Wake") && phys.wake) {
        for (auto id : ids) phys.wake(id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Teleport To Camera") && phys.teleport) {
        for (auto id : ids) phys.teleport(id, camera_position);
    }

    static matter::Float3 impulse{0.0f, 0.0f, 0.0f};
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::DragFloat3("##impulse", &impulse.x, 0.1f);
    ImGui::SameLine();
    if (ImGui::Button("Apply Impulse") && phys.apply_impulse) {
        for (auto id : ids) phys.apply_impulse(id, impulse);
    }

    static matter::Float3 target_velocity{0.0f, 0.0f, 0.0f};
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::DragFloat3("##set_velocity", &target_velocity.x, 0.1f);
    ImGui::SameLine();
    if (ImGui::Button("Set Velocity") && phys.set_linear_velocity) {
        for (auto id : ids) phys.set_linear_velocity(id, target_velocity);
    }
}

void draw_streaming_editor(SpecializedEditors& specialized,
                           const std::vector<SceneEntityId>& ids) {
    StreamingEditorCommands& stream_cmds = specialized.streaming_commands();
    StreamingEditorState& stream_state = specialized.streaming_state();

    ImGui::Spacing();
    if (ImGui::Button("Remove Streaming") && stream_cmds.remove_streaming) {
        for (auto id : ids) stream_cmds.remove_streaming(id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Attach Streaming") && stream_cmds.attach_streaming) {
        for (auto id : ids) stream_cmds.attach_streaming(id);
    }

    ImGui::DragFloat("Radius", &stream_state.radius, 1.0f, 0.0f, 100000.0f);

    bool follow = stream_state.follow_camera;
    if (ImGui::Checkbox("Follow Camera", &follow)) {
        stream_state.follow_camera = follow;
        if (stream_cmds.set_follow_camera) stream_cmds.set_follow_camera(follow);
    }

    uint32_t seed = static_cast<uint32_t>(stream_state.seed);
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::DragScalar("Seed", ImGuiDataType_U32, &seed, 1.0f)) {
        stream_state.seed = seed;
    }
    ImGui::SameLine();
    if (ImGui::Button("Regenerate") && stream_cmds.regenerate) {
        stream_cmds.regenerate(stream_state.seed);
    }
}

void draw_specialized_editor(SpecializedEditors& specialized, const FieldCommands& fields,
                             const std::vector<SceneEntityId>& ids,
                             matter::scene::ComponentKind kind,
                             const matter::Float3& camera_position) {
    using matter::scene::ComponentKind;
    switch (kind) {
        case ComponentKind::PartInstance:
            draw_part_instance_editor(specialized, fields, ids);
            break;
        case ComponentKind::RigidBody:
            draw_rigidbody_editor(specialized, ids, camera_position);
            break;
        case ComponentKind::SectorStreaming:
            draw_streaming_editor(specialized, ids);
            break;
        default:
            break;
    }
}

void draw_add_component_footer(const PropertiesRegistry& registry,
                               const std::vector<SceneEntityId>& ids,
                               const std::vector<const HierarchyRow*>& rows,
                               const ComponentCommands& components) {
    ImGui::Separator();
    if (ImGui::Button("+ Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    if (ImGui::BeginPopup("AddComponentPopup")) {
        matter::scene::SceneRecord probe;
        probe.component_names = rows[0]->component_names;
        const auto addable = registry.addable_components(probe);
        bool any = false;
        for (const auto* entry : addable) {
            bool present_elsewhere = false;
            for (size_t i = 1; i < rows.size() && !present_elsewhere; ++i) {
                for (const auto& n : rows[i]->component_names) {
                    if (n == entry->name) { present_elsewhere = true; break; }
                }
            }
            if (present_elsewhere) continue;
            any = true;
            if (ImGui::MenuItem(entry->name) && components.add_component) {
                for (auto id : ids) components.add_component(id, entry->name);
            }
        }
        if (!any) ImGui::TextDisabled("No components to add");
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Task 9 — Baked root properties. BakedRoot selections carry a resolved part
// hash in SelectedObject::id (see viewport_pick.cpp — best_object.id =
// info.part_hash), which matches part_graph_snapshot::Node::resolved_hash.
// The snapshot's `nodes` map is keyed by module name, not hash, so we do a
// linear scan for the matching node.
// ---------------------------------------------------------------------------

const part_graph_snapshot::Node* find_node_by_hash(
    const part_graph_snapshot::Snapshot& snapshot, uint64_t hash) {
    for (const auto& kv : snapshot.nodes) {
        if (kv.second.resolved_hash == hash) return &kv.second;
    }
    return nullptr;
}

void draw_baked_root_card(const SelectedObject& obj,
                          const part_graph_snapshot::Snapshot* snapshot) {
    if (!snapshot) {
        ImGui::TextDisabled("No part graph snapshot available yet");
        return;
    }
    const part_graph_snapshot::Node* node = find_node_by_hash(*snapshot, obj.id);
    if (!node) {
        ImGui::TextDisabled("Baked root 0x%016llX not found in part graph",
                            (unsigned long long)obj.id);
        return;
    }

    ImGui::Text("Baked Root");
    ImGui::Separator();

    ImGui::TextDisabled("Module");
    ImGui::TextWrapped("%s", node->module.c_str());

    ImGui::TextDisabled("Source Path");
    ImGui::TextWrapped("%s", node->source_path.c_str());

    ImGui::TextDisabled("Resolved Hash");
    ImGui::Text("0x%016llX", (unsigned long long)node->resolved_hash);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%016llX",
                     (unsigned long long)node->resolved_hash);
        ImGui::SetClipboardText(buf);
    }

    ImGui::TextDisabled("Child Count");
    ImGui::Text("%d", static_cast<int>(node->children.size()));

    if (ImGui::CollapsingHeader("Parameters (JSON)")) {
        if (node->params_json.empty()) {
            ImGui::TextDisabled("(none)");
        } else {
            ImGui::TextWrapped("%s", node->params_json.c_str());
        }
    }

    ImGui::Separator();
    if (!node->source_path.empty()) {
        if (ImGui::Button("Open Source")) {
            os_open_file(node->source_path);
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Open Source");
        ImGui::EndDisabled();
    }
}

} // namespace

void draw_properties_contents(PropertiesPanelState& state,
                              const SelectionSet& selection, EditorModel& editor,
                              const PropertiesRegistry& registry,
                              const FieldCommands& fields,
                              const ComponentCommands& components,
                              matter::scene::SimulationMode mode,
                              const part_graph_snapshot::Snapshot* snapshot,
                              SpecializedEditors& specialized,
                              const matter::Float3& camera_position) {
    if (selection.empty()) {
        ImGui::TextDisabled("No selection");
        return;
    }

    bool has_entity = false;
    bool has_baked_root = false;
    for (const auto& obj : selection.items()) {
        if (obj.kind == SelectedObject::BakedRoot) has_baked_root = true;
        else has_entity = true;
    }

    if (has_baked_root && has_entity) {
        ImGui::TextDisabled("Mixed selection (entities + roots)");
        return;
    }

    if (has_baked_root) {
        if (selection.items().size() > 1) {
            ImGui::Text("%d baked roots selected",
                       static_cast<int>(selection.items().size()));
            ImGui::Separator();
        }
        for (const auto& obj : selection.items()) {
            ImGui::PushID(static_cast<int>(obj.id));
            draw_baked_root_card(obj, snapshot);
            ImGui::PopID();
            ImGui::Spacing();
        }
        return;
    }

    std::vector<SceneEntityId> ids;
    std::vector<const HierarchyRow*> rows;
    const auto& all_rows = editor.rows();
    for (const auto& obj : selection.items()) {
        if (obj.kind != SelectedObject::Entity) continue;
        for (const auto& row : all_rows) {
            if (row.id.value == obj.id) {
                ids.push_back(row.id);
                rows.push_back(&row);
                break;
            }
        }
    }

    if (ids.empty()) {
        ImGui::TextDisabled("Selection has no editable entities");
        return;
    }

    // Field values are only re-read from the ECS when not Playing — while
    // Playing the last values read before Play started stay pinned (and the
    // widgets are disabled below, so editing is impossible regardless).
    const bool live = (mode != matter::scene::SimulationMode::Play);
    const bool disabled = !live;

    if (rows.size() == 1) {
        ImGui::Text("%s", rows[0]->name.c_str());
        ImGui::TextDisabled("ID: 0x%llX", static_cast<unsigned long long>(ids[0].value));
    } else {
        ImGui::Text("%d entities selected", static_cast<int>(rows.size()));
    }
    ImGui::Separator();

    if (disabled) ImGui::BeginDisabled();

    for (const auto& entry : registry.entries()) {
        bool present_in_all = true;
        if (entry.kind != matter::scene::ComponentKind::Transform) {
            for (const auto* row : rows) {
                bool found = false;
                for (const auto& n : row->component_names) {
                    if (n == entry.name) { found = true; break; }
                }
                if (!found) { present_in_all = false; break; }
            }
        }
        if (!present_in_all) continue;

        ImGui::PushID(entry.name);
        const bool open = ImGui::CollapsingHeader(entry.name, ImGuiTreeNodeFlags_DefaultOpen);
        if (entry.user_removable) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 32.0f);
            if (ImGui::SmallButton("x") && components.remove_component) {
                for (auto id : ids) components.remove_component(id, entry.name);
            }
        }
        if (open) {
            ImGui::Indent();
            for (const auto& fw : entry.fields) {
                draw_field(state, fields, ids, entry.name, fw, live);
            }
            if (specialized.has_specialized_editor(entry.kind)) {
                draw_specialized_editor(specialized, fields, ids, entry.kind, camera_position);
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    if (disabled) ImGui::EndDisabled();

    if (!disabled) {
        draw_add_component_footer(registry, ids, rows, components);
    }

    if (mode == matter::scene::SimulationMode::Pause && !state.pause_edit_hint_shown) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::TextWrapped("Note: changes made in Pause mode are discarded on Stop.");
        ImGui::PopStyleColor();
        state.pause_edit_hint_shown = true;
    }
}

} // namespace viewer
