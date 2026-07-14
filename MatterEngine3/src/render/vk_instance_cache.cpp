#include "vk_instance_cache.h"

#include "../provider/sector_resolver.h"

#include <utility>

namespace viewer {

uint64_t fingerprint_resolved_instances(
    const std::vector<ResolvedInstance>& resolved) noexcept {
    uint64_t fingerprint = 1469598103934665603ull;
    const auto fold = [&fingerprint](const void* bytes, size_t size) {
        const auto* data = static_cast<const unsigned char*>(bytes);
        for (size_t index = 0; index < size; ++index)
            fingerprint = (fingerprint ^ data[index]) * 1099511628211ull;
    };
    for (const ResolvedInstance& instance : resolved) {
        fold(&instance.part_hash, sizeof(instance.part_hash));
        fold(instance.transform, sizeof(instance.transform));
        fold(&instance.segment, sizeof(instance.segment));
    }
    return fingerprint;
}

bool VulkanInstanceCache::matches(
    const std::vector<ResolvedInstance>& resolved) const noexcept {
    return valid_ && resolved_count_ == resolved.size() &&
           fingerprint_ == fingerprint_resolved_instances(resolved);
}

void VulkanInstanceCache::store(
    const std::vector<ResolvedInstance>& resolved,
    std::vector<VkSceneInstance> instances) {
    fingerprint_ = fingerprint_resolved_instances(resolved);
    resolved_count_ = resolved.size();
    instances_ = std::move(instances);
    valid_ = true;
    ++expansion_count_;
}

void VulkanInstanceCache::invalidate() noexcept {
    fingerprint_ = 0;
    resolved_count_ = 0;
    valid_ = false;
    instances_.clear();
}

const std::vector<VkSceneInstance>& VulkanInstanceCache::instances() const noexcept {
    return instances_;
}

uint64_t VulkanInstanceCache::expansion_count() const noexcept {
    return expansion_count_;
}

} // namespace viewer
