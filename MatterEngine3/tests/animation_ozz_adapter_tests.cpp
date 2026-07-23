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
    std::vector<AnimationTransform> first(5, transform(0.0f, 0.0f, 0.0f));
    std::vector<AnimationTransform> second(5, transform(0.0f, 0.0f, 0.0f)); second[0] = transform(4.0f, 0.0f, 0.0f);
    std::vector<AnimationTransform> additive(5, transform(0.0f, 0.0f, 0.0f)); additive[0] = transform(2.0f, 0.0f, 0.0f);
    std::vector<AnimationTransform> locals;
    Diagnostics diagnostics;
    OzzSkeleton skeleton;
    CHECK(build_skeleton(chain_rig(), skeleton, diagnostics), "builds rest pose for blending");
    CHECK(blend(skeleton, {{&first, 0.25f}, {&second, 0.75f}}, {}, locals), "blends normal layers");
    CHECK(near(locals[0].translation.x, 3.0f), "normal blend normalizes layer weights");
    CHECK(blend(skeleton, {{&first, 1.0f}}, {{&additive, 0.5f}}, locals), "blends additive layer");
    CHECK(near(locals[0].translation.x, 1.0f), "additive blend composes identity-relative translation");

    std::vector<AnimationTransform> bind_fallback = {transform(0.0f, 0.0f, 0.0f), transform(0.0f, 0.0f, 0.0f), transform(0.0f, 0.0f, 0.0f), transform(0.0f, 0.0f, 0.0f), transform(0.0f, 0.0f, 0.0f)};
    CHECK(blend(skeleton, {{&bind_fallback, 0.01f}}, {}, locals), "blends a layer below the fallback threshold");
    CHECK(near(locals[0].translation.x, 0.0f) && locals[1].translation.x > 0.5f,
          "low normal weight falls back to the skeleton bind pose rather than identity");
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

    TwoBoneSolve out_of_bounds = solve;
    out_of_bounds.end = static_cast<JointIndex>(skeleton.joint_count());
    CHECK(!solve_two_bone(out_of_bounds, models, locals, updated_models), "IK rejects every out-of-bounds joint index before indexing pose arrays");
    TwoBoneSolve non_chain = solve;
    non_chain.mid = 3;
    non_chain.end = 2;
    CHECK(!solve_two_bone(non_chain, models, locals, updated_models), "IK rejects a non-ancestral start-mid-end chain");
    TwoBoneSolve duplicate_start_mid = solve;
    duplicate_start_mid.mid = duplicate_start_mid.start;
    CHECK(!solve_two_bone(duplicate_start_mid, models, locals, updated_models), "IK rejects duplicate start and mid joints");
    TwoBoneSolve duplicate_mid_end = solve;
    duplicate_mid_end.end = duplicate_mid_end.mid;
    CHECK(!solve_two_bone(duplicate_mid_end, models, locals, updated_models), "IK rejects duplicate mid and end joints");
    TwoBoneSolve skipped_intermediate = solve;
    skipped_intermediate.mid = 3;
    skipped_intermediate.end = 4;
    CHECK(!solve_two_bone(skipped_intermediate, models, locals, updated_models), "IK rejects a chain that skips an intermediate joint");

    std::vector<Mat4f> partial_models = models;
    const Mat4f root_before = partial_models[0];
    locals[2].translation.y = 0.25f;
    CHECK(local_to_model(skeleton, locals, partial_models, skeleton.subtree(1)), "updates an exact compiled subtree range");
    CHECK(near(partial_models[0].m[3], root_before.m[3]) && near(partial_models[0].m[7], root_before.m[7]),
          "partial local-to-model preserves unaffected models");
    CHECK(!local_to_model(skeleton, locals, partial_models, {1, 3}), "rejects noncanonical partial ranges");
    CHECK(!local_to_model(skeleton, locals, partial_models, {0, 4}), "rejects a root-origin range that is not the full compiled subtree");
    CHECK(!local_to_model(skeleton, locals, partial_models, {kInvalidJoint, 4}), "rejects a default-begin partial range");
    CHECK(!local_to_model(skeleton, locals, partial_models, {1, kInvalidJoint}), "rejects a partial-begin default-end range");
    CHECK(local_to_model(skeleton, locals, partial_models, skeleton.subtree(0)), "accepts the exact root compiled subtree range");
    std::vector<Mat4f> undersized;
    CHECK(!local_to_model(skeleton, locals, undersized, skeleton.subtree(1)), "partial local-to-model requires the parent model palette");

    std::vector<AnimationTransform> rotated_locals;
    for (const JointDef& joint : rig_definition.joints) rotated_locals.push_back(joint.local);
    rotated_locals[1].rotation = {0.70710677f, 0.0f, 0.0f, 0.70710677f};
    std::vector<Mat4f> rotated_models;
    CHECK(local_to_model(skeleton, rotated_locals, rotated_models), "builds a non-identity local rotation IK pose");
    TwoBoneSolve rotated_solve = solve;
    rotated_solve.target = {1.0f, 1.5f, 0.0f};
    CHECK(solve_two_bone(rotated_solve, rotated_models, rotated_locals, updated_models), "solves IK with a non-identity local rotation");
    CHECK(near(updated_models[3].m[3], rotated_solve.target.x, 2e-2f) && near(updated_models[3].m[7], rotated_solve.target.y, 2e-2f),
          "IK composes ozz corrections after the local rotation");
}

