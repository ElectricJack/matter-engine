#pragma once

// MatterEditor/src/properties_registry.h
//
// The ECS-side Properties panel schema: a flattened, ImGui-ready view of the
// engine's component registry (`ecs/scene_registry.h`). For every registered
// ComponentDescriptor it records the component's name and kind, one
// FieldWidget per field, and whether the user may add or remove it from an
// entity.
//
// How it fits. `properties_panel.cpp` walks `PropertiesRegistry::entries()` to
// auto-generate the widget rows for the selected entity, and calls
// `addable_components()` to populate the "Add Component" menu; the actual
// reads and writes go through the `FieldCommands` / `ComponentCommands`
// callbacks main.cpp binds to the live session's ECS. This is a logic-only
// layer — no ImGui, no rendering — which is what lets
// `MatterEngine3/tests/properties_registry_tests.cpp` exercise it headlessly.
//
// Not to be confused with `property_editor.h` / `matter::props`, the OTHER
// property system in this editor. That one describes engine SETTINGS groups
// ("render.fog", "draw.overrides") and is what the Tunables panel and the FIFO
// `set` command drive. This file only describes ECS components on scene
// entities; the two share no types.
//
// Lifetime and threading. main.cpp constructs one `PropertiesRegistry` on the
// app thread at startup and it lives for the process. It is a SNAPSHOT of the
// component registry taken in the constructor, so a component type registered
// afterwards will not appear in it. Every `const char*` here — component
// names, field names, enum labels, doc strings — points into the descriptors'
// static string tables, which outlive the registry; nothing in this file owns
// a string. The registry is immutable after construction, so the pointers
// `find()` and `addable_components()` hand out stay valid for its whole life.

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

// One editable field of a component, resolved down to what the panel needs in
// order to draw it. Copied field-by-field off the ECS `FieldDescriptor` at
// construction; the descriptor remains the source of truth for the offset and
// type used to actually read and write the value, so this struct carries
// presentation only and no way to reach the data.
struct FieldWidget {
    const char* name = nullptr;
    WidgetKind kind = WidgetKind::FloatDrag;
    float range_min = 0.0f;  // slider low end; meaningful only when has_range
    float range_max = 0.0f;  // slider high end; meaningful only when has_range
    bool has_range = false;
    // Enum option labels, copied straight off the ECS FieldDescriptor
    // (property-system spec S7 — they used to live in a hardcoded table in
    // properties_panel.cpp). Null/0 for non-enum fields, and for an enum the
    // schema has not labelled: the panel then falls back to a numeric drag.
    // Points into the descriptor's static string table, which outlives the
    // registry.
    const char* const* enum_labels = nullptr;
    uint32_t enum_count = 0;
    const char* doc = nullptr;  // tooltip text (null when undocumented)
};

// One registered component type, with its fields in descriptor order — which
// is also the order the panel draws them in. `user_addable` / `user_removable`
// are false only for `ComponentKind::Transform`: LocalTransform is always
// present on a scene entity and cannot be added or removed
// (properties_registry.cpp). Everything else registered today is both.
struct ComponentEntry {
    const char* name = nullptr;
    matter::scene::ComponentKind kind{};
    std::vector<FieldWidget> fields;
    bool user_addable = true;     // can user add this from Add Component menu?
    bool user_removable = true;   // can user remove this?
};

// Callback interface for component add/remove mutations.
//
// main.cpp binds these to the live session's ECS. Either std::function may be
// empty (no world connected, or a panel driven in a test), so the caller must
// test before invoking. `component_name` is a registered
// `ComponentEntry::name`, not a display label. The returned SceneEditResult
// carries a SceneEditError that the panel surfaces to the user rather than
// asserting on.
struct ComponentCommands {
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId, const char* component_name)> add_component;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId, const char* component_name)> remove_component;
};

// The flattened component schema, built entirely in the constructor by walking
// `matter::scene::component_count()` / `component_at()`. Immutable afterwards:
// there is no register/unregister API, so `entries_` never reallocates under a
// pointer a caller is holding and a `const ComponentEntry*` obtained from
// `find()` or `addable_components()` remains valid for the registry's life.
class PropertiesRegistry {
public:
    PropertiesRegistry();

    // Get all registered component entries.
    const std::vector<ComponentEntry>& entries() const { return entries_; }

    // Get the entry for a given component name (or nullptr).
    // Linear `strcmp` scan over `entries()`; a null `name` yields nullptr
    // rather than matching anything.
    const ComponentEntry* find(const char* name) const;

    // Get the list of components that can be added to an entity (user_addable=true).
    // Excludes components already present (per the provided record's component_names).
    // Allocates a fresh vector on every call and is O(entries x
    // record.component_names), so call it when the menu opens, not per frame.
    // The pointers alias into this registry and are valid as long as it is.
    std::vector<const ComponentEntry*> addable_components(
        const matter::scene::SceneRecord& record) const;

    // Map a FieldType to its WidgetKind based on range.
    // `has_range` only splits Float and Int into slider-vs-drag: UInt is always
    // a drag, and Bool / Enum / Float3 / Quaternion ignore it entirely. An
    // unrecognized type falls back to FloatDrag rather than failing.
    static WidgetKind widget_for_field(matter::scene::FieldType type, bool has_range);

private:
    std::vector<ComponentEntry> entries_;
};

} // namespace viewer
