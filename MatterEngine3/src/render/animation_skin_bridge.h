// Immutable animation-to-Vulkan skin work adapter.  This boundary owns no
// evaluator, Flecs world, or renderer state: callers provide an exact
// presentation snapshot plus an already-resolved scene/dynamic-slot mapping.
#pragma once

#include "animation/animation_systems.h"
#include "render/dynamic_instance_slots.h"
#include "render/vk_animation_skinning.h"
#include "render/vk_animation_bounds.h"

#include <cstdint>
#include <vector>

namespace matter::render {

// Ranges are renderer-global, indexed-raster ranges selected by the existing
// conservative visibility path.  They are immutable for one asset revision;
// a new part arena/revision must publish a new asset generation instead of
// mutating this descriptor in place.
struct AnimationSkinnedLod {
    uint64_t part_hash = 0;
    uint32_t source_vertex = 0;
    uint32_t influence_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    // Renderer cluster identity for this immutable LOD range. The skin queue
    // is compacted against this exact cluster rather than a presentation LOD.
    uint32_t cluster = 0;
};

struct AnimationSkinnedAsset {
    uint64_t identity = 0;
    uint32_t generation = 0;
    const std::vector<viewer::VkSkinInfluence>* influences = nullptr;
    std::vector<AnimationSkinnedLod> lods;
    // Immutable, joint-local cluster bounds for this exact asset revision.
    // Culling is fail-open without this payload, so runtime bindings require
    // it rather than silently using stale data from another revision.
    const viewer::VkAnimationBoundsAsset* bounds = nullptr;
};

struct AnimationSkinnedBinding {
    AnimatorInstanceHandle animator{};
    const AnimationSkinnedAsset* asset = nullptr;
    uint32_t asset_generation = 0;
    uint32_t lod = 0;
    bool visible = true;
    // Cosmetic pose-rate priority only. It never changes fixed simulation or
    // current-frame culling/skin work ordering unless explicitly copied into
    // a completed presentation observation by the renderer bridge.
    int32_t presentation_priority = 0;
};

struct AnimationSkinExpansion {
    DynamicInstanceKey entity{};
    uint64_t part_hash = 0;
    uint32_t transform_slot = UINT32_MAX;
    uint64_t frame_serial = 0;
    AnimationSkinnedBinding binding{};
    uint32_t transform_generation = 0;
};

// Validates only data which is independent of a particular evaluated pose.
// Palette-dependent joint bounds are checked by VkAnimationSkinning atomically
// with the queue publication.
bool valid_animation_skinned_asset(const AnimationSkinnedAsset& asset) noexcept;

class AnimationSkinBridge {
public:
    explicit AnimationSkinBridge(const animation::AnimationPoseSnapshotStore* snapshots)
        : snapshots_(snapshots) {}

    void set_snapshots(const animation::AnimationPoseSnapshotStore* snapshots) noexcept {
        snapshots_ = snapshots;
    }

    // Appends exactly one C2 work record only after every binding, identity,
    // mapping, pose, and conversion check passes.  It never falls back to
    // latest(): a stale render serial must retain the bind-pose/static path.
    bool expand(const AnimationSkinExpansion& input,
                std::vector<viewer::VkSkinSubmission>& out) const;

private:
    const animation::AnimationPoseSnapshotStore* snapshots_ = nullptr;
};

}  // namespace matter::render
