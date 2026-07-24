#include "vk_animation_bounds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace viewer {
namespace {

bool finite_matrix(const VkSkinMatrix& matrix) noexcept {
    for (float value : matrix.elements)
        if (!std::isfinite(value)) return false;
    return true;
}

bool valid_aabb(const VkAnimationBoundsAabb& aabb) noexcept {
    for (uint32_t axis = 0; axis != 3; ++axis) {
        if (!std::isfinite(aabb.min[axis]) || !std::isfinite(aabb.max[axis]) ||
            aabb.min[axis] > aabb.max[axis]) return false;
    }
    return true;
}

void include_point(VkAnimationBoundsAabb& result, const float point[3]) noexcept {
    for (uint32_t axis = 0; axis != 3; ++axis) {
        result.min[axis] = std::min(result.min[axis], point[axis]);
        result.max[axis] = std::max(result.max[axis], point[axis]);
    }
}

VkAnimationBoundsAabb empty_aabb() noexcept {
    VkAnimationBoundsAabb result{};
    for (uint32_t axis = 0; axis != 3; ++axis) {
        result.min[axis] = std::numeric_limits<float>::infinity();
        result.max[axis] = -std::numeric_limits<float>::infinity();
    }
    return result;
}

void include_aabb(VkAnimationBoundsAabb& result,
                  const VkAnimationBoundsAabb& input) noexcept {
    for (uint32_t axis = 0; axis != 3; ++axis) {
        result.min[axis] = std::min(result.min[axis], input.min[axis]);
        result.max[axis] = std::max(result.max[axis], input.max[axis]);
    }
}

void transform_point(const VkSkinMatrix& matrix, const float in[3],
                     float out[3]) noexcept {
    // VkSkinMatrix is the column-major GLSL representation used by C2.
    for (uint32_t row = 0; row != 3; ++row) {
        out[row] = matrix.elements[row] * in[0] +
                   matrix.elements[4 + row] * in[1] +
                   matrix.elements[8 + row] * in[2] +
                   matrix.elements[12 + row];
    }
}

VkAnimationBoundsAabb transform_aabb(const VkAnimationBoundsAabb& local,
                                     const VkSkinMatrix& matrix) noexcept {
    VkAnimationBoundsAabb result = empty_aabb();
    for (uint32_t corner = 0; corner != 8; ++corner) {
        const float point[3]{
            (corner & 4u) != 0 ? local.max[0] : local.min[0],
            (corner & 2u) != 0 ? local.max[1] : local.min[1],
            (corner & 1u) != 0 ? local.max[2] : local.min[2]};
        float transformed[3]{};
        transform_point(matrix, point, transformed);
        include_point(result, transformed);
    }
    return result;
}

bool same_floats(const void* a, const void* b, size_t count) noexcept {
    return std::memcmp(a, b, count) == 0;
}

}  // namespace

bool VkAnimationBounds::valid_aabb(const VkAnimationBoundsAabb& aabb) noexcept {
    return ::viewer::valid_aabb(aabb);
}

bool VkAnimationBounds::valid_pose(const VkSkinPose& pose,
                                   bool history_valid) noexcept {
    if (pose.current.empty()) return false;
    if (history_valid && pose.previous.size() != pose.current.size()) return false;
    for (const VkSkinJoint& joint : pose.current)
        if (!finite_matrix(joint.position)) return false;
    if (history_valid) {
        for (const VkSkinJoint& joint : pose.previous)
            if (!finite_matrix(joint.position)) return false;
    }
    return true;
}

bool VkAnimationBounds::identical_asset(const VkAnimationBoundsAsset& a,
                                        const VkAnimationBoundsAsset& b) noexcept {
    if (a.asset_key != b.asset_key ||
        !same_floats(&a.conservative_asset_bound, &b.conservative_asset_bound,
                     sizeof(VkAnimationBoundsAabb)) ||
        a.clusters.size() != b.clusters.size()) return false;
    for (size_t cluster_index = 0; cluster_index != a.clusters.size(); ++cluster_index) {
        const auto& left = a.clusters[cluster_index];
        const auto& right = b.clusters[cluster_index];
        if (left.cluster_index != right.cluster_index || left.lod != right.lod ||
            left.joints.size() != right.joints.size()) return false;
        for (size_t joint_index = 0; joint_index != left.joints.size(); ++joint_index) {
            if (left.joints[joint_index].joint != right.joints[joint_index].joint ||
                !same_floats(&left.joints[joint_index].aabb,
                             &right.joints[joint_index].aabb,
                             sizeof(VkAnimationBoundsAabb))) return false;
        }
    }
    return true;
}

