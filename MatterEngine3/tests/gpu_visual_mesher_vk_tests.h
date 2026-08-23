#pragma once

namespace matter {
class VulkanDevice;
}

int run_gpu_visual_mesher_pure_vk_tests();
int run_gpu_visual_mesher_vk_tests(matter::VulkanDevice& vulkan);
