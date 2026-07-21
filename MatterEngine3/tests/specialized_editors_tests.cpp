// Headless tests for viewer::SpecializedEditors (Phase 4 Task 12 —
// specialized component editors + sector-panel retirement). Pure CPU logic:
// no ImGui, no rendering. Run via `make run-specialized-editors`.
#include "../../MatterViewer/specialized_editors.h"

#include "ecs/scene_registry.h"

#include <cstdio>

#include "check.h"

using matter::scene::ComponentKind;
using viewer::PartEditorCommands;
using viewer::PartPickerState;
using viewer::PhysicsEditorCommands;
using viewer::SpecializedEditors;
using viewer::StreamingEditorCommands;
using viewer::StreamingEditorState;

namespace {

void test_has_specialized_editor_true_for_part_instance() {
    SpecializedEditors editors;
    CHECK(editors.has_specialized_editor(ComponentKind::PartInstance),
          "has_specialized_editor_true_for_part_instance: expected true");
}

void test_has_specialized_editor_true_for_physics() {
    SpecializedEditors editors;
    CHECK(editors.has_specialized_editor(ComponentKind::RigidBody),
          "has_specialized_editor_true_for_physics: expected true");
}

void test_has_specialized_editor_true_for_streaming() {
    SpecializedEditors editors;
    CHECK(editors.has_specialized_editor(ComponentKind::SectorStreaming),
          "has_specialized_editor_true_for_streaming: expected true");
}

void test_has_specialized_editor_false_for_transform() {
    SpecializedEditors editors;
    CHECK(!editors.has_specialized_editor(ComponentKind::Transform),
          "has_specialized_editor_false_for_transform: expected false");
}

void test_has_specialized_editor_false_for_generic_kinds() {
    SpecializedEditors editors;
    CHECK(!editors.has_specialized_editor(ComponentKind::Velocity),
          "has_specialized_editor_false_for_generic_kinds: Velocity expected false");
    CHECK(!editors.has_specialized_editor(ComponentKind::SphereCollider),
          "has_specialized_editor_false_for_generic_kinds: SphereCollider expected false");
    CHECK(!editors.has_specialized_editor(ComponentKind::CapsuleCollider),
          "has_specialized_editor_false_for_generic_kinds: CapsuleCollider expected false");
    CHECK(!editors.has_specialized_editor(ComponentKind::BoxCollider),
          "has_specialized_editor_false_for_generic_kinds: BoxCollider expected false");
    CHECK(!editors.has_specialized_editor(ComponentKind::ConvexHullCollider),
          "has_specialized_editor_false_for_generic_kinds: ConvexHullCollider expected false");
}

void test_part_picker_state_defaults() {
    PartPickerState state;
    CHECK(state.selected_hash == 0,
          "part_picker_state_defaults: expected selected_hash == 0");
    CHECK(!state.picker_open,
          "part_picker_state_defaults: expected picker_open == false");
}

void test_streaming_editor_state_defaults() {
    StreamingEditorState state;
    CHECK(!state.follow_camera,
          "streaming_editor_state_defaults: expected follow_camera == false");
    CHECK(state.seed == 0,
          "streaming_editor_state_defaults: expected seed == 0");
}

void test_command_structs_default_construct_null() {
    PartEditorCommands part_cmds;
    CHECK(!part_cmds.assign_part,
          "command_structs_default_construct_null: assign_part should be null");
    CHECK(!part_cmds.list_available_parts,
          "command_structs_default_construct_null: list_available_parts should be null");

    PhysicsEditorCommands physics_cmds;
    CHECK(!physics_cmds.set_linear_velocity,
          "command_structs_default_construct_null: set_linear_velocity should be null");
    CHECK(!physics_cmds.apply_impulse,
          "command_structs_default_construct_null: apply_impulse should be null");
    CHECK(!physics_cmds.wake,
          "command_structs_default_construct_null: wake should be null");
    CHECK(!physics_cmds.teleport,
          "command_structs_default_construct_null: teleport should be null");

    StreamingEditorCommands streaming_cmds;
    CHECK(!streaming_cmds.attach_streaming,
          "command_structs_default_construct_null: attach_streaming should be null");
    CHECK(!streaming_cmds.remove_streaming,
          "command_structs_default_construct_null: remove_streaming should be null");
    CHECK(!streaming_cmds.set_follow_camera,
          "command_structs_default_construct_null: set_follow_camera should be null");
    CHECK(!streaming_cmds.regenerate,
          "command_structs_default_construct_null: regenerate should be null");
}

void test_registry_accessors_round_trip() {
    SpecializedEditors editors;
    editors.part_picker_state().selected_hash = 42;
    editors.part_picker_state().picker_open = true;
    CHECK(editors.part_picker_state().selected_hash == 42,
          "registry_accessors_round_trip: expected selected_hash == 42");
    CHECK(editors.part_picker_state().picker_open,
          "registry_accessors_round_trip: expected picker_open == true");

    editors.streaming_state().follow_camera = true;
    editors.streaming_state().seed = 7;
    CHECK(editors.streaming_state().follow_camera,
          "registry_accessors_round_trip: expected follow_camera == true");
    CHECK(editors.streaming_state().seed == 7,
          "registry_accessors_round_trip: expected seed == 7");
}

} // namespace

int main() {
    test_has_specialized_editor_true_for_part_instance();
    test_has_specialized_editor_true_for_physics();
    test_has_specialized_editor_true_for_streaming();
    test_has_specialized_editor_false_for_transform();
    test_has_specialized_editor_false_for_generic_kinds();
    test_part_picker_state_defaults();
    test_streaming_editor_state_defaults();
    test_command_structs_default_construct_null();
    test_registry_accessors_round_trip();

    return check_summary();
}
