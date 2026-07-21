// Phase 4 Task 10 — PropertiesRegistry: maps ComponentDescriptor field types
// to widget kinds, determines which components are user-editable (vs
// internal), and provides add/remove component logic through callbacks.
// Logic-only layer: no ImGui code, no rendering.
#include "properties_registry.h"

#include "ecs/scene_registry.h"

#include <cstring>

namespace viewer {

using matter::scene::ComponentDescriptor;
using matter::scene::FieldDescriptor;
using matter::scene::FieldType;

WidgetKind PropertiesRegistry::widget_for_field(FieldType type, bool has_range) {
    switch (type) {
        case FieldType::Float:
            return has_range ? WidgetKind::FloatSlider : WidgetKind::FloatDrag;
        case FieldType::Int:
            return has_range ? WidgetKind::IntSlider : WidgetKind::IntDrag;
        case FieldType::UInt:
            return WidgetKind::UIntDrag;
        case FieldType::Bool:
            return WidgetKind::Checkbox;
        case FieldType::Enum:
            return WidgetKind::EnumDropdown;
        case FieldType::Float3:
            return WidgetKind::Float3Editor;
        case FieldType::Quaternion:
            return WidgetKind::QuaternionEditor;
    }
    return WidgetKind::FloatDrag;
}

PropertiesRegistry::PropertiesRegistry() {
    uint32_t count = matter::scene::component_count();
    entries_.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const ComponentDescriptor* desc = matter::scene::component_at(i);
        if (!desc) continue;

        ComponentEntry entry;
        entry.name = desc->name;
        entry.kind = desc->kind;
        entry.fields.reserve(desc->field_count);
        for (uint32_t f = 0; f < desc->field_count; ++f) {
            const FieldDescriptor& fd = desc->fields[f];
            FieldWidget widget;
            widget.name = fd.name;
            widget.kind = widget_for_field(fd.type, fd.has_range);
            widget.range_min = fd.range_min;
            widget.range_max = fd.range_max;
            widget.has_range = fd.has_range;
            entry.fields.push_back(widget);
        }

        // LocalTransform (Transform) is always present on scene entities and
        // cannot be added or removed by the user.
        if (desc->kind == matter::scene::ComponentKind::Transform) {
            entry.user_addable = false;
            entry.user_removable = false;
        } else {
            entry.user_addable = true;
            entry.user_removable = true;
        }

        entries_.push_back(entry);
    }
}

const ComponentEntry* PropertiesRegistry::find(const char* name) const {
    if (!name) return nullptr;
    for (const auto& entry : entries_) {
        if (entry.name && std::strcmp(entry.name, name) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const ComponentEntry*> PropertiesRegistry::addable_components(
    const matter::scene::SceneRecord& record) const {
    std::vector<const ComponentEntry*> result;
    for (const auto& entry : entries_) {
        if (!entry.user_addable) continue;

        bool present = false;
        for (const auto& name : record.component_names) {
            if (entry.name && name == entry.name) {
                present = true;
                break;
            }
        }
        if (present) continue;

        result.push_back(&entry);
    }
    return result;
}

} // namespace viewer
