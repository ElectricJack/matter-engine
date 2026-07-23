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
    build.targets = {{"hand", "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("hand", 7), {0.0f, 0.0f, 1.0f}, true}};
    build.graph.nodes = {
        {"source", {}, false, EvaluationCadence::Fixed, at("source", 8)},
        {"output", {"source"}, true, EvaluationCadence::Fixed, at("output", 9)},
    };
    build.graph.nodes[0].kind = GraphNodeKind::Clip;
    build.graph.nodes[0].clip = "idle";
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
    build.controllers = {{"controller", at("controller", 13), EvaluationCadence::Fixed, "native"}, {"controller", at("controller-again", 14), EvaluationCadence::Fixed, "native"}};
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
    AnimationBuild socket = valid_build(); socket.rig.sockets = {{"bad-socket", "missing", transform(), at("bad-socket", 15)}}; check_invalid(socket, "missing-socket-joint", "socket missing joint fails");
}

void test_limits_and_clip_data_are_validated() {
    AnimationBuild joints = valid_build(); for (uint32_t i = 0; i < kMaxJoints; ++i) joints.rig.joints.push_back({"j" + std::to_string(i), "root", transform(), 1.0f, at("joint", 20 + i)}); check_invalid(joints, "joint-limit", "joint limit fails without truncation");
    AnimationBuild targets = valid_build(); for (uint32_t i = 0; i < kMaxTargets; ++i) targets.targets.push_back({"t" + std::to_string(i), "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("target", 300 + i)}); check_invalid(targets, "target-limit", "target limit fails without truncation");
    AnimationBuild graph = valid_build(); for (uint32_t i = 0; i < kMaxGraphNodes; ++i) graph.graph.nodes.push_back({"g" + std::to_string(i), {}, false, EvaluationCadence::Fixed, at("node", 400 + i)}); check_invalid(graph, "graph-node-limit", "graph limit fails without truncation");
    AnimationBuild clip = valid_build(); clip.clips[0].duration = 0.0f; clip.clips[0].rate = std::numeric_limits<float>::quiet_NaN();
    clip.clips[0].tracks = {{"root", {{0.8f, transform(), at("key-a", 30)}, {0.2f, transform(), at("key-b", 31)}, {std::numeric_limits<float>::quiet_NaN(), transform(), at("key-c", 32)}}, at("track", 29)}}; clip.clips[0].markers = {{"bad", std::numeric_limits<float>::infinity(), at("marker", 33)}};
    Diagnostics diagnostics; CHECK(!validate_animation_build(clip, diagnostics), "invalid clip fails");
    CHECK(has_code(diagnostics, "invalid-clip-duration"), "duration diagnosed"); CHECK(has_code(diagnostics, "invalid-clip-rate"), "rate diagnosed"); CHECK(has_code(diagnostics, "key-out-of-range"), "key range diagnosed"); CHECK(has_code(diagnostics, "nonmonotonic-key"), "key order diagnosed"); CHECK(has_code(diagnostics, "marker-out-of-range"), "marker range diagnosed");
    AnimationBuild track = valid_build(); track.clips[0].tracks = {{"missing", {{0.0f, transform(), at("missing-key", 34)}}, at("missing-track", 35)}}; check_invalid(track, "missing-track-joint", "clip track missing joint fails");
    AnimationBuild key_value = valid_build(); key_value.clips[0].tracks = {{"root", {{0.0f, transform(), at("key", 36)}}, at("track", 37)}}; key_value.clips[0].tracks[0].keys[0].value.rotation = {0, 0, 0, 0}; key_value.clips[0].tracks[0].keys[0].value.scale.x = std::numeric_limits<float>::infinity();
    diagnostics.items.clear(); CHECK(!validate_animation_build(key_value, diagnostics), "invalid clip key transform fails"); CHECK(has_code(diagnostics, "non-finite-transform"), "non-finite key transform diagnosed"); CHECK(has_code(diagnostics, "non-normalizable-rotation"), "key rotation diagnosed");
    AnimationBuild bindings = valid_build(); bindings.skin_bindings = {{"skin", {"root", "mid", "tip", "arm", "root"}, at("skin", 33)}}; bindings.rigid_bindings = {{"rigid", "missing", transform(), at("rigid", 34)}}; bindings.attachments = {{"attachment", "missing", transform(), at("attachment", 35)}};
    diagnostics.items.clear(); CHECK(!validate_animation_build(bindings, diagnostics), "invalid bindings fail"); CHECK(has_code(diagnostics, "skin-influence-limit"), "skin influence limit diagnosed"); CHECK(has_code(diagnostics, "missing-rigid-joint"), "rigid joint diagnosed"); CHECK(has_code(diagnostics, "missing-attachment-socket"), "attachment socket diagnosed");
}

void test_inputs_drivers_targets_and_graph_are_validated() {
    AnimationBuild build = valid_build(); build.inputs[0].default_value = true; build.inputs[0].cadence = static_cast<EvaluationCadence>(99);
    build.controllers = {{"controller", at("controller", 40), EvaluationCadence::Fixed, "native"}}; build.targets[0].driver = TargetDriverKind::Controller; build.targets[0].controller = "missing";
    build.targets.push_back({"also-hand", "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("also-hand", 41)}); build.graph.nodes[0].cadence = EvaluationCadence::Frame;
    Diagnostics diagnostics; CHECK(!validate_animation_build(build, diagnostics), "invalid input, drivers and graph fail");
    CHECK(has_code(diagnostics, "input-default-type"), "input default type diagnosed"); CHECK(has_code(diagnostics, "invalid-cadence"), "unsupported cadence diagnosed"); CHECK(has_code(diagnostics, "bad-controller-reference"), "bad controller diagnosed"); CHECK(has_code(diagnostics, "multiple-target-drivers"), "multiple driver diagnosed"); CHECK(has_code(diagnostics, "frame-to-fixed-dependency"), "cadence crossing diagnosed");
    AnimationBuild graph = valid_build(); graph.graph.nodes[0].dependencies = {"missing"}; graph.graph.nodes[1].is_output = false; Diagnostics graph_diagnostics; CHECK(!validate_animation_build(graph, graph_diagnostics), "missing graph reference fails"); CHECK(has_code(graph_diagnostics, "missing-graph-reference"), "missing graph reference diagnosed"); CHECK(has_code(graph_diagnostics, "missing-graph-output"), "absent graph output diagnosed");
    graph = valid_build(); graph.graph.nodes[0].dependencies = {"output"}; check_invalid(graph, "graph-cycle", "graph cycle fails");
    graph = valid_build(); graph.graph.nodes[0].is_output = true; check_invalid(graph, "multiple-graph-output", "multiple graph outputs fail");
    AnimationBuild driver = valid_build(); driver.targets[0].driver = static_cast<TargetDriverKind>(99); check_invalid(driver, "invalid-target-driver", "missing target driver fails");
    AnimationBuild defaults = valid_build(); defaults.inputs[0].default_value = std::numeric_limits<double>::infinity(); check_invalid(defaults, "non-finite-input-default", "non-finite input default fails");
    AnimationBuild all_types = valid_build(); all_types.inputs = {{"bool", AnimationValueType::Bool, true, EvaluationCadence::Fixed, at("bool", 42)}, {"number", AnimationValueType::Number, 1.0, EvaluationCadence::Fixed, at("number", 43)}, {"float3", AnimationValueType::Float3, Float3{1, 2, 3}, EvaluationCadence::Fixed, at("float3", 44)}, {"quat", AnimationValueType::Quaternion, Quaternion{0, 0, 0, 1}, EvaluationCadence::Fixed, at("quat", 45)}, {"transform", AnimationValueType::Transform, transform(), EvaluationCadence::Fixed, at("transform", 46)}, {"symbol", AnimationValueType::Symbol, "idle", EvaluationCadence::Fixed, at("symbol", 47)}};
    diagnostics.items.clear(); CHECK(validate_animation_build(all_types, diagnostics), "every typed valid default succeeds");
    all_types.inputs[0].default_value = 1.0; all_types.inputs[1].default_value = std::numeric_limits<double>::infinity(); all_types.inputs[2].default_value = Float3{std::numeric_limits<float>::infinity(), 0, 0}; all_types.inputs[3].default_value = Quaternion{0, 0, 0, 0}; all_types.inputs[4].default_value = transform(); all_types.inputs[4].default_value.transform.rotation = {0, 0, 0, 0}; all_types.inputs[5].default_value = "";
    diagnostics.items.clear(); CHECK(!validate_animation_build(all_types, diagnostics), "typed default mismatches and invalid values fail"); CHECK(has_code(diagnostics, "input-default-type"), "typed mismatch diagnosed"); CHECK(has_code(diagnostics, "non-finite-input-default"), "invalid typed defaults diagnosed");
    AnimationBuild invalid_type = valid_build(); invalid_type.inputs[0].type = static_cast<AnimationValueType>(99); check_invalid(invalid_type, "unsupported-input-type", "unsupported input type fails");
    AnimationBuild invalid_controller_type = valid_build(); invalid_controller_type.controllers = {{"controller", at("controller-type", 48), EvaluationCadence::Fixed, ""}}; check_invalid(invalid_controller_type, "invalid-controller-type", "controller type is required");
}

void test_target_chains_and_canonical_orders_are_deterministic() {
    AnimationBuild missing_start = valid_build(); missing_start.targets[0].start_joint = "missing"; check_invalid(missing_start, "missing-target-start-joint", "missing target start fails");
    AnimationBuild missing_end = valid_build(); missing_end.targets[0].end_joint = "missing"; check_invalid(missing_end, "missing-target-end-joint", "missing target end fails");
    AnimationBuild external_controller = valid_build(); external_controller.targets[0].controller = "controller"; check_invalid(external_controller, "multiple-target-drivers", "external target cannot name controller");
    AnimationBuild bad_target_cadence = valid_build(); bad_target_cadence.targets[0].cadence = static_cast<EvaluationCadence>(99); check_invalid(bad_target_cadence, "invalid-cadence", "invalid target cadence fails");
    AnimationBuild bad_graph_cadence = valid_build(); bad_graph_cadence.graph.nodes[0].cadence = static_cast<EvaluationCadence>(99); check_invalid(bad_graph_cadence, "invalid-cadence", "invalid graph cadence fails");
    AnimationBuild bad_controller_cadence = valid_build(); bad_controller_cadence.controllers = {{"controller", at("controller", 48), static_cast<EvaluationCadence>(99), "native"}}; check_invalid(bad_controller_cadence, "invalid-cadence", "invalid controller cadence fails");
    AnimationBuild frame_controller_fixed_target = valid_build(); frame_controller_fixed_target.controllers = {{"controller", at("controller", 49), EvaluationCadence::Frame, "native"}}; frame_controller_fixed_target.targets[0].driver = TargetDriverKind::Controller; frame_controller_fixed_target.targets[0].controller = "controller"; frame_controller_fixed_target.targets[0].cadence = EvaluationCadence::Fixed; check_invalid(frame_controller_fixed_target, "frame-controller-to-fixed-target", "frame controller cannot drive fixed target");
    AnimationBuild path = valid_build(); path.targets[0].start_joint = "mid"; path.targets[0].end_joint = "arm"; check_invalid(path, "target-not-descendant", "target end must descend from start");
    AnimationBuild length = valid_build(); length.targets[0].start_joint = "mid"; check_invalid(length, "target-chain-length", "v1 needs three joints");
    AnimationBuild overlap = valid_build(); overlap.targets.push_back({"other", "root", "tip", TargetDriverKind::External, "", EvaluationCadence::Frame, at("other", 50)}); check_invalid(overlap, "overlapping-target-chain", "overlapping writable chains fail");
    AnimationBuild a = valid_build(), b = valid_build(); b.rig.joints[0].source.line = 99; CanonicalAnimationBuild ca, cb; Diagnostics da, db;
    CHECK(validate_and_canonicalize_animation_build(a, ca, da), "canonical fixture validates"); CHECK(validate_and_canonicalize_animation_build(b, cb, db), "equivalent fixture validates");
    CHECK(ca.rig.joints[0].name == "root" && ca.rig.joints[1].name == "mid" && ca.rig.joints[2].name == "tip" && ca.rig.joints[3].name == "arm", "rig uses preorder and declared sibling order");
    const JointRange root_range{0, 4};
    const JointRange mid_range{1, 3};
    const JointRange tip_range{2, 3};
    const JointRange arm_range{3, 4};
    CHECK(ca.rig.joints[0].subtree == root_range && ca.rig.joints[1].subtree == mid_range && ca.rig.joints[2].subtree == tip_range && ca.rig.joints[3].subtree == arm_range, "every branching subtree range is contiguous and exact"); CHECK(ca.targets[0].chain.size() == 3 && ca.targets[0].chain[0] == 0 && ca.targets[0].chain[2] == 2, "target chain is inclusive and canonical");
    CHECK(ca.encode() == cb.encode(), "equivalent builds encode identically");
    AnimationBuild changed = valid_build(); changed.clips[0].name = "other"; CanonicalAnimationBuild changed_canonical; Diagnostics changed_diagnostics;
    CHECK(validate_and_canonicalize_animation_build(changed, changed_canonical, changed_diagnostics), "changed fixture validates"); CHECK(ca.encode() != changed_canonical.encode(), "encoding retains all authored IR data");
    AnimationBuild fixed_controller = valid_build(); fixed_controller.controllers = {{"controller", at("controller", 63), EvaluationCadence::Fixed, "native"}};
    AnimationBuild frame_controller = fixed_controller; frame_controller.controllers[0].cadence = EvaluationCadence::Frame;
    CanonicalAnimationBuild fixed_canonical, frame_canonical; Diagnostics fixed_diagnostics, frame_diagnostics;
    CHECK(validate_and_canonicalize_animation_build(fixed_controller, fixed_canonical, fixed_diagnostics) && validate_and_canonicalize_animation_build(frame_controller, frame_canonical, frame_diagnostics), "controller cadence fixtures validate"); CHECK(fixed_canonical.encode() != frame_canonical.encode(), "encoding retains controller cadence");
    AnimationBuild tied = valid_build(); tied.graph.nodes = {{"first", {}, false, EvaluationCadence::Fixed, at("first", 60)}, {"second", {}, false, EvaluationCadence::Fixed, at("second", 61)}, {"add", {"first", "second"}, false, EvaluationCadence::Fixed, at("add", 62)}, {"output", {"add"}, true, EvaluationCadence::Fixed, at("output", 63)}}; tied.graph.nodes[0].kind = GraphNodeKind::Clip; tied.graph.nodes[0].clip = "idle"; tied.graph.nodes[1].kind = GraphNodeKind::Clip; tied.graph.nodes[1].clip = "idle"; tied.graph.nodes[2].kind = GraphNodeKind::Additive;
    CanonicalAnimationBuild tied_canonical; Diagnostics tied_diagnostics;
    CHECK(validate_and_canonicalize_animation_build(tied, tied_canonical, tied_diagnostics), "tied graph fixture validates"); CHECK(tied_canonical.graph_order == std::vector<uint16_t>({0, 1, 2, 3}), "graph topological sort keeps declaration ties");
    AnimationBuild bend = valid_build(); bend.rig.joints[2].local.translation = {1.0f, 1.0f, 0.0f}; bend.targets[0].has_pole = false; CanonicalAnimationBuild bend_canonical; Diagnostics bend_diagnostics;
    CHECK(validate_and_canonicalize_animation_build(bend, bend_canonical, bend_diagnostics), "non-collinear omitted pole fixture validates"); CHECK(std::fabs(bend_canonical.targets[0].bend_axis.z) > 0.99f, "bend axis is computed in start-joint-local bind space");
    AnimationBuild collinear = valid_build(); collinear.targets[0].has_pole = false; check_invalid(collinear, "collinear-target-pole-required", "collinear omitted pole is rejected");
    AnimationBuild half_life = valid_build(); half_life.targets[0].position_half_life = -1.0f; check_invalid(half_life, "invalid-target-half-life", "negative target half-life is rejected");
}

void test_graph_node_contracts_are_strict() {
    AnimationBuild build = valid_build(); build.graph.nodes[0].dependencies = {"output"}; check_invalid(build, "clip-fields", "clip dependencies are rejected");
    build = valid_build(); build.graph.nodes[0].kind = GraphNodeKind::Blend1D; build.graph.nodes[0].input = "speed"; build.graph.nodes[0].dependencies = {"output"}; build.graph.nodes[0].thresholds = {0.0f}; check_invalid(build, "blend1d-arity", "blend1D arity is rejected");
    build = valid_build(); build.graph.nodes[0].kind = GraphNodeKind::Additive; build.graph.nodes[0].dependencies = {"output"}; check_invalid(build, "additive-arity", "additive arity is rejected");
    build = valid_build(); build.graph.nodes[0].kind = GraphNodeKind::NativeController; build.graph.nodes[0].controller = "missing"; check_invalid(build, "missing-controller-reference", "native controller reference is required");
    build = valid_build(); build.graph.nodes[1].dependencies.clear(); check_invalid(build, "output-arity", "output arity is rejected");
    build = valid_build(); build.graph.nodes[0].kind = static_cast<GraphNodeKind>(99); check_invalid(build, "invalid-graph-kind", "unsupported graph kind is rejected");
}

void test_diagnostics_are_stably_sorted() {
    AnimationBuild build = valid_build(); build.rig.joints[2].radius = -1.0f; build.rig.joints[1].parent = "missing"; Diagnostics first, second;
    validate_animation_build(build, first); validate_animation_build(build, second); CHECK(first.items == second.items, "diagnostics are stable across validation runs"); CHECK(std::is_sorted(first.items.begin(), first.items.end(), DiagnosticLess{}), "diagnostics have deterministic order");
    AnimationBuild permuted = valid_build();
    permuted.rig.sockets = {{"late", "missing-a", transform(), at("late", 80)}, {"early", "missing-b", transform(), at("early", 70)}};
    permuted.clips = {{"late-clip", 0.0f, 30.0f, {}, {}, at("late-clip", 82)}, {"early-clip", -1.0f, 30.0f, {}, {}, at("early-clip", 72)}};
    AnimationBuild reordered = permuted; std::swap(reordered.rig.sockets[0], reordered.rig.sockets[1]); std::swap(reordered.clips[0], reordered.clips[1]);
    Diagnostics permuted_diagnostics, reordered_diagnostics; validate_animation_build(permuted, permuted_diagnostics); validate_animation_build(reordered, reordered_diagnostics); CHECK(permuted_diagnostics.items == reordered_diagnostics.items, "diagnostic order is independent of declaration traversal");
}

} // namespace

int main() {
    test_duplicate_names_are_rejected(); test_rig_structure_and_numeric_values_are_validated(); test_limits_and_clip_data_are_validated(); test_inputs_drivers_targets_and_graph_are_validated(); test_target_chains_and_canonical_orders_are_deterministic(); test_graph_node_contracts_are_strict(); test_diagnostics_are_stably_sorted();
    if (g_failures != 0) { std::printf("animation_ir_tests: %d failure(s)\n", g_failures); return 1; }
    std::printf("animation_ir_tests: all tests passed\n"); return 0;
}
