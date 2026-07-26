// CPU contract tests for Phase C1 Vulkan skinning staging.

#include "check.h"
#include "render/vk_animation_skinning.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
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

static std::vector<VkSkinInfluence> valid_influences(uint32_t count) {
    std::vector<VkSkinInfluence> result(count);
    for (VkSkinInfluence& influence : result) influence.weight[0] = 65535;
    return result;
}

static void test_shader_abi_and_weight_decode() {
    CHECK(sizeof(VkSkinMatrix) == 64, "skin matrix is a std430 mat4");
    CHECK(alignof(VkSkinMatrix) == 16, "skin matrix keeps vec4 alignment");
    CHECK(offsetof(VkSkinMatrix, elements) == 0, "matrix elements begin at byte zero");
    CHECK(sizeof(VkSkinInfluence) == 16, "four u16 joints and weights are 16 bytes");
    CHECK(alignof(VkSkinInfluence) == alignof(uint16_t), "influence uses scalar alignment");
    CHECK(offsetof(VkSkinInfluence, joint) == 0, "joints begin at byte zero");
    CHECK(offsetof(VkSkinInfluence, weight) == 8, "weights start after four joints");
    CHECK(sizeof(VkSkinJoint) == 128, "two mat4 skin matrices are 128 bytes");
    CHECK(alignof(VkSkinJoint) == 16, "skin joints retain mat4 alignment");
    CHECK(offsetof(VkSkinJoint, position) == 0, "position matrix begins at byte zero");
    CHECK(offsetof(VkSkinJoint, normal) == 64, "normal matrix follows position matrix");
    CHECK(sizeof(VkSkinWorkItem) == 32, "work item is eight u32 fields");
    CHECK(alignof(VkSkinWorkItem) == alignof(uint32_t), "work item uses scalar alignment");
    CHECK(offsetof(VkSkinWorkItem, source_vertex) == 0, "source vertex ABI offset");
    CHECK(offsetof(VkSkinWorkItem, influence) == 4, "influence ABI offset");
    CHECK(offsetof(VkSkinWorkItem, vertex_count) == 8, "vertex count ABI offset");
    CHECK(offsetof(VkSkinWorkItem, palette) == 12, "palette ABI offset");
    CHECK(offsetof(VkSkinWorkItem, output_current) == 16, "current output ABI offset");
    CHECK(offsetof(VkSkinWorkItem, output_previous) == 20, "previous output ABI offset");
    CHECK(offsetof(VkSkinWorkItem, instance_slot) == 24, "instance slot ABI offset");
    CHECK(offsetof(VkSkinWorkItem, flags) == 28, "flags ABI offset");
    VkSkinInfluence influence{};
    influence.weight[0] = 32768;
    influence.weight[1] = 32767;
    CHECK(vk_skin_decode_weight(influence.weight[0]) + vk_skin_decode_weight(influence.weight[1]) == 1.0f,
          "UNORM16 weights sum exactly for complementary values");
}

