#pragma once

// MatterEngine3/src/render/vk_animation_bounds.h
//
// Animated bounding volumes for skinned geometry, and the rules that stop the
// bind-pose and skinned versions of the same cluster being drawn at once.
//
// Two things live here:
//
//  1. `VkAnimationBounds` -- a per-frame transactional store mapping
//     (instance slot, generation, cluster, LOD) to an object-space AABB. It
//     is fed from the skin bridge (`animation_skin_bridge.h`) through
//     `VkSceneRenderer`, and its `gpu_records()` output is uploaded as the
//     dynamic-bounds buffer that `shaders_vk/cull.comp` tests instead of the
//     static cluster AABB.
//  2. Free functions that resolve the skinned/static hand-off:
//     `filter_ready_animation_skin_raster_draws` (which skinned draws the GPU
//     state actually supports this frame),
//     `animation_skin_raster_owns_cluster` and
//     `mark_animation_skin_raster_records` (which static draws must therefore
//     be suppressed), and `resolve_animation_cluster_union` (the all-LOD
//     union the CPU must agree with the culler on).
//
// Conventions and invariants:
//  - Every AABB here is OBJECT space. The culler applies the instance
//    transform exactly once, so nothing in this file may pre-apply it.
//  - Identity is always the full (instance_slot, instance_generation) pair.
//    Dynamic slots are recycled, so a slot alone would let a new entity
//    inherit the previous occupant's bounds.
//  - The store is fail-open but never fail-small. A rejected or missing pose
//    republishes the last complete bounds, or else the asset's conservative
//    bound with occlusion disabled. It must never publish a stale *smaller*
//    box, which would cull geometry that is on screen.
//  - Registered assets are immutable: re-registering a key only succeeds if
//    the payload is byte-identical.
//  - No Vulkan handles, no threading of its own, no internal locking. It is
//    plain CPU state driven from the render thread during frame preparation.
#include "vk_animation_skinning.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace viewer {

// Object-space AABB. Dynamic animation bounds deliberately remain in object
// space; the existing culler applies the instance transform exactly once.
// Axis order is [x, y, z]. The "empty" box used while accumulating a union is
// min = +inf, max = -inf; `valid_aabb` rejects any box containing a
// non-finite component or whose min exceeds its max on any axis.
struct VkAnimationBoundsAabb {
    float min[3]{};
    float max[3]{};
};

// One joint's contribution to a cluster's bound, expressed in that JOINT's
// local space. `joint` indexes the pose palette. At update time each of these
// is transformed by the joint's current (and, when history is valid,
// previous) skin matrix and unioned into the cluster's animated AABB, so the
// result covers the whole shutter interval.
struct VkAnimationBoundsJointAabb {
    uint32_t joint = 0;
    VkAnimationBoundsAabb aabb{};
};

// The serialized asset payload is one joint-local AABB per joint that affects
// a cluster/LOD.  It is intentionally not derived from a transient work queue:
// culling must be able to bound a newly visible skinned cluster before skinning
// output has been dispatched.
struct VkAnimationBoundsCluster {
    uint32_t cluster_index = 0;
    uint32_t lod = 0;
    std::vector<VkAnimationBoundsJointAabb> joints;
};

// The immutable per-asset bounds payload, registered once per asset revision.
// `conservative_asset_bound` is the whole-asset fallback published when a pose
// is missing or malformed -- object space, like everything else here. A
// (cluster_index, lod) pair may appear at most once in `clusters`, and every
// cluster must list at least one joint; `valid_animation_bounds_asset`
// enforces both.
struct VkAnimationBoundsAsset {
    uint64_t asset_key = 0;
    VkAnimationBoundsAabb conservative_asset_bound{};
    std::vector<VkAnimationBoundsCluster> clusters;
};

// The full identity of one dynamic bound. `operator<` below is not incidental:
// it is the sort order `VkAnimationBounds::dynamic_bounds()` is kept in, so
// the published record order is stable frame to frame.
struct VkAnimationBoundsKey {
    uint32_t instance_slot = 0;
    // Slot indices are deliberately reusable.  Pair them with the generation
    // issued by DynamicInstanceSlots so a new entity incarnation cannot reuse
    // a previous owner's last-complete animated bounds.
    uint32_t instance_generation = 0;
    uint32_t cluster_index = 0;
    uint32_t lod = 0;

