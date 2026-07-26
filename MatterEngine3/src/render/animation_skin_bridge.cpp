#include "render/animation_skin_bridge.h"

#include <cmath>
#include <limits>

namespace matter::render {
namespace {

bool finite_matrix(const Mat4f& value) noexcept {
    for (float element : value.m)
        if (!std::isfinite(element)) return false;
    return true;
}

bool inverse(const Mat4f& source, Mat4f& out) noexcept {
    double values[4][8]{};
    for (uint32_t row = 0; row != 4; ++row) {
        for (uint32_t column = 0; column != 4; ++column) {
            values[row][column] = source.m[row * 4u + column];
            if (!std::isfinite(values[row][column])) return false;
        }
        values[row][row + 4u] = 1.0;
    }
    for (uint32_t column = 0; column != 4; ++column) {
        uint32_t pivot = column;
        for (uint32_t row = column + 1u; row != 4; ++row)
            if (std::fabs(values[row][column]) > std::fabs(values[pivot][column])) pivot = row;
        if (!(std::fabs(values[pivot][column]) > 1e-12) ||
            !std::isfinite(values[pivot][column])) return false;
        if (pivot != column)
            for (uint32_t index = 0; index != 8; ++index)
                std::swap(values[pivot][index], values[column][index]);
        const double divisor = values[column][column];
        for (uint32_t index = 0; index != 8; ++index) values[column][index] /= divisor;
        for (uint32_t row = 0; row != 4; ++row) {
            if (row == column) continue;
            const double factor = values[row][column];
            for (uint32_t index = 0; index != 8; ++index)
                values[row][index] -= factor * values[column][index];
        }
    }
    for (uint32_t row = 0; row != 4; ++row)
        for (uint32_t column = 0; column != 4; ++column) {
            const float value = static_cast<float>(values[row][column + 4u]);
            if (!std::isfinite(value)) return false;
            out.m[row * 4u + column] = value;
        }
    return true;
}

bool pack_joint(const Mat4f& position, viewer::VkSkinJoint& out) noexcept {
    Mat4f inverse_position{};
    if (!finite_matrix(position) || !inverse(position, inverse_position)) return false;
    // Matter matrices are row-major/column-vector; GLSL storage is
    // column-major.  The normal transform is transpose(inverse(position)).
    for (uint32_t row = 0; row != 4; ++row)
        for (uint32_t column = 0; column != 4; ++column) {
            out.position.elements[column * 4u + row] = position.m[row * 4u + column];
            out.normal.elements[column * 4u + row] = inverse_position.m[column * 4u + row];
        }
    return true;
}

bool valid_lod(const AnimationSkinnedLod& lod,
               const std::vector<viewer::VkSkinInfluence>& influences) noexcept {
    if (lod.part_hash == 0 || lod.vertex_count == 0 || lod.index_count == 0 ||
        lod.index_count % 3u != 0u || lod.source_vertex > std::numeric_limits<uint32_t>::max() - lod.vertex_count ||
        lod.influence_vertex > influences.size() ||
        lod.vertex_count > influences.size() - lod.influence_vertex) return false;
    for (uint32_t index = lod.influence_vertex;
         index != lod.influence_vertex + lod.vertex_count; ++index) {
        uint32_t total = 0;
        for (uint16_t weight : influences[index].weight) total += weight;
        if (total == 0) return false;
    }
    return true;
}

}  // namespace

bool valid_animation_skinned_asset(const AnimationSkinnedAsset& asset) noexcept {
    if (asset.identity == 0 || asset.generation == 0 || asset.influences == nullptr ||
        asset.influences->empty() || asset.lods.empty() || asset.bounds == nullptr ||
        asset.bounds->asset_key != asset.identity ||
        !viewer::valid_animation_bounds_asset(*asset.bounds)) return false;
    for (const AnimationSkinnedLod& lod : asset.lods)
        if (!valid_lod(lod, *asset.influences)) return false;
    return true;
}

bool AnimationSkinBridge::expand(
    const AnimationSkinExpansion& input,
    std::vector<viewer::VkSkinSubmission>& out) const {
    const AnimationSkinnedBinding& binding = input.binding;
    if (!binding.visible) return true;
    const AnimationSkinnedAsset* asset = binding.asset;
    if (!snapshots_ || !input.entity.entity_id || input.entity.binding_index != 0 ||
        input.part_hash == 0 || input.transform_slot == UINT32_MAX || input.frame_serial == 0 ||
        !binding.animator.valid() || !asset ||
        binding.asset_generation != asset->generation || !valid_animation_skinned_asset(*asset) ||
        binding.lod >= asset->lods.size()) return false;
    const animation::AnimationPoseSnapshot pose =
        snapshots_->snapshot(binding.animator, input.frame_serial);
    if (!pose.instance.valid() || pose.skin_palette.empty() ||
        pose.previous_skin_palette.count != pose.skin_palette.count ||
        pose.skin_palette.count > viewer::kVkSkinPaletteCountMax) return false;

    viewer::VkSkinPose converted{};
    converted.current.resize(pose.skin_palette.count);
    converted.previous.resize(pose.skin_palette.count);
    for (uint32_t joint = 0; joint != pose.skin_palette.count; ++joint)
        if (!pack_joint(pose.skin_palette[joint], converted.current[joint]) ||
            !pack_joint(pose.previous_skin_palette[joint], converted.previous[joint])) return false;

    // Presentation LOD controls pose evaluation cadence, not geometry.
    // Publish every immutable mesh candidate and let the renderer compact it
    // against this frame's animated bounds/frustum/cluster LOD decision.
    bool emitted = false;
    for (uint32_t lod_index = 0; lod_index < asset->lods.size(); ++lod_index) {
        const AnimationSkinnedLod& lod = asset->lods[lod_index];
        if (lod.part_hash != input.part_hash) continue;
        viewer::VkSkinSubmission submission{};
        submission.asset_key = asset->identity;
        submission.influence_vertex = lod.influence_vertex;
        submission.source_vertex = lod.source_vertex;
        submission.vertex_count = lod.vertex_count;
        submission.instance_slot = input.transform_slot;
        submission.instance_generation = input.transform_generation;
        submission.render_priority = binding.presentation_priority;
        submission.lod = lod.lod;
        submission.cluster = lod.cluster;
        submission.first_index = lod.first_index;
        submission.index_count = lod.index_count;
        submission.history_valid = true;
        submission.pose = converted;
        out.push_back(std::move(submission));
        emitted = true;
    }
    return emitted;
}

}  // namespace matter::render
