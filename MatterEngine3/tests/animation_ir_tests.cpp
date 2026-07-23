#include "check.h"
#include "animation/animation_validate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace matter;
using namespace matter::animation;

namespace {

SourceSpan at(const char* object, uint32_t line) { return {"fixture.anim", line, 1, object}; }

AnimationTransform transform(float x = 0.0f, float y = 0.0f, float z = 0.0f) {
    AnimationTransform value;
    value.translation = {x, y, z};
    return value;
}

AnimationBuild valid_build() {
    AnimationBuild build;
    build.rig.joints = {
        {"root", "", transform(), 1.0f, at("root", 1)},
        {"mid", "root", transform(0.0f, 1.0f), 1.0f, at("mid", 2)},
        {"tip", "mid", transform(0.0f, 1.0f), 1.0f, at("tip", 3)},
        {"arm", "root", transform(1.0f), 1.0f, at("arm", 4)},
    };
    build.clips = {{"idle", 1.0f, 30.0f, {}, {}, at("idle", 5)}};
    build.inputs = {{"speed", AnimationValueType::Number, 0.0, EvaluationCadence::Fixed, at("speed", 6)}};
    build.targets = {{"hand", "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("hand", 7)}};
    build.graph.nodes = {
        {"source", {}, false, EvaluationCadence::Fixed, at("source", 8)},
        {"output", {"source"}, true, EvaluationCadence::Fixed, at("output", 9)},
    };
    return build;
}

bool has_code(const Diagnostics& diagnostics, const char* code) {
    return std::any_of(diagnostics.items.begin(), diagnostics.items.end(), [code](const Diagnostic& value) { return value.code == code; });
}

void check_invalid(AnimationBuild build, const char* code, const char* message) {
    Diagnostics diagnostics;
    CHECK(!validate_animation_build(build, diagnostics), message);
    CHECK(has_code(diagnostics, code), code);
}

void test_duplicate_names_are_rejected() {
    AnimationBuild build = valid_build();
    build.rig.joints.push_back({"root", "", transform(), 1.0f, at("root-again", 10)});
    build.rig.sockets = {{"socket", "root", transform(), at("socket", 11)}, {"socket", "root", transform(), at("socket-again", 12)}};
    build.clips.push_back(build.clips.front()); build.inputs.push_back(build.inputs.front()); build.targets.push_back(build.targets.front());
    build.controllers = {{"controller", at("controller", 13)}, {"controller", at("controller-again", 14)}};
    build.graph.nodes.push_back(build.graph.nodes.front());
    Diagnostics diagnostics;
    CHECK(!validate_animation_build(build, diagnostics), "duplicate authored names fail");
    CHECK(has_code(diagnostics, "duplicate-joint"), "duplicate joint diagnosed");
    CHECK(has_code(diagnostics, "duplicate-socket"), "duplicate socket diagnosed");
    CHECK(has_code(diagnostics, "duplicate-clip"), "duplicate clip diagnosed");
    CHECK(has_code(diagnostics, "duplicate-input"), "duplicate input diagnosed");
    CHECK(has_code(diagnostics, "duplicate-target"), "duplicate target diagnosed");
    CHECK(has_code(diagnostics, "duplicate-controller"), "duplicate controller diagnosed");
    CHECK(has_code(diagnostics, "duplicate-graph-node"), "duplicate graph node diagnosed");
}

void test_rig_structure_and_numeric_values_are_validated() {
    AnimationBuild missing = valid_build(); missing.rig.joints[1].parent = "none"; check_invalid(missing, "missing-parent", "missing parent fails");
    AnimationBuild forward = valid_build(); forward.rig.joints[0].parent = "mid"; check_invalid(forward, "parent-forward-reference", "forward parent fails");
    AnimationBuild roots = valid_build(); roots.rig.joints[1].parent.clear(); check_invalid(roots, "multiple-roots", "multiple roots fail");
    AnimationBuild cycle = valid_build(); cycle.rig.joints[0].parent = "tip"; Diagnostics cycle_diagnostics; CHECK(!validate_animation_build(cycle, cycle_diagnostics), "cycle fails deterministically"); CHECK(has_code(cycle_diagnostics, "parent-forward-reference"), "cycle forward reference diagnosed"); CHECK(has_code(cycle_diagnostics, "joint-cycle"), "cycle diagnosed");
    AnimationBuild numeric = valid_build();
    numeric.rig.joints[0].local.translation.x = std::numeric_limits<float>::infinity();
    numeric.rig.joints[1].local.rotation = {0, 0, 0, 0}; numeric.rig.joints[2].radius = 0.0f;
    Diagnostics diagnostics; CHECK(!validate_animation_build(numeric, diagnostics), "numeric rig values fail");
    CHECK(has_code(diagnostics, "non-finite-transform"), "non-finite transforms diagnosed");
    CHECK(has_code(diagnostics, "non-normalizable-rotation"), "invalid rotations diagnosed");
    CHECK(has_code(diagnostics, "invalid-joint-radius"), "invalid radii diagnosed");
}

void test_limits_and_clip_data_are_validated() {
    AnimationBuild joints = valid_build(); for (uint32_t i = 0; i < kMaxJoints; ++i) joints.rig.joints.push_back({"j" + std::to_string(i), "root", transform(), 1.0f, at("joint", 20 + i)}); check_invalid(joints, "joint-limit", "joint limit fails without truncation");
    AnimationBuild targets = valid_build(); for (uint32_t i = 0; i < kMaxTargets; ++i) targets.targets.push_back({"t" + std::to_string(i), "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("target", 300 + i)}); check_invalid(targets, "target-limit", "target limit fails without truncation");
    AnimationBuild graph = valid_build(); for (uint32_t i = 0; i < kMaxGraphNodes; ++i) graph.graph.nodes.push_back({"g" + std::to_string(i), {}, false, EvaluationCadence::Fixed, at("node", 400 + i)}); check_invalid(graph, "graph-node-limit", "graph limit fails without truncation");
    AnimationBuild clip = valid_build(); clip.clips[0].duration = 0.0f; clip.clips[0].rate = std::numeric_limits<float>::quiet_NaN();
    clip.clips[0].tracks = {{"root", {{0.8f, transform(), at("key-a", 30)}, {0.2f, transform(), at("key-b", 31)}, {std::numeric_limits<float>::quiet_NaN(), transform(), at("key-c", 32)}}, at("track", 29)}}; clip.clips[0].markers = {{"bad", std::numeric_limits<float>::infinity(), at("marker", 33)}};
    Diagnostics diagnostics; CHECK(!validate_animation_build(clip, diagnostics), "invalid clip fails");
    CHECK(has_code(diagnostics, "invalid-clip-duration"), "duration diagnosed"); CHECK(has_code(diagnostics, "invalid-clip-rate"), "rate diagnosed"); CHECK(has_code(diagnostics, "key-out-of-range"), "key range diagnosed"); CHECK(has_code(diagnostics, "nonmonotonic-key"), "key order diagnosed"); CHECK(has_code(diagnostics, "marker-out-of-range"), "marker range diagnosed");
    AnimationBuild bindings = valid_build(); bindings.skin_bindings = {{"skin", {"root", "mid", "tip", "arm", "root"}, at("skin", 33)}}; bindings.rigid_bindings = {{"rigid", "missing", transform(), at("rigid", 34)}}; bindings.attachments = {{"attachment", "missing", transform(), at("attachment", 35)}};
    diagnostics.items.clear(); CHECK(!validate_animation_build(bindings, diagnostics), "invalid bindings fail"); CHECK(has_code(diagnostics, "skin-influence-limit"), "skin influence limit diagnosed"); CHECK(has_code(diagnostics, "missing-rigid-joint"), "rigid joint diagnosed"); CHECK(has_code(diagnostics, "missing-attachment-socket"), "attachment socket diagnosed");
}

void test_inputs_drivers_targets_and_graph_are_validated() {
    AnimationBuild build = valid_build(); build.inputs[0].default_value = true; build.inputs[0].cadence = static_cast<EvaluationCadence>(99);
    build.controllers = {{"controller", at("controller", 40)}}; build.targets[0].driver = TargetDriverKind::Controller; build.targets[0].controller = "missing";
    build.targets.push_back({"also-hand", "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("also-hand", 41)}); build.graph.nodes[0].cadence = EvaluationCadence::Frame;
    Diagnostics diagnostics; CHECK(!validate_animation_build(build, diagnostics), "invalid input, drivers and graph fail");
    CHECK(has_code(diagnostics, "input-default-type"), "input default type diagnosed"); CHECK(has_code(diagnostics, "invalid-cadence"), "unsupported cadence diagnosed"); CHECK(has_code(diagnostics, "bad-controller-reference"), "bad controller diagnosed"); CHECK(has_code(diagnostics, "multiple-target-drivers"), "multiple driver diagnosed"); CHECK(has_code(diagnostics, "frame-to-fixed-dependency"), "cadence crossing diagnosed");
    AnimationBuild graph = valid_build(); graph.graph.nodes[0].dependencies = {"missing"}; graph.graph.nodes[1].is_output = false; Diagnostics graph_diagnostics; CHECK(!validate_animation_build(graph, graph_diagnostics), "missing graph reference fails"); CHECK(has_code(graph_diagnostics, "missing-graph-reference"), "missing graph reference diagnosed"); CHECK(has_code(graph_diagnostics, "missing-graph-output"), "absent graph output diagnosed");
    graph = valid_build(); graph.graph.nodes[0].dependencies = {"output"}; check_invalid(graph, "graph-cycle", "graph cycle fails");
    graph = valid_build(); graph.graph.nodes[0].is_output = true; check_invalid(graph, "multiple-graph-output", "multiple graph outputs fail");
    AnimationBuild driver = valid_build(); driver.targets[0].driver = static_cast<TargetDriverKind>(99); check_invalid(driver, "invalid-target-driver", "missing target driver fails");
    AnimationBuild defaults = valid_build(); defaults.inputs[0].default_value = std::numeric_limits<double>::infinity(); check_invalid(defaults, "non-finite-input-default", "non-finite input default fails");
}

void test_target_chains_and_canonical_orders_are_deterministic() {
    AnimationBuild path = valid_build(); path.targets[0].start_joint = "mid"; path.targets[0].end_joint = "arm"; check_invalid(path, "target-not-descendant", "target end must descend from start");
    AnimationBuild length = valid_build(); length.targets[0].start_joint = "mid"; check_invalid(length, "target-chain-length", "v1 needs three joints");
    AnimationBuild overlap = valid_build(); overlap.targets.push_back({"other", "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("other", 50)}); check_invalid(overlap, "overlapping-target-chain", "overlapping writable chains fail");
    AnimationBuild a = valid_build(), b = valid_build(); b.rig.joints[0].source.line = 99; CanonicalAnimationBuild ca, cb; Diagnostics da, db;
    CHECK(validate_and_canonicalize_animation_build(a, ca, da), "canonical fixture validates"); CHECK(validate_and_canonicalize_animation_build(b, cb, db), "equivalent fixture validates");
    CHECK(ca.rig.joints[0].name == "root" && ca.rig.joints[1].name == "mid" && ca.rig.joints[2].name == "tip" && ca.rig.joints[3].name == "arm", "rig uses preorder and declared sibling order");
    const JointRange root_range{0, 4};
    const JointRange mid_range{1, 3};
    CHECK(ca.rig.joints[0].subtree == root_range && ca.rig.joints[1].subtree == mid_range, "subtree ranges are contiguous and exact"); CHECK(ca.targets[0].chain.size() == 3 && ca.targets[0].chain[0] == 0 && ca.targets[0].chain[2] == 2, "target chain is inclusive and canonical");
    CHECK(ca.encode() == cb.encode(), "equivalent builds encode identically");
    AnimationBuild changed = valid_build(); changed.clips[0].name = "other"; CanonicalAnimationBuild changed_canonical; Diagnostics changed_diagnostics;
    CHECK(validate_and_canonicalize_animation_build(changed, changed_canonical, changed_diagnostics), "changed fixture validates"); CHECK(ca.encode() != changed_canonical.encode(), "encoding retains all authored IR data");
    AnimationBuild tied = valid_build(); tied.graph.nodes = {{"first", {}, false, EvaluationCadence::Fixed, at("first", 60)}, {"second", {}, false, EvaluationCadence::Fixed, at("second", 61)}, {"output", {"first", "second"}, true, EvaluationCadence::Fixed, at("output", 62)}};
    CanonicalAnimationBuild tied_canonical; Diagnostics tied_diagnostics;
    CHECK(validate_and_canonicalize_animation_build(tied, tied_canonical, tied_diagnostics), "tied graph fixture validates"); CHECK(tied_canonical.graph_order == std::vector<uint16_t>({0, 1, 2}), "graph topological sort keeps declaration ties");
}

void test_diagnostics_are_stably_sorted() {
    AnimationBuild build = valid_build(); build.rig.joints[2].radius = -1.0f; build.rig.joints[1].parent = "missing"; Diagnostics first, second;
    validate_animation_build(build, first); validate_animation_build(build, second); CHECK(first.items == second.items, "diagnostics are stable across validation runs"); CHECK(std::is_sorted(first.items.begin(), first.items.end(), DiagnosticLess{}), "diagnostics have deterministic order");
}

} // namespace

int main() {
    test_duplicate_names_are_rejected(); test_rig_structure_and_numeric_values_are_validated(); test_limits_and_clip_data_are_validated(); test_inputs_drivers_targets_and_graph_are_validated(); test_target_chains_and_canonical_orders_are_deterministic(); test_diagnostics_are_stably_sorted();
    if (g_failures != 0) { std::printf("animation_ir_tests: %d failure(s)\n", g_failures); return 1; }
    std::printf("animation_ir_tests: all tests passed\n"); return 0;
}