void test_archive_boundaries_and_skeleton_limits() {
    Diagnostics diagnostics;
    OzzSkeleton skeleton;
    OzzAnimation animation;
    CHECK(build_skeleton(chain_rig(), skeleton, diagnostics), "builds archive fixture skeleton");
    CHECK(build_clip(chain_rig(), moving_clip(), animation, diagnostics), "builds archive fixture animation");
    std::vector<uint8_t> skeleton_bytes;
    std::vector<uint8_t> animation_bytes;
    CHECK(serialize_skeleton(skeleton, skeleton_bytes), "serializes skeleton boundary fixture");
    CHECK(serialize_animation(animation, animation_bytes), "serializes animation boundary fixture");
    OzzSkeleton loaded_skeleton;
    OzzAnimation loaded_animation;
    std::vector<uint8_t> truncated(skeleton_bytes.begin(), skeleton_bytes.end() - 1);
    CHECK(!deserialize_skeleton(truncated.data(), truncated.size(), loaded_skeleton, diagnostics), "rejects truncated skeleton archive");
    std::vector<uint8_t> wrong_tag = skeleton_bytes; wrong_tag[0] = 'X';
    CHECK(!deserialize_skeleton(wrong_tag.data(), wrong_tag.size(), loaded_skeleton, diagnostics), "rejects wrong skeleton tag");
    std::vector<uint8_t> trailing_skeleton = skeleton_bytes; trailing_skeleton.push_back(0);
    CHECK(!deserialize_skeleton(trailing_skeleton.data(), trailing_skeleton.size(), loaded_skeleton, diagnostics), "rejects trailing skeleton archive bytes");
    std::vector<uint8_t> bad_parent = skeleton_bytes; bad_parent[6] = 1;
    CHECK(!deserialize_skeleton(bad_parent.data(), bad_parent.size(), loaded_skeleton, diagnostics), "rejects malformed skeleton parent metadata");
    std::vector<uint8_t> bad_subtree = skeleton_bytes; bad_subtree[8] = 1;
    CHECK(!deserialize_skeleton(bad_subtree.data(), bad_subtree.size(), loaded_skeleton, diagnostics), "rejects malformed skeleton subtree metadata");
    std::vector<uint8_t> trailing_animation = animation_bytes; trailing_animation.push_back(0);
    CHECK(!deserialize_animation(trailing_animation.data(), trailing_animation.size(), loaded_animation, diagnostics), "rejects trailing animation archive bytes");
    std::vector<uint8_t> truncated_animation(animation_bytes.begin(), animation_bytes.end() - 1);
    CHECK(!deserialize_animation(truncated_animation.data(), truncated_animation.size(), loaded_animation, diagnostics), "rejects truncated animation archive bytes");
    std::vector<uint8_t> wrong_animation_tag = animation_bytes; wrong_animation_tag[0] = 'X';
    CHECK(!deserialize_animation(wrong_animation_tag.data(), wrong_animation_tag.size(), loaded_animation, diagnostics), "rejects wrong animation tag");

    RigDefinition too_many;
    too_many.joints.reserve(kMaxJoints + 1);
    for (uint32_t i = 0; i <= kMaxJoints; ++i) {
        too_many.joints.push_back({"joint" + std::to_string(i), i == 0 ? "" : "joint" + std::to_string(i - 1), transform(0.0f, 0.0f, 0.0f), 1.0f, source("limit")});
    }
    CHECK(!build_skeleton(too_many, loaded_skeleton, diagnostics), "rejects a rig with more than 256 joints");
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
    test_archive_boundaries_and_skeleton_limits();
    test_invalid_data_reports_matter_diagnostic();
    if (g_failures != 0) {
        std::printf("animation_ozz_adapter_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("animation_ozz_adapter_tests: all tests passed\n");
    return 0;
}
