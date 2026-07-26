// C2 production handoff: immutable skinned assets, exact presentation
// snapshots, and the scene/dynamic-slot mapping must agree before any work
// reaches the Vulkan skin queue.

#include "check.h"
#include "animation/animation_systems.h"
#include "render/animation_skin_bridge.h"

using namespace matter;

namespace {

Mat4f identity(float translate_x = 0.0f) {
    Mat4f value{};
    value.m[0] = value.m[5] = value.m[10] = value.m[15] = 1.0f;
    value.m[3] = translate_x;
    return value;
}

AnimatorInstanceHandle animator() { return {3, 7, UINT32_MAX, static_cast<AnimationValueType>(0xff), AnimationCadence::Invalid}; }

animation::AnimationPoseSnapshot pose(AnimatorInstanceHandle handle,
                                      uint64_t serial,
                                      const Mat4f* current,
                                      const Mat4f* previous) {
    animation::AnimationPoseSnapshot value{};
    static const AnimationTransform local{};
    value.instance = handle;
    value.frame_serial = serial;
    value.local_pose = {&local, 1};
    value.model_pose = {current, 1};
    value.previous_model_pose = {previous, 1};
    value.skin_palette = {current, 1};
    value.previous_skin_palette = {previous, 1};
    return value;
}

std::vector<viewer::VkSkinInfluence> influences() {
    std::vector<viewer::VkSkinInfluence> result(3);
    for (auto& value : result) value.weight[0] = 65535;
    return result;
}

viewer::VkAnimationBoundsAsset bounds(uint64_t key) {
    viewer::VkAnimationBoundsAsset result{};
    result.asset_key = key;
    result.conservative_asset_bound = {{-10.0f, -10.0f, -10.0f}, {10.0f, 10.0f, 10.0f}};
    result.clusters.push_back({0, 0, {{0, {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}}}});
    return result;
}

void test_exact_snapshot_becomes_indexed_skin_submission() {
    animation::AnimationPoseSnapshotStore snapshots;
    const Mat4f current = identity(5.0f);
    const Mat4f previous = identity(2.0f);
    CHECK(snapshots.publish(pose(animator(), 41, &current, &previous)),
          "fixture publishes the exact render serial snapshot");

    render::AnimationSkinnedLod lod{};
    lod.part_hash = 0xabc;
    lod.source_vertex = 10;
    lod.influence_vertex = 0;
    lod.vertex_count = 3;
    lod.first_index = 30;
    lod.index_count = 3;
    std::vector<viewer::VkSkinInfluence> storage = influences();
    // The asset is immutable in production; retain the influence storage in
    // this fixture so the bridge can validate it before renderer registration.
    const auto asset_bounds = bounds(0x99);
    render::AnimationSkinnedAsset asset{0x99, 2, &storage, {lod}, &asset_bounds};
    render::AnimationSkinnedBinding binding{animator(), &asset, 2, 0, true};
    render::AnimationSkinBridge bridge(&snapshots);
    std::vector<viewer::VkSkinSubmission> out;
    const render::AnimationSkinExpansion input{{0x55, 4, 0}, 0xabc, 17, 41, binding, 6};
    CHECK(bridge.expand(input, out), "matching entity/part/LOD and fresh snapshot submit");
    CHECK(out.size() == 1 && out[0].asset_key == asset.identity &&
              out[0].source_vertex == 10 && out[0].influence_vertex == 0 &&
              out[0].vertex_count == 3 && out[0].first_index == 30 &&
              out[0].index_count == 3 && out[0].instance_slot == 17 &&
              out[0].instance_generation == 6,
          "submission retains the exact immutable source/indexed/transform-slot mapping");
    CHECK(out[0].history_valid && out[0].pose.current.size() == 1 &&
              out[0].pose.current[0].position.elements[12] == 5.0f &&
              out[0].pose.previous[0].position.elements[12] == 2.0f,
          "bridge converts current and previous palettes at the renderer boundary");
}

void test_stale_and_mismatched_bindings_fail_without_torn_work() {
    animation::AnimationPoseSnapshotStore snapshots;
    const Mat4f matrix = identity();
    CHECK(snapshots.publish(pose(animator(), 5, &matrix, &matrix)), "fixture snapshot publishes");
    std::vector<viewer::VkSkinInfluence> storage = influences();
    render::AnimationSkinnedLod lod{0xabc, 0, 0, 3, 0, 3};
    const auto asset_bounds = bounds(0x99);
    render::AnimationSkinnedAsset asset{0x99, 2, &storage, {lod}, &asset_bounds};
    render::AnimationSkinnedBinding binding{animator(), &asset, 2, 0, true};
    render::AnimationSkinBridge bridge(&snapshots);
    std::vector<viewer::VkSkinSubmission> out;
    const render::AnimationSkinExpansion stale{{1, 1, 0}, 0xabc, 2, 6, binding};
    CHECK(!bridge.expand(stale, out) && out.empty(), "stale presentation serial publishes no work");
    const render::AnimationSkinExpansion wrong_part{{1, 1, 0}, 0xdef, 2, 5, binding};
    CHECK(!bridge.expand(wrong_part, out) && out.empty(), "part replacement cannot reuse an old mapping");
    binding.asset_generation = 3;
    const render::AnimationSkinExpansion stale_asset{{1, 1, 0}, 0xabc, 2, 5, binding};
    CHECK(!bridge.expand(stale_asset, out) && out.empty(), "stale immutable asset generation fails closed");
}

void test_bridge_emits_all_baked_lod_candidates_for_current_cull() {
    animation::AnimationPoseSnapshotStore snapshots;
    const Mat4f matrix = identity();
    CHECK(snapshots.publish(pose(animator(), 9, &matrix, &matrix)),
          "multi-LOD fixture publishes the exact current snapshot");
    std::vector<viewer::VkSkinInfluence> storage(6);
    for (auto& influence : storage) influence.weight[0] = 65535;
    render::AnimationSkinnedLod near{0xabc, 10, 0, 3, 30, 6, 0, 0};
    render::AnimationSkinnedLod far{0xabc, 40, 3, 3, 90, 3, 0, 1};
    const auto asset_bounds = bounds(0x99);
    render::AnimationSkinnedAsset asset{0x99, 2, &storage, {near, far},
                                        &asset_bounds};
    // This presentation LOD must not preselect mesh LOD 0. The renderer's
    // current bounds/frustum/cluster planner receives both exact ranges.
    render::AnimationSkinnedBinding binding{animator(), &asset, 2, 0, true};
    render::AnimationSkinBridge bridge(&snapshots);
    std::vector<viewer::VkSkinSubmission> out;
    const render::AnimationSkinExpansion input{{1, 1, 0}, 0xabc, 2, 9,
                                                binding, 4};
    CHECK(bridge.expand(input, out) && out.size() == 2,
          "bridge publishes every independently baked mesh LOD candidate");
    CHECK(out[0].lod == 0 && out[0].source_vertex == 10 &&
              out[0].influence_vertex == 0 && out[0].first_index == 30 &&
              out[0].index_count == 6 && out[1].lod == 1 &&
              out[1].source_vertex == 40 && out[1].influence_vertex == 3 &&
              out[1].first_index == 90 && out[1].index_count == 3,
          "near/far candidates retain their own global source, influence, and index ranges");
}

} // namespace

int main() {
    test_exact_snapshot_becomes_indexed_skin_submission();
    test_stale_and_mismatched_bindings_fail_without_torn_work();
    test_bridge_emits_all_baked_lod_candidates_for_current_cull();
    return check_summary();
}
