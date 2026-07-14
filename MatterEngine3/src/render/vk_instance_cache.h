#ifndef VIEWER_VK_INSTANCE_CACHE_H
#define VIEWER_VK_INSTANCE_CACHE_H

#include "vk_scene_renderer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace viewer {

struct ResolvedInstance;

uint64_t fingerprint_resolved_instances(
    const std::vector<ResolvedInstance>& resolved) noexcept;

class VulkanInstanceCache {
public:
    bool matches(const std::vector<ResolvedInstance>& resolved) const noexcept;
    void store(const std::vector<ResolvedInstance>& resolved,
               std::vector<VkSceneInstance> instances);
    void invalidate() noexcept;
    const std::vector<VkSceneInstance>& instances() const noexcept;
    uint64_t expansion_count() const noexcept;

private:
    uint64_t fingerprint_ = 0;
    size_t resolved_count_ = 0;
    bool valid_ = false;
    uint64_t expansion_count_ = 0;
    std::vector<VkSceneInstance> instances_;
};

} // namespace viewer

#endif // VIEWER_VK_INSTANCE_CACHE_H
