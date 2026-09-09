// MatterEngine3/src/render/animation_skin_bridge.h
//
// Immutable animation-to-Vulkan skin work adapter.  This boundary owns no
// evaluator, Flecs world, or renderer state: callers provide an exact
// presentation snapshot plus an already-resolved scene/dynamic-slot mapping.
//
// Where it sits
// -------------
// The skinned counterpart of animation_rigid_bridge.h. The only production
// caller is matter::scene::DynamicSceneBridge::collect_animation_skinning()
// (MatterEngine3/src/ecs/dynamic_scene_bridge.cpp), which runs AFTER
// reconcile() so that every animated root already owns a dynamic transform
// slot. Output records go to viewer::VkAnimationSkinning (vk_animation_skinning.h)
// as the frame's compute-skinning queue; the renderer then compacts that queue
// against this frame's animated bounds, frustum and cluster LOD.
//
// Lifecycle
// ---------
// Same as the rigid bridge: constructed with a non-owning pointer to the
// AnimationPoseSnapshotStore owned by AnimationSystems, swappable via
// set_snapshots(), null until animation is attached.
//
// Conventions and gotchas
// -----------------------
//  - The pose comes from snapshot(animator, frame_serial). A stale serial
//    reads as empty and the submission is rejected; the bridge deliberately
//    never falls back to latest(), so a lagging animator keeps the static /
//    bind-pose path instead of rendering an old pose as current.
//  - Palette matrices are converted to the GPU's column-major layout here, and
//    the normal matrix is uploaded as transpose(inverse(position)) rather than
//    derived in the shader (non-uniform authored scale would otherwise produce
//    malformed normals). A non-invertible joint matrix rejects the submission.
//  - Validation is per call, not cached: valid_animation_skinned_asset() walks
//    every LOD's influence window, so cost is O(total skinned vertices in the
//    asset) on every entity every frame.
//  - No internal synchronization; call from the thread that owns the pose
//    snapshot store for the frame being collected.
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
    // Part-LOCAL first vertex of this range. The shared index buffer stores
    // PART-LOCAL index values (matter_engine.cpp rebases each mesh by its
    // offset within the part and no further; vk_scene_renderer never rewrites
    // them), so a skinned draw must subtract exactly this to land on the
    // compute output. Subtracting the renderer-global source_vertex instead
    // over-rebases by the part's arena base -- invisible whenever that base is
    // 0, which is every fixture but not real content.
    uint32_t local_vertex_base = 0;
    uint32_t influence_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    // Renderer cluster identity for this immutable LOD range. The skin queue
    // is compacted against this exact cluster rather than a presentation LOD.
    uint32_t cluster = 0;
    uint32_t lod = 0;
};

// One immutable revision of a skinned asset. Like AnimationRigidAsset this is a
// view: `influences` and `bounds` point at storage owned by the loader and are
// never copied. `identity` is the asset key the renderer's skinning queue is
// registered under (must be non-zero), and `generation` must be bumped on every
// republish so a component still holding the old pointer is rejected by the
// compare in expand() rather than dereferenced.
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

// The value the ECS stores next to a skinned entity, copied wholesale into an
// AnimationSkinExpansion by the scene bridge.
//
// `lod` is the PRESENTATION LOD: it is range-checked against asset->lods (an
// out-of-range value rejects the whole submission) but it does NOT select the
// geometry that gets published — expand() emits every LOD range whose part_hash
// matches, and the renderer picks. `visible = false` short-circuits to success
// with no work emitted, which is not the same as a rejection.
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

// One frame's input for one skinned entity. All values; the bridge reads
// nothing else.
//
//  - entity must be the ROOT key (binding_index == 0).
//  - part_hash selects which of the asset's LOD ranges are published: every
//    AnimationSkinnedLod with this part_hash is emitted.
//  - transform_slot / transform_generation are the entity's live
//    DynamicInstanceSlots index and generation, resolved by the caller before
//    expansion. UINT32_MAX means "unresolved" and is rejected; the generation
//    travels with the slot so a recycled slot cannot inherit a previous
//    occupant's skinning output.
//  - frame_serial must be non-zero and must exactly match a published pose.
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

// Pure adapter whose only state is a non-owning pose-store pointer, so it is
// trivially copyable and held by value inside DynamicSceneBridge. A null store
// makes every expand() call fail, which is the legal pre-animation state.
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