    bool operator<(const VkAnimationBoundsKey& rhs) const noexcept {
        if (instance_slot != rhs.instance_slot) return instance_slot < rhs.instance_slot;
        if (instance_generation != rhs.instance_generation)
            return instance_generation < rhs.instance_generation;
        if (cluster_index != rhs.cluster_index) return cluster_index < rhs.cluster_index;
        return lod < rhs.lod;
    }
};

// The renderer-facing identity of one animated owner.  This deliberately
// includes the dynamic-slot generation: a rejected bridge frame must never
// clear or inherit a bound belonging to a later reuse of the same slot.
struct VkAnimationBoundsInstance {
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    uint64_t asset_key = 0;
};

struct VkAnimationDynamicClusterBound {
    VkAnimationBoundsKey key{};
    VkAnimationBoundsAabb aabb{};
    // false means the input was missing/corrupt. Culling may frustum test the
    // conservative asset AABB, but must not make an occlusion decision from it.
    bool occlusion_enabled = false;
};

// std430 payload for cull.comp. Its offset indexes the dynamic-bounds buffer,
// never the immutable cluster metadata buffer.
struct alignas(16) VkAnimationBoundsGpuRecord {
    float aabb_min[4]{};
    float aabb_max[4]{};
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    uint32_t cluster_index = 0;
    uint32_t lod = 0;
    uint32_t flags = 0;
    uint32_t pad = 0;
};
static_assert(sizeof(VkAnimationBoundsGpuRecord) == 64,
              "dynamic animation bounds must remain std430-compatible");

// `VkAnimationBoundsGpuRecord::flags` bits, mirrored in cull.comp.
//   kVkAnimationBoundsOcclusionEnabled  the bound came from a complete pose,
//       so it is tight enough to make an occlusion decision from. Cleared on
//       a fail-open record, where only the frustum test may use it.
//   kVkAnimationBoundsSkinRaster        a skinned raster draw owns this
//       (instance, generation, cluster) for the frame, so the static indirect
//       draw for it must be dropped. Set by
//       `mark_animation_skin_raster_records`.
constexpr uint32_t kVkAnimationBoundsOcclusionEnabled = 1u;
constexpr uint32_t kVkAnimationBoundsSkinRaster = 1u << 1u;

// Everything `filter_ready_animation_skin_raster_draws` needs to know about
// the renderer's real GPU state this frame, gathered by the caller so the
// filter itself stays free of Vulkan. All counts are elements, not bytes.
//
//  - `current_frame_slot`     the frame slot being built right now.
//  - `current_source_ready`   this frame's skin source upload and dispatch
//                             succeeded; draws pointing at the current slot
//                             are rejected without it, while draws retained
//                             from older slots are not.
//  - `index_count`            size of the shared index buffer, bounding
//                             first_index + index_count.
//  - `draw_transform_slots`   number of instance-transform slots, bounding
//                             `instance_slot`.
//  - `output_vertex_counts`   skinned output vertex count per frame slot.
struct VkSkinRasterValidationView {
    uint32_t current_frame_slot = 0;
    bool current_source_ready = false;
    uint32_t index_count = 0;
    uint32_t draw_transform_slots = 0;
    std::vector<uint32_t> output_vertex_counts;
    // One byte per frame slot; nonzero means both current and previous skin
    // vertex buffers are available for raster consumption.
    std::vector<uint8_t> output_buffers_ready;
};

// Resolves the exact draws that may leave the static cull path. A current
// frame draw additionally requires successful source validation/dispatch;
// retained draws require their producing buffers and range to remain valid.
std::vector<VkSkinRasterDraw> filter_ready_animation_skin_raster_draws(
    const std::vector<VkSkinRasterDraw>& draws,
    const VkSkinRasterValidationView& validation);

// True when an accepted skin draw owns this generational cluster for the
// frame. This is the single exclusion predicate for BOTH consumers of the
// bind-pose geometry: cull.comp drops the static indirect draw (via the
// flag mark_animation_skin_raster_records sets below), and the ray-traced
// lane drops the cluster's bind-pose BLAS from the TLAS. Splitting those
// decisions is what let the tracer keep a bind-pose ghost inside a skinned
// mesh. Deliberately LOD-agnostic, like the marking below.
bool animation_skin_raster_owns_cluster(
    const std::vector<VkSkinRasterDraw>& draws,
    uint32_t instance_slot, uint32_t instance_generation,
    uint32_t cluster_index) noexcept;

