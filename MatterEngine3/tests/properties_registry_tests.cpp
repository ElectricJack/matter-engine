// Headless tests for viewer::PropertiesRegistry (Phase 4 Task 10 — generic
// entities and properties UI logic layer). Pure CPU logic: exercises the
// static ComponentDescriptor table via scene_registry, no ImGui, no
// rendering. Run via `make run-properties-registry`.
#include "../../MatterEditor/src/properties_registry.h"

#include "ecs/scene_registry.h"

#include <cstdio>
#include <string>
#include <vector>

#include "check.h"

using matter::scene::FieldType;
using matter::scene::SceneEntityId;
using matter::scene::SceneRecord;
using viewer::ComponentEntry;
using viewer::PropertiesRegistry;
using viewer::WidgetKind;

namespace {

void test_registry_has_entries() {
    PropertiesRegistry registry;
    CHECK(!registry.entries().empty(), "registry_has_entries: expected non-empty entries");
    CHECK(registry.entries().size() == matter::scene::component_count(),
          "registry_has_entries: entry count should match scene::component_count()");
}

void test_widget_for_float_with_range() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Float, true) == WidgetKind::FloatSlider,
          "widget_for_float_with_range: expected FloatSlider");
}

void test_widget_for_float_no_range() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Float, false) == WidgetKind::FloatDrag,
          "widget_for_float_no_range: expected FloatDrag");
}

void test_widget_for_int_with_range() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Int, true) == WidgetKind::IntSlider,
          "widget_for_int_with_range: expected IntSlider");
}

void test_widget_for_bool() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Bool, false) == WidgetKind::Checkbox,
          "widget_for_bool: expected Checkbox");
}

void test_widget_for_enum() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Enum, false) == WidgetKind::EnumDropdown,
          "widget_for_enum: expected EnumDropdown");
}

void test_widget_for_float3() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Float3, false) == WidgetKind::Float3Editor,
          "widget_for_float3: expected Float3Editor");
}

void test_widget_for_quaternion() {
    CHECK(PropertiesRegistry::widget_for_field(FieldType::Quaternion, false) == WidgetKind::QuaternionEditor,
          "widget_for_quaternion: expected QuaternionEditor");
}

void test_transform_not_addable() {
    PropertiesRegistry registry;
    const ComponentEntry* entry = registry.find("LocalTransform");
    CHECK(entry != nullptr, "transform_not_addable: expected LocalTransform entry to exist");
    if (entry) {
        CHECK(!entry->user_addable, "transform_not_addable: expected user_addable == false");
        CHECK(!entry->user_removable, "transform_not_addable: expected user_removable == false");
    }
}

void test_addable_excludes_present() {
    PropertiesRegistry registry;
    SceneRecord record;
    record.id = SceneEntityId{1};
    record.component_names = {"RigidBody"};

    std::vector<const ComponentEntry*> addable = registry.addable_components(record);
    bool found_rigidbody = false;
    for (const auto* entry : addable) {
        if (entry->name && std::string(entry->name) == "RigidBody") {
            found_rigidbody = true;
        }
    }
    CHECK(!found_rigidbody, "addable_excludes_present: RigidBody should not be in addable list");
}

// Property-system spec S7: enum option labels reach the panel through the
// FieldWidget instead of the hardcoded enum_options_for table that used to
// live in properties_panel.cpp.
void test_enum_labels_flow_from_descriptor() {
    PropertiesRegistry registry;
    const ComponentEntry* entry = registry.find("RigidBody");
    CHECK(entry != nullptr, "enum_labels_flow: expected RigidBody entry");
    if (!entry) return;

    const viewer::FieldWidget* type_widget = nullptr;
    for (const auto& fw : entry->fields) {
        if (fw.name && std::string(fw.name) == "type") type_widget = &fw;
    }
    CHECK(type_widget != nullptr, "enum_labels_flow: expected a 'type' field widget");
    if (!type_widget) return;

    CHECK(type_widget->kind == WidgetKind::EnumDropdown,
          "enum_labels_flow: RigidBody.type should be an EnumDropdown");
    CHECK(type_widget->enum_count == 3, "enum_labels_flow: expected 3 labels");
    CHECK(type_widget->enum_labels != nullptr, "enum_labels_flow: expected non-null labels");
    if (type_widget->enum_labels && type_widget->enum_count == 3) {
        CHECK(std::string(type_widget->enum_labels[0]) == "Static", "label 0 should be Static");
        CHECK(std::string(type_widget->enum_labels[1]) == "Kinematic", "label 1 should be Kinematic");
        CHECK(std::string(type_widget->enum_labels[2]) == "Dynamic", "label 2 should be Dynamic");
    }

    // Non-enum fields carry no labels, so the panel keeps its numeric widgets.
    for (const auto& fw : entry->fields) {
        if (fw.kind != WidgetKind::EnumDropdown)
            CHECK(fw.enum_labels == nullptr, "enum_labels_flow: non-enum field carries labels");
    }
}

void test_find_unknown_returns_null() {
    PropertiesRegistry registry;
    CHECK(registry.find("NonExistent") == nullptr,
          "find_unknown_returns_null: expected nullptr for unknown component");
}

} // namespace

int main() {
    test_registry_has_entries();
    test_widget_for_float_with_range();
    test_widget_for_float_no_range();
    test_widget_for_int_with_range();
    test_widget_for_bool();
    test_widget_for_enum();
    test_widget_for_float3();
    test_widget_for_quaternion();
    test_transform_not_addable();
    test_addable_excludes_present();
    test_enum_labels_flow_from_descriptor();
    test_find_unknown_returns_null();

    return check_summary();
}
