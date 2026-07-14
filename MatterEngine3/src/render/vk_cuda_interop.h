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
uint32_t cuda_vulkan_application_export_handle_count() noexcept;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
uintptr_t cuda_vulkan_current_context_for_test(std::string& error) noexcept;
uintptr_t cuda_vulkan_create_caller_context_for_test(std::string& error) noexcept;
void cuda_vulkan_destroy_caller_context_for_test(uintptr_t context) noexcept;
bool cuda_vulkan_luid_matches_for_test(
    bool vulkan_valid, const std::array<uint8_t, VK_LUID_SIZE>& vulkan_luid,
    uint32_t vulkan_node_mask, bool cuda_valid,
    const std::array<uint8_t, VK_LUID_SIZE>& cuda_luid,
    uint32_t cuda_node_mask) noexcept;
std::string cuda_vulkan_luid_failure_diagnostic_for_test(
    const std::string& vulkan_name,
    const std::array<uint8_t, VK_UUID_SIZE>& vulkan_uuid,
    const std::array<uint8_t, VK_LUID_SIZE>& vulkan_luid,
    uint32_t vulkan_node_mask, const std::string& cuda_name,
    const std::array<uint8_t, VK_UUID_SIZE>& cuda_uuid,
    int cuda_result);
void cuda_vulkan_reset_test_destroy_call_count() noexcept;
uint32_t cuda_vulkan_test_destroy_call_count() noexcept;
#endif

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
    bool poisoned() const noexcept;

private:
    CudaVulkanInterop();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter
