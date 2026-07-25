#pragma once

#include "matter/scene.h"
#include "ecs/scene_registry.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace viewer {

// Widget kind for a field in the Properties panel.
enum class WidgetKind : uint8_t {
    FloatSlider,      // float with range
    FloatDrag,        // float without range
    IntSlider,        // int with range
    IntDrag,          // int without range
    UIntDrag,         // unsigned int
    Checkbox,         // bool
    EnumDropdown,     // enum
    Float3Editor,     // Float3 (translation/scale/etc)
    QuaternionEditor  // Quaternion (rotation)
};

struct FieldWidget {
    const char* name = nullptr;
    WidgetKind kind = WidgetKind::FloatDrag;
    float range_min = 0.0f;
    float range_max = 0.0f;
    bool has_range = false;
};

struct ComponentEntry {
    const char* name = nullptr;
    matter::scene::ComponentKind kind{};
    std::vector<FieldWidget> fields;
    bool user_addable = true;     // can user add this from Add Component menu?
    bool user_removable = true;   // can user remove this?
};

// Callback interface for component add/remove mutations.
struct ComponentCommands {
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId, const char* component_name)> add_component;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId, const char* component_name)> remove_component;
};

class PropertiesRegistry {
public:
    PropertiesRegistry();

    // Get all registered component entries.
    const std::vector<ComponentEntry>& entries() const { return entries_; }

    // Get the entry for a given component name (or nullptr).
    const ComponentEntry* find(const char* name) const;

    // Get the list of components that can be added to an entity (user_addable=true).
    // Excludes components already present (per the provided record's component_names).
    std::vector<const ComponentEntry*> addable_components(
        const matter::scene::SceneRecord& record) const;

    // Map a FieldType to its WidgetKind based on range.
    static WidgetKind widget_for_field(matter::scene::FieldType type, bool has_range);

private:
    std::vector<ComponentEntry> entries_;
};

} // namespace viewer