bool valid_animation_bounds_asset(const VkAnimationBoundsAsset& asset) noexcept {
    if (asset.asset_key == 0 || asset.clusters.empty() ||
        !valid_aabb(asset.conservative_asset_bound)) return false;
    std::map<std::pair<uint32_t, uint32_t>, bool> seen_clusters;
    for (const VkAnimationBoundsCluster& cluster : asset.clusters) {
        if (cluster.joints.empty() ||
            !seen_clusters.emplace(std::make_pair(cluster.cluster_index, cluster.lod), true).second)
            return false;
        for (const VkAnimationBoundsJointAabb& joint : cluster.joints)
            if (!valid_aabb(joint.aabb)) return false;
    }
    return true;
}

bool VkAnimationBounds::register_asset(const VkAnimationBoundsAsset& asset) {
    if (!valid_animation_bounds_asset(asset)) return false;
    const auto existing = assets_.find(asset.asset_key);
    if (existing != assets_.end()) return identical_asset(existing->second, asset);
    assets_.emplace(asset.asset_key, asset);
    return true;
}

bool VkAnimationBounds::update_instance(uint32_t instance_slot,
                                        uint32_t instance_generation,
                                        uint64_t asset_key,
                                        const VkSkinPose& pose,
                                        bool history_valid) {
    const auto asset_it = assets_.find(asset_key);
    if (asset_it == assets_.end()) return false;
    const VkAnimationBoundsAsset& asset = asset_it->second;
    std::vector<VkAnimationDynamicClusterBound> staged;
    bool complete = valid_pose(pose, history_valid);
    if (complete) {
        staged.reserve(asset.clusters.size());
        for (const VkAnimationBoundsCluster& cluster : asset.clusters) {
            VkAnimationBoundsAabb unioned = empty_aabb();
            for (const VkAnimationBoundsJointAabb& local : cluster.joints) {
                if (local.joint >= pose.current.size() ||
                    (history_valid && local.joint >= pose.previous.size())) {
                    complete = false;
                    break;
                }
                include_aabb(unioned, transform_aabb(local.aabb,
                                                     pose.current[local.joint].position));
                if (history_valid)
                    include_aabb(unioned, transform_aabb(local.aabb,
                                                         pose.previous[local.joint].position));
            }
            if (!complete || !valid_aabb(unioned)) {
                complete = false;
                break;
            }
            staged.push_back({{instance_slot, instance_generation, cluster.cluster_index, cluster.lod}, unioned, true});
        }
    }

    const auto instance_key = std::make_pair(instance_slot, instance_generation);
    InstanceState& state = instances_[instance_key];
    if (complete) {
        state.asset_key = asset_key;
        state.last_complete = staged;
    } else if (state.asset_key == asset_key && !state.last_complete.empty()) {
        staged = state.last_complete;
    } else {
        staged.reserve(asset.clusters.size());
        for (const VkAnimationBoundsCluster& cluster : asset.clusters)
            staged.push_back({{instance_slot, instance_generation, cluster.cluster_index, cluster.lod},
                              asset.conservative_asset_bound, false});
    }

    dynamic_bounds_.erase(
        std::remove_if(dynamic_bounds_.begin(), dynamic_bounds_.end(),
                       [instance_slot, instance_generation](const VkAnimationDynamicClusterBound& value) {
                           return value.key.instance_slot == instance_slot &&
                                  value.key.instance_generation == instance_generation;
                       }),
        dynamic_bounds_.end());
    dynamic_bounds_.insert(dynamic_bounds_.end(), staged.begin(), staged.end());
    std::sort(dynamic_bounds_.begin(), dynamic_bounds_.end(),
              [](const VkAnimationDynamicClusterBound& left,
                 const VkAnimationDynamicClusterBound& right) {
                    return left.key < right.key;
              });
    return complete;
}

