// MatterEngine3/src/render/animation_skin_bridge.cpp
//
// Implementation of the skinned articulation adapter declared in
// animation_skin_bridge.h. See that header for subsystem context, the
// stale-serial rule, and lifetime rules.
//
// Two things happen here that are worth knowing before reading the code:
//
//  1. Every palette matrix is inverted in double precision and packed into the
//     GPU's column-major layout, so a joint matrix that is non-finite or
//     singular fails the whole submission instead of producing NaN normals.
//  2. Validation is exhaustive and uncached. valid_animation_skinned_asset()
//     re-walks every LOD's influence window on every call, and expand() calls
//     it once per entity per frame — the cost scales with the asset's total
//     skinned vertex count, not with the number of LODs actually drawn.

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

// Gauss-Jordan inversion with partial pivoting, carried in double precision.
// Returns false (leaving `out` partially written) for a non-finite input, a
// pivot magnitude <= 1e-12 (treated as singular), or a non-finite result — all
// of which the callers turn into a rejected submission, never a NaN upload.
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

// Converts one skin-palette matrix into the GPU's VkSkinJoint pair: the skin
// position matrix and its inverse-transpose for normals. False means the input
// was non-finite or not invertible, which rejects the whole submission.
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

// Pose-independent checks for one LOD range: a non-zero part hash, a non-empty
// vertex range, an index count that is a whole number of triangles, no overflow
// of source_vertex + vertex_count, and an influence window that lies entirely
// inside the packed influence arena.
//
// It then walks every vertex in that window and rejects any with all-zero
// weights — an unweighted vertex would skin to the origin and drag a visible
// spike out of the mesh. That walk is what makes this O(vertex_count), not O(1).
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

// Whole-asset validation: identity/generation present, influence arena and LOD
// list non-empty, a bounds payload attached whose asset_key matches this exact
// identity (culling is fail-open without it), and every LOD internally
// consistent.
//
// Costs O(sum of all LOD vertex counts) because of the per-vertex weight scan
// in valid_lod(), and expand() calls it on every entity every frame. It is not
// the cheap predicate its name suggests.
bool valid_animation_skinned_asset(const AnimationSkinnedAsset& asset) noexcept {
    if (asset.identity == 0 || asset.generation == 0 || asset.influences == nullptr ||
        asset.influences->empty() || asset.lods.empty() || asset.bounds == nullptr ||
        asset.bounds->asset_key != asset.identity ||
        !viewer::valid_animation_bounds_asset(*asset.bounds)) return false;
    for (const AnimationSkinnedLod& lod : asset.lods)
        if (!valid_lod(lod, *asset.influences)) return false;
    return true;
}

// Appends one VkSkinSubmission per matching LOD range to `out` (appended to,
// never cleared), each carrying its own copy of the converted palette pair.
//
// The three return states are distinct:
//  - true, nothing appended: the binding is not visible this frame. Normal.
//  - true, records appended: at least one LOD's part_hash matched input.part_hash.
//  - false: rejected. Either a validation failure (unresolved transform slot,
//    stale asset generation, out-of-range presentation LOD, no pose published
//    for exactly input.frame_serial, a palette longer than
//    kVkSkinPaletteCountMax, a non-invertible joint) or — note — a fully valid
//    asset in which NO LOD carried the requested part_hash.
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
        submission.local_vertex_base = lod.local_vertex_base;
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