static void test_asset_registration_and_visible_sorted_submission() {
    VkAnimationSkinning skinning(2);
    std::vector<VkSkinInfluence> influences = valid_influences(12);
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
    CHECK(skinning.register_asset(9, valid_influences(8)), "asset registers");
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

static void test_indexed_raster_mapping_tracks_sorted_output_offsets() {
    VkAnimationSkinning skinning(1);
    CHECK(skinning.register_asset(7, valid_influences(16)),
          "mapping fixture registers immutable influences");
    VkSkinSubmission later = candidate(7, 9, 3, 0, 4, 0);
    later.first_index = 30; later.index_count = 6;
    VkSkinSubmission first = candidate(7, 2, 5, 1, 1, 0);
    first.source_vertex = 11;
    first.influence_vertex = 4;
    first.first_index = 6; first.index_count = 9;
    CHECK(skinning.submit_visible(0, {later, first}),
          "indexed visible work publishes atomically");
    const auto& frame = skinning.frame(0);
    CHECK(frame.raster_draws.size() == 2 &&
              frame.raster_draws[0].instance_slot == 2 &&
              frame.raster_draws[0].output_vertex == 0 &&
              frame.raster_draws[0].source_vertex == 11 &&
              frame.work_items[0].influence == 4 &&
              frame.raster_draws[1].instance_slot == 9 &&
              frame.raster_draws[1].output_vertex == 5,
          "raster mappings follow stable work sorting and never retain stale offsets");
    VkSkinSubmission malformed = candidate(7, 3, 2, 0, 0, 0);
    malformed.first_index = 1; malformed.index_count = 5;
    CHECK(skinning.begin_frame(0, 0), "completed unsealed frame resets for malformed map");
    CHECK(skinning.submit_visible(0, {malformed}) &&
              skinning.frame(0).raster_draws.empty(),
          "non-triangle mapping cannot publish a raster draw and falls back safely");
}

static void test_fence_lifetime_wrap_and_transactional_caps() {
    VkAnimationSkinning skinning(2);
    CHECK(skinning.register_asset(1, valid_influences(kVkMaxSkinnedOutputVertices + 1)),
          "large asset registers");
    VkSkinSubmission one = candidate(1, 1, 1000000, 0, 0, 0);
    CHECK(skinning.submit_visible(0, {one}), "first frame owns its arena slices");
    CHECK(skinning.mark_submitted(0, 10), "first seal records the submitted fence");
    CHECK(!skinning.mark_submitted(0, 0),
          "duplicate stale seal is rejected instead of replacing an in-flight fence");
    CHECK(skinning.frame(0).in_flight && skinning.frame(0).submitted_fence == 10 &&
              skinning.frame(0).work_items.size() == 1,
          "rejected stale seal leaves the existing arena and fence intact");
    CHECK(!skinning.begin_frame(0, 0),
          "stale completion cannot reset the arena after a rejected stale seal");
    CHECK(!skinning.begin_frame(0, 9), "in-flight arena cannot wrap before its fence");
    CHECK(skinning.begin_frame(0, 10), "completed fence permits arena reuse");
    CHECK(skinning.submit_visible(0, {one}), "completed slot accepts a new allocation");
    CHECK(skinning.mark_submitted(0, 11), "reused slot seals with its next fence");
    CHECK(!skinning.begin_frame(0, 10),
          "reused slot stays sealed until its own newer fence completes");
    CHECK(skinning.begin_frame(0, 11), "reused slot resets only at its own completed fence");

    std::vector<VkSkinSubmission> too_many;
    for (uint32_t i = 0; i < kVkMaxSkinWorkItems + 1; ++i) too_many.push_back(candidate(1, i, 1, 0, 0, 0));
    CHECK(skinning.submit_visible(1, too_many), "work-item cap admits the deterministic priority prefix");
    CHECK(skinning.frame(1).work_items.size() == kVkMaxSkinWorkItems,
          "work cap leaves every admitted high-priority item in the queue");
    CHECK(skinning.frame(1).fallbacks.size() == 1 &&
              skinning.frame(1).fallbacks[0].mode == VkSkinFallbackMode::BindPose,
          "work cap emits a concrete bind-pose fallback only for the overflow tail");
    CHECK(skinning.begin_frame(1, 0), "unsealed partial-admission frame resets");
    CHECK(skinning.submit_visible(1, {candidate(1, 1, kVkMaxSkinnedOutputVertices + 1, 0, 0, 0)}),
          "vertex cap publishes a complete fallback-only transaction");
    CHECK(skinning.frame(1).work_items.empty(), "vertex cap leaves no partial queue");
    CHECK(skinning.frame(1).fallbacks.size() == 1 &&
              skinning.frame(1).fallbacks[0].mode == VkSkinFallbackMode::BindPose,
          "oversized first item emits a concrete bind-pose fallback");
    CHECK(!skinning.mark_submitted(1, 11),
          "a duplicate fence on another slot is rejected on the global submission timeline");
    CHECK(skinning.mark_submitted(1, 12),
          "a later fence seals an independent slot on the global timeline");
}

static void test_central_budget_controls_skinning_fallback_reason() {
    matter::animation::AnimationBudgetConfig budget;
    budget.max_skin_work_items = 2;
    budget.max_skinned_vertices = 4;
    VkAnimationSkinning skinning(1, budget);
    CHECK(skinning.register_asset(91, valid_influences(8)),
          "budget fixture registers immutable influences");
    VkSkinSubmission history = candidate(91, 3, 1, 0, 0, 0);
    history.history_valid = true;
    CHECK(skinning.submit_visible(0, {candidate(91, 1, 1, 0, 0, 0),
                                       candidate(91, 2, 1, 0, 0, 0),
                                       history}),
          "central work budget admits the stable high-priority prefix");
    CHECK(skinning.frame(0).work_items.size() == 2 && skinning.frame(0).fallbacks.size() == 1 &&
              skinning.frame(0).fallbacks[0].reason ==
                  matter::animation::AnimationFallbackReason::SkinWorkBudget &&
              skinning.frame(0).fallbacks[0].mode == VkSkinFallbackMode::BindPose,
          "history flags alone cannot claim a retained renderer output");
    CHECK(skinning.stats().fallback_count == 1 && skinning.fallback_count() == 1,
          "renderer staging exposes one shared fallback counter");
}

static void test_overflow_reuses_only_fence_owned_last_complete_raster_output() {
    matter::animation::AnimationBudgetConfig budget;
    budget.max_skin_work_items = 1;
    budget.max_skinned_vertices = 4;
    VkAnimationSkinning skinning(2, budget);
    CHECK(skinning.register_asset(92, valid_influences(8)),
          "retained-output fixture registers immutable influences");

    VkSkinSubmission retained = candidate(92, 3, 2, 0, 0, 0);
    retained.instance_generation = 7;
    retained.first_index = 12;
    retained.index_count = 6;
    CHECK(skinning.submit_visible(0, {retained}) &&
              skinning.mark_submitted(0, 20),
          "accepted frame seals a fence-owned skinned raster slice");

    VkSkinSubmission preferred = candidate(92, 9, 1, 10, 0, 0);
    preferred.instance_generation = 1;
    preferred.first_index = 0;
    preferred.index_count = 3;
    retained.history_valid = true;
    CHECK(skinning.submit_visible(1, {retained, preferred}),
          "next frame admits the priority prefix and falls back its tail");
    const auto& reused = skinning.frame(1);
    CHECK(reused.fallbacks.size() == 1 &&
              reused.fallbacks[0].mode == VkSkinFallbackMode::LastCompletePose &&
              reused.raster_draws.size() == 2,
          "overflow emits an actual retained raster draw alongside current work");
    const auto retained_draw = std::find_if(
        reused.raster_draws.begin(), reused.raster_draws.end(),
        [](const VkSkinRasterDraw& draw) { return draw.instance_slot == 3; });
    CHECK(retained_draw != reused.raster_draws.end() &&
              retained_draw->output_frame_slot == 0 &&
              retained_draw->instance_generation == 7,
          "last-complete fallback selects the sealed prior buffer slice");
    CHECK(!skinning.begin_frame(0, 20),
          "producer slot cannot recycle while an unsealed consumer references it");
    CHECK(skinning.mark_submitted(1, 21),
          "consumer frame seals its retained-source dependency");
    CHECK(!skinning.begin_frame(0, 20),
          "producer slot cannot recycle when only its own fence is complete");
    CHECK(skinning.begin_frame(0, 21),
          "producer slot recycles after every dependent consumer fence completes");

    VkSkinSubmission first_overflow = candidate(92, 4, 2, 0, 0, 0);
    first_overflow.instance_generation = 1;
    first_overflow.first_index = 24;
    first_overflow.index_count = 6;
    CHECK(skinning.begin_frame(1, 21) &&
              skinning.submit_visible(1, {first_overflow, preferred}),
          "first-frame overflow still publishes a complete fallback transaction");
    CHECK(skinning.frame(1).fallbacks.size() == 1 &&
              skinning.frame(1).fallbacks[0].mode == VkSkinFallbackMode::BindPose &&
              std::none_of(skinning.frame(1).raster_draws.begin(),
                           skinning.frame(1).raster_draws.end(),
                           [](const VkSkinRasterDraw& draw) {
                               return draw.instance_slot == 4;
                           }),
          "without retained output no skin draw can exclude the bind fallback from static culling");

    CHECK(skinning.begin_frame(1, 21) &&
              skinning.submit_visible(1, {retained, preferred}),
          "overflow after source-slot reuse remains valid");
    CHECK(skinning.frame(1).fallbacks[0].mode == VkSkinFallbackMode::BindPose &&
              std::none_of(skinning.frame(1).raster_draws.begin(),
                           skinning.frame(1).raster_draws.end(),
                           [](const VkSkinRasterDraw& draw) {
                               return draw.instance_slot == 3;
                           }),
          "reusing the source arena retires old metadata before it can alias overwritten vertices");
}

static void test_submission_rejects_invalid_influences_and_palettes_transactionally() {
    VkAnimationSkinning skinning(1);
    std::vector<VkSkinInfluence> invalid_joint = valid_influences(1);
    invalid_joint[0].joint[0] = 2;
    CHECK(skinning.register_asset(71, invalid_joint),
          "immutable malformed asset can be registered before its palette is known");
    VkSkinSubmission submission = candidate(71, 1, 1, 0, 0, 0);
    CHECK(!skinning.submit_visible(0, {submission}),
          "out-of-range nonzero influence rejects before a work queue allocation");
    CHECK(skinning.frame(0).work_items.empty() && skinning.frame(0).raster_draws.empty(),
          "invalid influence publishes no work or raster mapping");

    CHECK(skinning.begin_frame(0, 0), "unsealed rejected frame resets");
    std::vector<VkSkinInfluence> zero_weight(1);
    CHECK(skinning.register_asset(72, zero_weight), "zero-weight asset registers for submission validation");
    submission = candidate(72, 2, 1, 0, 0, 0);
    CHECK(!skinning.submit_visible(0, {submission}),
          "zero UNORM total weight rejects before queue allocation");
    CHECK(skinning.frame(0).work_items.empty() && skinning.frame(0).raster_draws.empty(),
          "zero-weight rejection leaves no partial queue");

    CHECK(skinning.begin_frame(0, 0), "second rejected frame resets");
    submission = candidate(71, 3, 1, 0, 0, 0);
    submission.pose.current[0].position.elements[0] = NAN;
    CHECK(!skinning.submit_visible(0, {submission}),
          "non-finite palette rejects before queue allocation");
    CHECK(skinning.frame(0).work_items.empty() && skinning.frame(0).raster_draws.empty(),
          "non-finite palette rejection leaves no partial queue");
}

static void test_cpu_skinning_matches_compute_contract() {
    VkSkinSourceVertex source{};
    source.position[0] = 1.0f;
    source.normal[1] = 1.0f;
    source.tint[0] = 0.25f; source.tint[3] = 1.0f;
    source.surface[2] = 0.75f; source.material_index = 9;
    VkSkinInfluence influence{};
    influence.joint[0] = 0; influence.joint[1] = 1;
    influence.weight[0] = 32768; influence.weight[1] = 32767;
    VkSkinJoint current[2]{};
    current[0].position = matrix(2.0f); current[0].normal = matrix();
    current[1].position = matrix(4.0f); current[1].normal = matrix();
    VkSkinJoint previous[2]{};
    previous[0].position = matrix(-1.0f); previous[0].normal = matrix();
    previous[1].position = matrix(1.0f); previous[1].normal = matrix();
    VkSkinVertex output{};
    CHECK(vk_skin_vertex_cpu(source, influence, current, previous, 2, output),
          "CPU reference skins a four-influence vertex");
    CHECK(std::fabs(output.position[0] - 4.0f) < 0.0001f,
          "UNORM weights blend current positions");
    CHECK(std::fabs(output.previous_position[0] - 1.0f) < 0.0001f,
          "previous position uses the prior palette");
    CHECK(std::fabs(output.normal[1] - 1.0f) < 0.0001f,
          "normal is inverse-transpose blended and normalized");
    CHECK(output.material_index == 9 && output.tint[0] == 0.25f &&
              output.surface[2] == 0.75f,
          "non-deforming vertex attributes are copied exactly");
    influence.joint[3] = 2;
    influence.weight[3] = 1;
    CHECK(!vk_skin_vertex_cpu(source, influence, current, previous, 2, output),
          "out-of-range nonzero influence fails closed");
}

int main() {
    test_shader_abi_and_weight_decode();
    test_asset_registration_and_visible_sorted_submission();
    test_current_previous_pair_and_history_fallback();
    test_indexed_raster_mapping_tracks_sorted_output_offsets();
    test_fence_lifetime_wrap_and_transactional_caps();
    test_central_budget_controls_skinning_fallback_reason();
    test_overflow_reuses_only_fence_owned_last_complete_raster_output();
    test_submission_rejects_invalid_influences_and_palettes_transactionally();
    test_cpu_skinning_matches_compute_contract();
    return check_summary();
}
