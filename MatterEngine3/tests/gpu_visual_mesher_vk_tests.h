#pragma once

#include <filesystem>

namespace matter {
class VulkanDevice;
}

int run_gpu_visual_mesher_pure_vk_tests();
int run_gpu_visual_mesher_vk_tests(matter::VulkanDevice& vulkan);
int run_gpu_visual_mesher_acceptance(matter::VulkanDevice& vulkan);
int run_gpu_visual_mesher_waterfall_quality(
    matter::VulkanDevice& vulkan, const std::filesystem::path& report);
