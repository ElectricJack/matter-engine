#pragma once
#include <vulkan/vulkan_core.h>
#include <cstdint>

namespace vt {
// Shared G-buffer attachment contract, including draws that emit no VT request
// (voxel foliage and surface proxies). Both receiver/module requests fit here.
inline constexpr VkFormat kVtFeedbackFormat = VK_FORMAT_R32G32B32A32_UINT;
inline constexpr uint32_t kVtFeedbackRequestsPerSample = 2;
}
