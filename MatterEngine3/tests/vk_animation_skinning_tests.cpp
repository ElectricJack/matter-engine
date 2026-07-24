// CPU contract tests for Phase C1 Vulkan skinning staging.

#include "check.h"
#include "render/vk_animation_skinning.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace viewer;

static VkSkinMatrix matrix(float translate_x = 0.0f) {
    VkSkinMatrix out{};
    out.elements[0] = out.elements[5] = out.elements[10] = out.elements[15] = 1.0f;
    out.elements[12] = translate_x;
    return out;
}

static VkSkinPose pose(uint32_t joints, float translate_x = 0.0f) {
    VkSkinPose value{};
    value.current.assign(joints, {});
    value.previous.assign(joints, {});
    for (uint32_t i = 0; i < joints; ++i) {
        value.current[i].position = matrix(translate_x);
        value.current[i].normal = matrix();
        value.previous[i] = value.current[i];
    }
    return value;
}

static VkSkinSubmission candidate(uint64_t asset, uint32_t slot,
                                  uint32_t vertices, int priority,
                                  uint32_t distance, uint32_t lod) {
    VkSkinSubmission result{};
    result.asset_key = asset;
    result.instance_slot = slot;
    result.source_vertex = 0;
    result.vertex_count = vertices;
    result.render_priority = priority;
    result.distance_bucket = distance;
    result.lod = lod;
    result.pose = pose(2);
    return result;
}

static void test_shader_abi_and_weight_decode() {
    CHECK(sizeof(VkSkinMatrix) == 64, "skin matrix is a std430 mat4");
    CHECK(alignof(VkSkinMatrix) == 16, "skin matrix keeps vec4 alignment");
    CHECK(sizeof(VkSkinInfluence) == 16, "four u16 joints and weights are 16 bytes");
    CHECK(offsetof(VkSkinInfluence, weight) == 8, "weights start after four joints");
    CHECK(sizeof(VkSkinJoint) == 128, "two mat4 skin matrices are 128 bytes");
    CHECK(offsetof(VkSkinJoint, normal) == 64, "normal matrix follows position matrix");
    CHECK(sizeof(VkSkinWorkItem) == 32, "work item is eight u32 fields");
    CHECK(offsetof(VkSkinWorkItem, output_previous) == 20, "previous output ABI offset");
    VkSkinInfluence influence{};
    influence.weight[0] = 32768;
    influence.weight[1] = 32767;
    CHECK(vk_skin_decode_weight(influence.weight[0]) + vk_skin_decode_weight(influence.weight[1]) == 1.0f,
          "UNORM16 weights sum exactly for complementary values");
}

static void test_asset_registration_and_visible_sorted_submission() {
    VkAnimationSkinning skinning(2);
    std::vector<VkSkinInfluence> influences(12);
    CHECK(skinning.register_asset(44, influences), "immutable influence asset registers");
    CHECK(skinning.register_asset(44, influences), "same immutable asset deduplicates");
    influences.resize(13);
    CHECK(!skinning.register_asset(44, influences), "different payload cannot replace immutable asset");

    std::vector<VkSkinSubmission> visible;
    visible.push_back(candidate(44, 8, 4, 1, 2, 1));
    visible.push_back(candidate(44, 7, 4, 2, 5, 0));
    visible.push_back(candidate(44, 6, 4, 2, 3, 2));
    visible.push_back(candidate(44, 5, 4, 2, 3, 1));
    CHECK(skinning.submit_visible(0, visible), "visible candidates submit");
    const auto& queue = skinning.frame(0).work_items;
    CHECK(queue.size() == 4, "all visible candidates accepted");
    CHECK(queue[0].instance_slot == 5 && queue[1].instance_slot == 6 &&
              queue[2].instance_slot == 7 && queue[3].instance_slot == 8,
          "queue sort is priority, distance, slot, then LOD");
}

static void test_current_previous_pair_and_history_fallback() {
    VkAnimationSkinning skinning(2);
    CHECK(skinning.register_asset(9, std::vector<VkSkinInfluence>(8)), "asset registers");
    VkSkinSubmission fresh = candidate(9, 3, 8, 0, 0, 0);
    fresh.history_valid = false;
    fresh.pose = pose(2, 4.0f);
    CHECK(skinning.submit_visible(0, {fresh}), "newly visible candidate submits");
    const auto& state = skinning.frame(0);
    CHECK(state.work_items.size() == 1 &&
              (state.work_items[0].flags & kVkSkinHistoryInvalid) != 0,
          "newly visible work marks history invalid");
    CHECK(state.palette_current.size() == state.palette_previous.size(),
          "current and previous palette allocations are paired");
    CHECK(state.palette_current[0].position.elements[12] ==
              state.palette_previous[0].position.elements[12],
          "newly visible instance uses current palette as previous history");
}

static void test_fence_lifetime_wrap_and_transactional_caps() {
    VkAnimationSkinning skinning(2);
    CHECK(skinning.register_asset(1, std::vector<VkSkinInfluence>(2000000)), "large asset registers");
    VkSkinSubmission one = candidate(1, 1, 1000000, 0, 0, 0);
    CHECK(skinning.submit_visible(0, {one}), "first frame owns its arena slices");
    skinning.mark_submitted(0, 10);
    CHECK(!skinning.begin_frame(0, 9), "in-flight arena cannot wrap before its fence");
    CHECK(skinning.begin_frame(0, 10), "completed fence permits arena reuse");

    std::vector<VkSkinSubmission> too_many;
    for (uint32_t i = 0; i < kVkMaxSkinWorkItems + 1; ++i) too_many.push_back(candidate(1, i, 1, 0, 0, 0));
    CHECK(!skinning.submit_visible(1, too_many), "work-item cap rejects transactionally");
    CHECK(skinning.frame(1).work_items.empty(), "work cap leaves no partial queue");
    CHECK(skinning.frame(1).fallbacks.size() == kVkMaxSkinWorkItems + 1,
          "work cap emits one deterministic fallback per rejected visible item");
    CHECK(!skinning.submit_visible(1, {candidate(1, 1, kVkMaxSkinnedOutputVertices + 1, 0, 0, 0)}),
          "vertex cap rejects transactionally");
    CHECK(skinning.frame(1).work_items.empty(), "vertex cap leaves no partial queue");
    CHECK(skinning.frame(1).fallbacks.size() == 1,
          "vertex cap emits a deterministic bind-or-last-pose fallback");
}

int main() {
    test_shader_abi_and_weight_decode();
    test_asset_registration_and_visible_sorted_submission();
    test_current_previous_pair_and_history_fallback();
    test_fence_lifetime_wrap_and_transactional_caps();
    return check_summary();
}