// Marks only the generational instance/LOD records which have a concrete
// skinned raster draw. The culler then omits those instances from the static
// indirect bucket without suppressing bind fallbacks or shared-mesh peers.
void mark_animation_skin_raster_records(
    std::vector<VkAnimationBoundsGpuRecord>& records,
    const std::vector<VkSkinRasterDraw>& draws) noexcept;

// Resolves cull.comp's conservative all-LOD union for one full
// generational cluster identity. CPU skin-work planning uses this before LOD
// selection so it cannot disagree with the GPU culler when animation crosses
// a threshold.
bool resolve_animation_cluster_union(
    const std::vector<VkAnimationBoundsGpuRecord>& records,
    uint32_t instance_slot, uint32_t instance_generation,
    uint32_t cluster_index, VkAnimationBoundsAabb& out) noexcept;

// Validates immutable serialized joint-local bounds without creating a
// renderer-side registration.  The skin bridge uses it to reject a malformed
// ECS binding before any queue or culling state is touched.
bool valid_animation_bounds_asset(const VkAnimationBoundsAsset& asset) noexcept;

// Frame-local, transactional resolver for animated bounds.  A successful pose
// replaces all cluster bounds for that instance together. A rejected pose keeps
// the matching last complete bounds; without one it publishes only the asset
// fallback with occlusion disabled, never a stale smaller box.
class VkAnimationBounds {
public:
    // Returns false for a payload `valid_animation_bounds_asset` rejects, or
    // for a re-register of a known key whose payload differs. Re-registering
    // a byte-identical payload succeeds and changes nothing.
    bool register_asset(const VkAnimationBoundsAsset& asset);
    // Republishes every cluster bound for one generational instance, as one
    // transaction. Returns true only when a complete pose was consumed;
    // false covers two different outcomes -- an unknown `asset_key`, where
    // nothing at all is published, and a rejected pose, where the last
    // complete bounds or the conservative fallback is published instead.
    // Costs a linear erase plus a re-sort of the whole published set.
    bool update_instance(uint32_t instance_slot, uint32_t instance_generation,
                         uint64_t asset_key,
                         const VkSkinPose& pose, bool history_valid);
    // A bridge/snapshot rejection has no valid pose for this frame.  Remove
    // any prior complete bound for every supplied full generational slot, then
    // publish the asset fallback with occlusion disabled when the asset is
    // still registered.  An unknown asset still removes the old record.
    void fail_open_instances(
        const std::vector<VkAnimationBoundsInstance>& instances);
    // Forgets one generational instance entirely: both its retained
    // last-complete bounds and its published records. Unlike the fail-open
    // path this leaves nothing behind, so the culler falls back to the
    // static cluster bound.
    void remove_instance(uint32_t instance_slot, uint32_t instance_generation) noexcept;
    bool unregister_asset(uint64_t asset_key) noexcept;

    const std::vector<VkAnimationDynamicClusterBound>& dynamic_bounds() const noexcept {
        return dynamic_bounds_;
    }
    // Builds a fresh std430 record array from the published bounds. It
    // allocates and is O(dynamic bounds) on every call, so hoist it out of
    // loops rather than treating it as an accessor.
    std::vector<VkAnimationBoundsGpuRecord> gpu_records() const;
    // Linear scan of the published set; intended for tests and assertions,
    // not for per-instance queries in a frame loop.
    bool has_dynamic_bound(const VkAnimationBoundsKey& key) const noexcept;

private:
    struct InstanceState {
        uint64_t asset_key = 0;
        std::vector<VkAnimationDynamicClusterBound> last_complete;
    };

    // assets_          immutable payloads, keyed by asset_key.
    // instances_       per (instance_slot, instance_generation) state; its
    //                  `last_complete` is the retained bound reused when a
    //                  later pose is rejected, and `asset_key` records which
    //                  asset revision owns the identity so
    //                  `unregister_asset` can retire it.
    // dynamic_bounds_  the published set, kept sorted by
    //                  `VkAnimationBoundsKey` and mirrored 1:1 into the GPU
    //                  record array.
    std::map<uint64_t, VkAnimationBoundsAsset> assets_;
    std::map<std::pair<uint32_t, uint32_t>, InstanceState> instances_;
    std::vector<VkAnimationDynamicClusterBound> dynamic_bounds_;

    static bool valid_aabb(const VkAnimationBoundsAabb& aabb) noexcept;
    static bool valid_pose(const VkSkinPose& pose, bool history_valid) noexcept;
    static bool identical_asset(const VkAnimationBoundsAsset& a,
                                const VkAnimationBoundsAsset& b) noexcept;
};

}  // namespace viewer
