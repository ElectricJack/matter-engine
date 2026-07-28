#pragma once

#include "vk_animation_skinning.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace viewer {

// Object-space AABB. Dynamic animation bounds deliberately remain in object
// space; the existing culler applies the instance transform exactly once.
struct VkAnimationBoundsAabb {
    float min[3]{};
    float max[3]{};
};

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

struct VkAnimationBoundsAsset {
    uint64_t asset_key = 0;
    VkAnimationBoundsAabb conservative_asset_bound{};
    std::vector<VkAnimationBoundsCluster> clusters;
};

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

constexpr uint32_t kVkAnimationBoundsOcclusionEnabled = 1u;
constexpr uint32_t kVkAnimationBoundsSkinRaster = 1u << 1u;

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
    bool register_asset(const VkAnimationBoundsAsset& asset);
    bool update_instance(uint32_t instance_slot, uint32_t instance_generation,
                         uint64_t asset_key,
                         const VkSkinPose& pose, bool history_valid);
    // A bridge/snapshot rejection has no valid pose for this frame.  Remove
    // any prior complete bound for every supplied full generational slot, then
    // publish the asset fallback with occlusion disabled when the asset is
    // still registered.  An unknown asset still removes the old record.
    void fail_open_instances(
        const std::vector<VkAnimationBoundsInstance>& instances);
    void remove_instance(uint32_t instance_slot, uint32_t instance_generation) noexcept;
    bool unregister_asset(uint64_t asset_key) noexcept;
    void clear_frame() noexcept;

    const std::vector<VkAnimationDynamicClusterBound>& dynamic_bounds() const noexcept {
        return dynamic_bounds_;
    }
    std::vector<VkAnimationBoundsGpuRecord> gpu_records() const;
    bool has_dynamic_bound(const VkAnimationBoundsKey& key) const noexcept;

private:
    struct InstanceState {
        uint64_t asset_key = 0;
        std::vector<VkAnimationDynamicClusterBound> last_complete;
    };

    std::map<uint64_t, VkAnimationBoundsAsset> assets_;
    std::map<std::pair<uint32_t, uint32_t>, InstanceState> instances_;
    std::vector<VkAnimationDynamicClusterBound> dynamic_bounds_;

    static bool valid_aabb(const VkAnimationBoundsAabb& aabb) noexcept;
    static bool valid_pose(const VkSkinPose& pose, bool history_valid) noexcept;
    static bool identical_asset(const VkAnimationBoundsAsset& a,
                                const VkAnimationBoundsAsset& b) noexcept;
};

}  // namespace viewer