void VkAnimationBounds::fail_open_instances(
    const std::vector<VkAnimationBoundsInstance>& instances) {
    std::vector<VkAnimationBoundsInstance> ordered = instances;
    std::sort(ordered.begin(), ordered.end(),
              [](const VkAnimationBoundsInstance& left,
                 const VkAnimationBoundsInstance& right) {
                  if (left.instance_slot != right.instance_slot)
                      return left.instance_slot < right.instance_slot;
                  if (left.instance_generation != right.instance_generation)
                      return left.instance_generation < right.instance_generation;
                  return left.asset_key < right.asset_key;
              });
    for (size_t index = 0; index != ordered.size();) {
        const VkAnimationBoundsInstance value = ordered[index];
        size_t next = index + 1;
        // A valid bridge produces one asset per dynamic owner.  Conflicting
        // asset identities are ambiguous: removal is safer than guessing a
        // conservative bound from the wrong mesh revision.
        bool unambiguous_asset = true;
        while (next != ordered.size() &&
               ordered[next].instance_slot == value.instance_slot &&
               ordered[next].instance_generation == value.instance_generation) {
            unambiguous_asset = unambiguous_asset &&
                                ordered[next].asset_key == value.asset_key;
            ++next;
        }
        remove_instance(value.instance_slot, value.instance_generation);
        if (unambiguous_asset && assets_.find(value.asset_key) != assets_.end()) {
            const VkSkinPose missing_pose{};
            (void)update_instance(value.instance_slot,
                                  value.instance_generation,
                                  value.asset_key, missing_pose, false);
        }
        index = next;
    }
}

bool VkAnimationBounds::unregister_asset(uint64_t asset_key) noexcept {
    if (asset_key == 0 || assets_.erase(asset_key) == 0) return false;
    std::vector<std::pair<uint32_t, uint32_t>> retired_instances;
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (it->second.asset_key == asset_key) {
            retired_instances.push_back(it->first);
            it = instances_.erase(it);
        }
        else ++it;
    }
    dynamic_bounds_.erase(
        std::remove_if(dynamic_bounds_.begin(), dynamic_bounds_.end(),
                       [&retired_instances](const VkAnimationDynamicClusterBound& value) {
                           return std::find(retired_instances.begin(), retired_instances.end(),
                                            std::make_pair(value.key.instance_slot,
                                                           value.key.instance_generation)) !=
                                  retired_instances.end();
                       }),
        dynamic_bounds_.end());
    return true;
}

void VkAnimationBounds::remove_instance(uint32_t instance_slot,
                                        uint32_t instance_generation) noexcept {
    instances_.erase(std::make_pair(instance_slot, instance_generation));
    dynamic_bounds_.erase(
        std::remove_if(dynamic_bounds_.begin(), dynamic_bounds_.end(),
                       [instance_slot, instance_generation](const VkAnimationDynamicClusterBound& value) {
                           return value.key.instance_slot == instance_slot &&
                                  value.key.instance_generation == instance_generation;
                       }),
        dynamic_bounds_.end());
}

void VkAnimationBounds::clear_frame() noexcept {
    dynamic_bounds_.clear();
}

std::vector<VkAnimationBoundsGpuRecord> VkAnimationBounds::gpu_records() const {
    std::vector<VkAnimationBoundsGpuRecord> records;
    records.reserve(dynamic_bounds_.size());
    for (const VkAnimationDynamicClusterBound& value : dynamic_bounds_) {
        VkAnimationBoundsGpuRecord record{};
        for (uint32_t axis = 0; axis != 3; ++axis) {
            record.aabb_min[axis] = value.aabb.min[axis];
            record.aabb_max[axis] = value.aabb.max[axis];
        }
        record.instance_slot = value.key.instance_slot;
        record.instance_generation = value.key.instance_generation;
        record.cluster_index = value.key.cluster_index;
        record.lod = value.key.lod;
        record.flags = value.occlusion_enabled ? kVkAnimationBoundsOcclusionEnabled : 0u;
        records.push_back(record);
    }
    return records;
}

bool VkAnimationBounds::has_dynamic_bound(const VkAnimationBoundsKey& key) const noexcept {
    return std::any_of(dynamic_bounds_.begin(), dynamic_bounds_.end(),
                       [&key](const VkAnimationDynamicClusterBound& value) {
                           return value.key.instance_slot == key.instance_slot &&
                                  value.key.instance_generation == key.instance_generation &&
                                  value.key.cluster_index == key.cluster_index &&
                                  value.key.lod == key.lod;
                       });
}

}  // namespace viewer
