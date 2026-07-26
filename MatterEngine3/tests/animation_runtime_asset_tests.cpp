#include "animation/animation_binding_bake.h"
#include "animation/animation_evaluator.h"
#include "animation/animation_runtime_asset.h"
#include "animation/animation_validate.h"
#include "animation/ozz_adapter.h"
#include "check.h"

#include <algorithm>
#include <cmath>

using namespace matter;
using namespace matter::animation;

namespace {

AnimationTransform local(float x, float y, float z) {
    AnimationTransform value{};
    value.translation = {x, y, z};
    return value;
}

JointDef joint(const char* name, const char* parent, AnimationTransform transform) {
    JointDef value{};
    value.name = name;
    value.parent = parent;
    value.local = transform;
    value.radius = 0.2f;
    return value;
}

ClipDefinition clip(const char* name, float duration, bool moving) {
    ClipDefinition value{};
    value.name = name;
    value.duration = duration;
    value.rate = 1.0f;
    value.loop = true;
    ClipTrack root{};
    root.joint = "root";
    root.keys = {{0.0f, local(0, 0, 0), {}},
                 {duration, local(moving ? duration : 0.0f, 0, 0), {}}};
    value.tracks.push_back(std::move(root));
    value.markers.push_back({moving ? "step" : "idle", duration * 0.5f, {}});
    return value;
}

AnimationBuild fixture_build() {
    AnimationBuild build{};
    build.rig.joints = {
        joint("root", "", local(0, 1, 0)),
        joint("leftHip", "root", local(-0.3f, -0.2f, 0)),
        joint("leftKnee", "leftHip", local(0, -0.8f, 0)),
        joint("leftFoot", "leftKnee", local(0, -0.8f, 0.2f)),
        joint("rightHip", "root", local(0.3f, -0.2f, 0)),
        joint("rightKnee", "rightHip", local(0, -0.8f, 0)),
        joint("rightFoot", "rightKnee", local(0, -0.8f, 0.2f)),
    };
    build.clips = {clip("idle", 1.5f, false), clip("walk", 0.8f, true)};

    InputSchema speed{};
    speed.name = "speed";
    speed.type = AnimationValueType::Number;
    speed.default_value = AnimationValue(0.0);
    speed.cadence = EvaluationCadence::Fixed;
    build.inputs.push_back(speed);

    ControllerDef gait{};
    gait.name = "gait";
    gait.type = "proceduralGait";
    gait.cadence = EvaluationCadence::Fixed;
    build.controllers.push_back(gait);

    TargetSchema left{};
    left.name = "leftFootTarget";
    left.start_joint = "leftHip";
    left.end_joint = "leftFoot";
    left.driver = TargetDriverKind::Controller;
    left.controller = "gait";
    left.cadence = EvaluationCadence::Fixed;
    left.pole = {0, 0, 1};
    left.has_pole = true;
    TargetSchema right = left;
    right.name = "rightFootTarget";
    right.start_joint = "rightHip";
    right.end_joint = "rightFoot";
    build.targets = {left, right};

    GraphNode idle{};
    idle.name = "idleNode";
    idle.kind = GraphNodeKind::Clip;
    idle.clip = "idle";
    GraphNode walk = idle;
    walk.name = "walkNode";
    walk.clip = "walk";
    GraphNode blend{};
    blend.name = "speedBlend";
    blend.kind = GraphNodeKind::Blend1D;
    blend.input = "speed";
    blend.dependencies = {"idleNode", "walkNode"};
    blend.thresholds = {0.0f, 1.0f};
    GraphNode native{};
    native.name = "gaitNode";
    native.kind = GraphNodeKind::NativeController;
    native.controller = "gait";
    native.dependencies = {"speedBlend"};
    GraphNode output{};
    output.name = "out";
    output.kind = GraphNodeKind::Output;
    output.is_output = true;
    output.dependencies = {"gaitNode"};
    build.graph.nodes = {idle, walk, blend, native, output};
    return build;
}

Mat4f identity() {
    Mat4f value{};
    value.m[0] = value.m[5] = value.m[10] = value.m[15] = 1.0f;
    return value;
}

AnimAsset encoded_fixture() {
    AnimationBuild build = fixture_build();
    Diagnostics diagnostics;
    CanonicalAnimationBuild canonical;
    CHECK(validate_and_canonicalize_animation_build(build, canonical, diagnostics),
          "runtime-asset fixture canonicalizes");

    OzzSkeleton skeleton;
    CHECK(build_skeleton(build.rig, skeleton, diagnostics) &&
              serialize_skeleton(skeleton, build.ozz_skeleton_blob),
          "runtime-asset fixture serializes its skeleton");
    for (ClipDefinition& source : build.clips) {
        OzzAnimation animation;
        CHECK(build_clip(build.rig, source, animation, diagnostics) &&
                  serialize_animation(animation, source.ozz_blob),
              "runtime-asset fixture serializes each clip");
    }

    AnimAsset asset{};
    asset.resolved_hash = 0xc401u;
    asset.nonce = {7, 9};
    asset.target_abi_tag = kAnimationTargetAbiTag;
    asset.ozz_tag_hash = kAnimationOzzTagHash;
    CHECK(encode_animation_runtime_sections(build, canonical, asset, diagnostics),
          "runtime sections encode from canonical authoring data");

    BindingBake binding{};
    binding.inverse_bind_matrices.assign(canonical.rig.joints.size(), identity());
    RigidSegmentBake rigid{};
    rigid.name = "fixture";
    rigid.joint = 1;
    rigid.geometry.push_back({0, 1, 0, 0});
    binding.rigid_segments.push_back(rigid);
    CHECK(set_anim_binding_bake(asset, binding),
          "runtime-asset fixture carries the committed inverse-bind payload");
    return asset;
}

void test_round_trip_builds_real_runtime_definition() {
    AnimAsset asset = encoded_fixture();
    Diagnostics diagnostics;
    DecodedAnimationRuntimeAsset decoded;
    CHECK(decode_animation_runtime_asset(asset, decoded, diagnostics),
          "committed runtime sections decode");
    CHECK(diagnostics.items.empty(), "successful runtime decode has no diagnostics");
    CHECK(decoded.rig.joints.size() == 7 && decoded.definition.binding &&
              decoded.definition.binding->evaluation &&
              decoded.definition.binding->evaluation->skeleton->joint_count() == 7,
          "decoded definition owns the canonical rig and real Ozz skeleton");
    CHECK(decoded.definition.inputs.size() == 1 &&
              decoded.definition.inputs[0].name == "speed" &&
              decoded.definition.inputs[0].type == AnimationValueType::Number &&
              decoded.definition.inputs[0].cadence == EvaluationCadence::Fixed,
          "decoded definition preserves typed input declarations");
    CHECK(decoded.definition.targets.size() == 2 &&
              decoded.definition.binding->targets.size() == 2 &&
              decoded.definition.binding->controllers.size() == 1,
          "decoded definition preserves gait targets and native controller");

    const auto& evaluation = *decoded.definition.binding->evaluation;
    CHECK(evaluation.clips.size() == 2 && evaluation.clips[0].markers.size() == 1 &&
              evaluation.clips[1].markers.size() == 1,
          "framed Ozz clips retain declaration-order markers");
    CHECK(evaluation.nodes.size() == 5 &&
              evaluation.nodes[2].kind == RuntimeGraphNodeKind::Blend1D &&
              evaluation.nodes[2].dependencies.size() == 2 &&
              evaluation.nodes[4].kind == RuntimeGraphNodeKind::Output,
          "decoded runtime uses the authored idle/walk blend graph");

    AnimationEvaluator evaluator;
    const AnimationValue speed(1.0);
    AnimationEvaluationRequest request{};
    request.instance = {0, 1};
    request.definition = &evaluation;
    request.fixed_previous = {&speed, 1};
    request.fixed_current = {&speed, 1};
    request.fixed_tick = 1;
    request.frame_serial = 1;
    request.fixed_delta_seconds = 0.1f;
    request.accumulator_alpha = 1.0f;
    CHECK(evaluator.evaluate({request}), "decoded Ozz graph evaluates after decode returns");
    request.fixed_tick = 2;
    request.frame_serial = 2;
    CHECK(evaluator.evaluate({request}), "decoded Ozz graph advances a second fixed sample");
    DesiredRootMotion motion{};
    CHECK(evaluator.fixed_root_motion(request.instance, motion) && motion.valid &&
              std::fabs(motion.delta.translation.x) > 1e-4f,
          "decoded walk graph produces real root motion");
}

void test_corrupt_sections_fail_closed() {
    const AnimAsset good = encoded_fixture();
    const AnimSectionKind corruptions[] = {
        AnimSectionKind::RigSchema,
        AnimSectionKind::InputTargetSchemas,
        AnimSectionKind::GraphControllerBytecode,
        AnimSectionKind::OzzSkeleton,
        AnimSectionKind::OzzClips,
    };
    for (AnimSectionKind kind : corruptions) {
        AnimAsset corrupt = good;
        for (AnimSection& section : corrupt.sections)
            if (section.kind == kind) section.bytes.resize(section.bytes.size() / 2);
        Diagnostics diagnostics;
        DecodedAnimationRuntimeAsset decoded;
        CHECK(!decode_animation_runtime_asset(corrupt, decoded, diagnostics) &&
                  !diagnostics.items.empty(),
              "truncated runtime section fails closed with diagnostics");
    }

    AnimAsset missing = good;
    missing.sections.erase(
        std::remove_if(missing.sections.begin(), missing.sections.end(),
                       [](const AnimSection& section) {
                           return section.kind == AnimSectionKind::GraphControllerBytecode;
                       }),
        missing.sections.end());
    Diagnostics diagnostics;
    DecodedAnimationRuntimeAsset decoded;
    CHECK(!decode_animation_runtime_asset(missing, decoded, diagnostics) &&
              !diagnostics.items.empty(),
          "missing required runtime section fails closed with diagnostics");

    AnimAsset duplicate = good;
    for (const AnimSection& section : good.sections) {
        if (section.kind != AnimSectionKind::RigSchema) continue;
        duplicate.sections.push_back(section);
        break;
    }
    diagnostics.items.clear();
    CHECK(!decode_animation_runtime_asset(duplicate, decoded, diagnostics) &&
              !diagnostics.items.empty(),
          "duplicate required runtime section fails closed with diagnostics");
}

void test_runtime_sections_are_deterministic_and_reject_bad_counts() {
    const AnimAsset first = encoded_fixture();
    const AnimAsset second = encoded_fixture();
    for (AnimSectionKind kind : {AnimSectionKind::RigSchema,
                                 AnimSectionKind::InputTargetSchemas,
                                 AnimSectionKind::GraphControllerBytecode,
                                 AnimSectionKind::OzzSkeleton,
                                 AnimSectionKind::OzzClips}) {
        const auto find = [kind](const AnimAsset& asset) -> const AnimSection* {
            for (const AnimSection& section : asset.sections)
                if (section.kind == kind) return &section;
            return nullptr;
        };
        const AnimSection* left = find(first);
        const AnimSection* right = find(second);
        CHECK(left && right && left->bytes == right->bytes,
              "runtime section bytes are deterministic");
    }

    for (AnimSectionKind kind : {AnimSectionKind::RigSchema,
                                 AnimSectionKind::InputTargetSchemas,
                                 AnimSectionKind::GraphControllerBytecode,
                                 AnimSectionKind::OzzClips}) {
        AnimAsset corrupt = first;
        for (AnimSection& section : corrupt.sections) {
            if (section.kind != kind || section.bytes.size() < 12) continue;
            section.bytes[8] = section.bytes[9] = section.bytes[10] = section.bytes[11] = 0xff;
        }
        Diagnostics diagnostics;
        DecodedAnimationRuntimeAsset decoded;
        CHECK(!decode_animation_runtime_asset(corrupt, decoded, diagnostics) &&
                  !diagnostics.items.empty(),
              "oversized runtime section count fails closed");
    }

    AnimAsset bad_length = first;
    for (AnimSection& section : bad_length.sections) {
        if (section.kind != AnimSectionKind::OzzClips || section.bytes.size() < 16) continue;
        section.bytes[12] = section.bytes[13] = section.bytes[14] = section.bytes[15] = 0xff;
    }
    Diagnostics diagnostics;
    DecodedAnimationRuntimeAsset decoded;
    CHECK(!decode_animation_runtime_asset(bad_length, decoded, diagnostics) &&
              !diagnostics.items.empty(),
          "oversized framed clip length fails closed");
}

} // namespace

int main() {
    test_round_trip_builds_real_runtime_definition();
    test_corrupt_sections_fail_closed();
    test_runtime_sections_are_deterministic_and_reject_bad_counts();
    return check_summary();
}
