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
    uint32_t cluster_index = 0;
    uint32_t lod = 0;

    bool operator<(const VkAnimationBoundsKey& rhs) const noexcept {
        if (instance_slot != rhs.instance_slot) return instance_slot < rhs.instance_slot;
        if (cluster_index != rhs.cluster_index) return cluster_index < rhs.cluster_index;
        return lod < rhs.lod;
    }
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
    uint32_t cluster_index = 0;
    uint32_t lod = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(VkAnimationBoundsGpuRecord) == 48,
              "dynamic animation bounds must remain std430-compatible");

constexpr uint32_t kVkAnimationBoundsOcclusionEnabled = 1u;

// Frame-local, transactional resolver for animated bounds.  A successful pose
// replaces all cluster bounds for that instance together. A rejected pose keeps
// the matching last complete bounds; without one it publishes only the asset
// fallback with occlusion disabled, never a stale smaller box.
class VkAnimationBounds {
public:
    bool register_asset(const VkAnimationBoundsAsset& asset);
    bool update_instance(uint32_t instance_slot, uint64_t asset_key,
                         const VkSkinPose& pose, bool history_valid);
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
    std::map<uint32_t, InstanceState> instances_;
    std::vector<VkAnimationDynamicClusterBound> dynamic_bounds_;

    static bool valid_aabb(const VkAnimationBoundsAabb& aabb) noexcept;
    static bool valid_pose(const VkSkinPose& pose, bool history_valid) noexcept;
    static bool identical_asset(const VkAnimationBoundsAsset& a,
                                const VkAnimationBoundsAsset& b) noexcept;
};

}  // namespace viewer
