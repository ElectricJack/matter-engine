#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace matter {

class VulkanDevice;

struct CudaVulkanInteropPixel {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

bool cuda_vulkan_device_ids_match_for_test(
    const std::array<uint8_t, VK_UUID_SIZE>& vulkan_uuid,
    const std::array<uint8_t, VK_UUID_SIZE>& cuda_uuid,
    bool force_mismatch = false) noexcept;

uint32_t win32_process_handle_count() noexcept;

class CudaVulkanInterop {
public:
    static std::unique_ptr<CudaVulkanInterop> create(VulkanDevice& vulkan,
                                                     std::string& error);
    ~CudaVulkanInterop();

    CudaVulkanInterop(const CudaVulkanInterop&) = delete;
    CudaVulkanInterop& operator=(const CudaVulkanInterop&) = delete;

    bool round_trip(VkExtent2D extent, uint64_t frame_serial,
                    CudaVulkanInteropPixel& pixel, std::string& error);

    const std::array<uint8_t, VK_UUID_SIZE>& vulkan_uuid() const noexcept;
    const std::array<uint8_t, VK_UUID_SIZE>& cuda_uuid() const noexcept;

private:
    CudaVulkanInterop();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter
