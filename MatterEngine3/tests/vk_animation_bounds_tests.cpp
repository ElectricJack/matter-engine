#include "render/vk_animation_bounds.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace viewer;

namespace {

int failures = 0;

#define CHECK(condition, message) \
    do { if (!(condition)) { std::fprintf(stderr, "FAIL: %s\\n", message); ++failures; } } while (false)

VkSkinMatrix translate(float x, float y, float z) {
    VkSkinMatrix out{};
    out.elements[0] = out.elements[5] = out.elements[10] = out.elements[15] = 1.0f;
    out.elements[12] = x; out.elements[13] = y; out.elements[14] = z;
    return out;
}

VkSkinPose pose(float current_x, float previous_x) {
    VkSkinPose result{};
    VkSkinJoint current{}; current.position = translate(current_x, 0.0f, 0.0f);
    VkSkinJoint previous{}; previous.position = translate(previous_x, 0.0f, 0.0f);
    result.current.push_back(current);
    result.previous.push_back(previous);
    return result;
}

void transform(const VkSkinMatrix& matrix, const float input[3], float output[3]) {
    for (uint32_t row = 0; row != 3; ++row)
        output[row] = matrix.elements[row] * input[0] +
                      matrix.elements[4 + row] * input[1] +
                      matrix.elements[8 + row] * input[2] +
                      matrix.elements[12 + row];
}

bool contains(const VkAnimationBoundsAabb& aabb, const float point[3]) {
    constexpr float epsilon = 1e-4f;
    return point[0] >= aabb.min[0] - epsilon && point[0] <= aabb.max[0] + epsilon &&
           point[1] >= aabb.min[1] - epsilon && point[1] <= aabb.max[1] + epsilon &&
           point[2] >= aabb.min[2] - epsilon && point[2] <= aabb.max[2] + epsilon;
}

VkAnimationBoundsAsset asset(uint64_t key) {
    VkAnimationBoundsAsset result{};
    result.asset_key = key;
    result.conservative_asset_bound = {{-10.0f, -10.0f, -10.0f}, {10.0f, 10.0f, 10.0f}};
    VkAnimationBoundsCluster cluster{};
    cluster.cluster_index = 2;
    cluster.lod = 1;
    cluster.joints.push_back({0, {{-1.0f, -2.0f, -3.0f}, {1.0f, 2.0f, 3.0f}}});
    result.clusters.push_back(cluster);
    return result;
}

void test_current_and_previous_are_unioned() {
    VkAnimationBounds bounds;
    CHECK(bounds.register_asset(asset(7)), "register valid bounds asset");
    CHECK(bounds.update_instance(4, 1, 7, pose(100.0f, -50.0f), true),
          "publish current and previous bounds");
    const auto& dynamic = bounds.dynamic_bounds();
    CHECK(dynamic.size() == 1, "one dynamic cluster bound");
    CHECK(dynamic[0].key.instance_slot == 4 && dynamic[0].key.instance_generation == 1 && dynamic[0].key.cluster_index == 2 &&
              dynamic[0].key.lod == 1, "dynamic key is instance/cluster/lod");
    CHECK(dynamic[0].aabb.min[0] <= -51.0f && dynamic[0].aabb.max[0] >= 101.0f,
          "temporal union contains both transformed cluster boxes");
    CHECK(dynamic[0].occlusion_enabled, "valid dynamic bounds allow occlusion");
}

void test_missing_history_uses_current_not_stale_previous() {
    VkAnimationBounds bounds;
    CHECK(bounds.register_asset(asset(8)), "register asset for history test");
    CHECK(bounds.update_instance(9, 3, 8, pose(20.0f, -999.0f), false),
          "publish missing-history current bounds");
    const auto& value = bounds.dynamic_bounds()[0];
    CHECK(value.aabb.min[0] >= 19.0f && value.aabb.max[0] >= 21.0f,
          "missing history cannot include stale previous pose");
}

void test_corrupt_or_missing_pose_fails_open_to_asset_bound() {
    VkAnimationBounds bounds;
    CHECK(bounds.register_asset(asset(9)), "register fallback asset");
    VkSkinPose corrupt{};
    CHECK(!bounds.update_instance(1, 2, 9, corrupt, false), "empty pose is rejected");
    const auto& fallback = bounds.dynamic_bounds();
    CHECK(fallback.size() == 1 && !fallback[0].occlusion_enabled,
          "missing pose disables occlusion rather than using a stale smaller bound");
    CHECK(fallback[0].aabb.min[0] == -10.0f && fallback[0].aabb.max[0] == 10.0f,
          "missing pose uses conservative asset bound");
    const auto gpu = bounds.gpu_records();
    CHECK(gpu.size() == 1 && gpu[0].flags == 0,
          "fallback record explicitly disables cull occlusion");
}

void test_rejects_empty_influences_and_preserves_last_complete_bounds() {
    VkAnimationBounds bounds;
    VkAnimationBoundsAsset invalid = asset(10);
    invalid.clusters[0].joints.clear();
    CHECK(!bounds.register_asset(invalid), "empty influence cluster is an invalid asset");
    CHECK(bounds.register_asset(asset(11)), "register valid asset for frozen bound test");
    CHECK(bounds.update_instance(2, 1, 11, pose(3.0f, 2.0f), true), "publish complete pose");
    const auto complete = bounds.dynamic_bounds()[0].aabb;
    VkSkinPose corrupt{};
    CHECK(!bounds.update_instance(2, 1, 11, corrupt, false), "reject corrupt frozen pose");
    const auto& retained = bounds.dynamic_bounds()[0];
    CHECK(retained.occlusion_enabled && retained.aabb.min[0] == complete.min[0] &&
              retained.aabb.max[0] == complete.max[0],
          "frozen pose retains matching last-complete dynamic bounds");
    CHECK((bounds.gpu_records()[0].flags & kVkAnimationBoundsOcclusionEnabled) != 0,
          "matching frozen bound remains eligible for normal culling");
}

void test_static_clusters_keep_static_path() {
    VkAnimationBounds bounds;
    CHECK(!bounds.has_dynamic_bound({42, 0, 3, 0}), "unregistered/static cluster has no dynamic override");
}

void test_random_pose_bound_contains_every_brute_force_corner_and_lod() {
    VkAnimationBounds bounds;
    VkAnimationBoundsAsset multi = asset(12);
    VkAnimationBoundsCluster lod0 = multi.clusters[0];
    lod0.lod = 0;
    lod0.joints[0].aabb = {{-2.0f, -1.0f, -1.0f}, {2.0f, 1.0f, 1.0f}};
    multi.clusters.push_back(lod0);
    CHECK(bounds.register_asset(multi), "register multi-LOD random test asset");
    uint32_t state = 0x12345678u;
    for (uint32_t iteration = 0; iteration != 32; ++iteration) {
        state = state * 1664525u + 1013904223u;
        const float current_x = static_cast<float>(state & 0xffu) * 0.125f - 16.0f;
        state = state * 1664525u + 1013904223u;
        const float previous_x = static_cast<float>(state & 0xffu) * 0.125f - 16.0f;
        const VkSkinPose animated = pose(current_x, previous_x);
        CHECK(bounds.update_instance(19, 7, 12, animated, true),
              "publish deterministic random pose");
        const auto& values = bounds.dynamic_bounds();
        CHECK(values.size() == 2, "each serialized LOD receives a separate bound");
        for (const auto& dynamic : values) {
            const VkAnimationBoundsAabb local = dynamic.key.lod == 0
                ? lod0.joints[0].aabb : multi.clusters[0].joints[0].aabb;
            for (uint32_t corner = 0; corner != 8; ++corner) {
                const float point[3]{(corner & 4u) ? local.max[0] : local.min[0],
                                     (corner & 2u) ? local.max[1] : local.min[1],
                                     (corner & 1u) ? local.max[2] : local.min[2]};
                float current[3]{};
                float previous[3]{};
                transform(animated.current[0].position, point, current);
                transform(animated.previous[0].position, point, previous);
                CHECK(contains(dynamic.aabb, current) && contains(dynamic.aabb, previous),
                      "conservative dynamic AABB contains brute-force animated vertices");
            }
        }
    }
}

void test_recycled_slot_generation_never_reuses_old_complete_bound() {
    VkAnimationBounds bounds;
    CHECK(bounds.register_asset(asset(13)), "register recycled-slot asset");
    CHECK(bounds.update_instance(6, 1, 13, pose(40.0f, 39.0f), true),
          "old incarnation publishes a complete bound");
    VkSkinPose corrupt{};
    CHECK(!bounds.update_instance(6, 2, 13, corrupt, false),
          "new incarnation rejects missing pose");
    const auto& current = bounds.dynamic_bounds();
    const auto fresh = std::find_if(current.begin(), current.end(),
                                    [](const VkAnimationDynamicClusterBound& value) {
                                        return value.key.instance_slot == 6 &&
                                               value.key.instance_generation == 2;
                                    });
    CHECK(fresh != current.end() && !fresh->occlusion_enabled &&
              fresh->aabb.min[0] == -10.0f && fresh->aabb.max[0] == 10.0f,
          "recycled slot fails open to asset bound rather than old incarnation's box");
}

}  // namespace

int main() {
    test_current_and_previous_are_unioned();
    test_missing_history_uses_current_not_stale_previous();
    test_corrupt_or_missing_pose_fails_open_to_asset_bound();
    test_rejects_empty_influences_and_preserves_last_complete_bounds();
    test_static_clusters_keep_static_path();
    test_random_pose_bound_contains_every_brute_force_corner_and_lod();
    test_recycled_slot_generation_never_reuses_old_complete_bound();
    return failures == 0 ? 0 : 1;
}
