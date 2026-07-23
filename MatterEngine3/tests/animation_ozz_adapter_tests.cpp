#include "check.h"
#include "animation/ozz_adapter.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace matter;
using namespace matter::animation;

namespace {

bool near(float actual, float expected, float epsilon = 1e-3f) {
    return std::fabs(actual - expected) <= epsilon;
}

AnimationTransform transform(float x, float y, float z) {
    AnimationTransform value;
    value.translation = {x, y, z};
    return value;
}

SourceSpan source(const char* object) { return {"adapter.anim", 1, 1, object}; }

RigDefinition chain_rig() {
    RigDefinition rig;
    rig.joints = {
        {"root", "", transform(0.0f, 0.0f, 0.0f), 1.0f, source("root")},
        {"start", "root", transform(1.0f, 0.0f, 0.0f), 1.0f, source("start")},
        {"mid", "start", transform(1.0f, 0.0f, 0.0f), 1.0f, source("mid")},
        {"end", "mid", transform(1.0f, 0.0f, 0.0f), 1.0f, source("end")},
        {"tip", "end", transform(0.5f, 0.0f, 0.0f), 1.0f, source("tip")},
    };
    return rig;
}

ClipDefinition moving_clip() {
    ClipDefinition clip;
    clip.name = "moving";
    clip.duration = 1.0f;
    clip.rate = 30.0f;
    clip.source = source("moving");
    clip.tracks = {
        {"root", {{0.0f, transform(0.0f, 0.0f, 0.0f), source("root-0")},
                  {1.0f, transform(2.0f, 0.0f, 0.0f), source("root-1")}}, source("root-track")},
        {"start", {{0.0f, transform(1.0f, 0.0f, 0.0f), source("start-0")},
                   {1.0f, transform(1.0f, 0.0f, 0.0f), source("start-1")}}, source("start-track")},
        {"mid", {{0.0f, transform(1.0f, 0.0f, 0.0f), source("mid-0")},
                 {1.0f, transform(1.0f, 0.0f, 0.0f), source("mid-1")}}, source("mid-track")},
        {"end", {{0.0f, transform(1.0f, 0.0f, 0.0f), source("end-0")},
                 {1.0f, transform(1.0f, 0.0f, 0.0f), source("end-1")}}, source("end-track")},
        {"tip", {{0.0f, transform(0.5f, 0.0f, 0.0f), source("tip-0")},
                 {1.0f, transform(0.5f, 0.0f, 0.0f), source("tip-1")}}, source("tip-track")},
    };
    return clip;
}

void test_build_archive_and_sample_endpoints() {
    const RigDefinition rig_definition = chain_rig();
    const ClipDefinition clip_definition = moving_clip();
    Diagnostics diagnostics;
    OzzSkeleton skeleton;
    OzzAnimation animation;
    CHECK(build_skeleton(rig_definition, skeleton, diagnostics), "builds canonical Matter rig into ozz skeleton");
    CHECK(skeleton.joint_count() == 5 && skeleton.parent(2) == 1,
          "skeleton preserves Matter canonical joint order and parents");
    CHECK(build_clip(rig_definition, clip_definition, animation, diagnostics), "builds and optimizes raw clip");

    std::vector<uint8_t> skeleton_bytes;
    std::vector<uint8_t> animation_bytes;
    CHECK(serialize_skeleton(skeleton, skeleton_bytes) && !skeleton_bytes.empty(), "serializes skeleton archive");
    CHECK(serialize_animation(animation, animation_bytes) && !animation_bytes.empty(), "serializes animation archive");
    OzzSkeleton reloaded_skeleton;
    OzzAnimation reloaded_animation;
    CHECK(deserialize_skeleton(skeleton_bytes.data(), skeleton_bytes.size(), reloaded_skeleton, diagnostics), "deserializes skeleton archive");
    CHECK(deserialize_animation(animation_bytes.data(), animation_bytes.size(), reloaded_animation, diagnostics), "deserializes animation archive");
    std::vector<uint8_t> skeleton_round_trip;
    std::vector<uint8_t> animation_round_trip;
    CHECK(serialize_skeleton(reloaded_skeleton, skeleton_round_trip) && skeleton_round_trip == skeleton_bytes,
          "skeleton archive round trips exactly at Matter boundary");
    CHECK(serialize_animation(reloaded_animation, animation_round_trip) && animation_round_trip == animation_bytes,
          "animation archive round trips exactly at Matter boundary");

    OzzSampleContext context;
    std::vector<AnimationTransform> locals;
    CHECK(sample(reloaded_animation, 0.0f, context, locals), "samples clip start endpoint");
    CHECK(near(locals[0].translation.x, 0.0f), "start endpoint is exact");
    CHECK(sample(reloaded_animation, 1.0f, context, locals), "samples clip end endpoint");
    CHECK(near(locals[0].translation.x, 2.0f), "end endpoint is exact");
}

void test_normal_and_additive_blending() {
    std::vector<AnimationTransform> first = {transform(0.0f, 0.0f, 0.0f)};
    std::vector<AnimationTransform> second = {transform(4.0f, 0.0f, 0.0f)};
    std::vector<AnimationTransform> additive = {transform(2.0f, 0.0f, 0.0f)};
    std::vector<AnimationTransform> locals;
    CHECK(blend({{&first, 0.25f}, {&second, 0.75f}}, {}, locals), "blends normal layers");
    CHECK(near(locals[0].translation.x, 3.0f), "normal blend normalizes layer weights");
    CHECK(blend({{&first, 1.0f}}, {{&additive, 0.5f}}, locals), "blends additive layer");
    CHECK(near(locals[0].translation.x, 1.0f), "additive blend composes identity-relative translation");
}

void test_local_to_model_and_two_bone_subtree_refresh() {
    const RigDefinition rig_definition = chain_rig();
    Diagnostics diagnostics;
    OzzSkeleton skeleton;
    CHECK(build_skeleton(rig_definition, skeleton, diagnostics), "builds IK skeleton");
    std::vector<AnimationTransform> locals;
    for (const JointDef& joint : rig_definition.joints) locals.push_back(joint.local);
    std::vector<Mat4f> models;
    CHECK(local_to_model(skeleton, locals, models), "converts locals to model transforms");
    CHECK(near(models[4].m[3], 3.5f), "local-to-model composes the complete chain");
    const Mat4f old_tip_model = models[4];
    TwoBoneSolve solve;
    solve.skeleton = &skeleton;
    solve.start = 1;
    solve.mid = 2;
    solve.end = 3;
    solve.target = {1.0f, 1.5f, 0.0f};
    solve.pole_vector = {0.0f, 0.0f, 1.0f};
    solve.mid_axis = {0.0f, 0.0f, 1.0f};
    solve.affected = skeleton.subtree(1);
    std::vector<Mat4f> updated_models;
    CHECK(solve_two_bone(solve, models, locals, updated_models), "solves two-bone IK from model-space matrices");
    CHECK(updated_models.size() == locals.size(), "IK returns a complete canonical model palette");
    CHECK(!near(updated_models[4].m[3], old_tip_model.m[3]) || !near(updated_models[4].m[7], old_tip_model.m[7]),
          "IK recomputes the affected subtree through its tip");
}

void test_invalid_data_reports_matter_diagnostic() {
    OzzSkeleton skeleton;
    Diagnostics diagnostics;
    CHECK(!build_skeleton({}, skeleton, diagnostics), "invalid skeleton fails closed");
    CHECK(!diagnostics.items.empty(), "allocation or builder failure is exposed as Matter diagnostic");
}

} // namespace

int main() {
    test_build_archive_and_sample_endpoints();
    test_normal_and_additive_blending();
    test_local_to_model_and_two_bone_subtree_refresh();
    test_invalid_data_reports_matter_diagnostic();
    if (g_failures != 0) {
        std::printf("animation_ozz_adapter_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("animation_ozz_adapter_tests: all tests passed\n");
    return 0;
}
