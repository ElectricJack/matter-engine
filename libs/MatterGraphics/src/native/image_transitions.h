#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace viewer::raster_detail {
inline void record_image_transition(VkCommandBuffer command_buffer,
                             VkImage image, VkImageLayout& layout,
                             uint32_t mip_levels, uint32_t array_layers,
                             VkImageLayout new_layout,
                             VkPipelineStageFlags2 source_stage,
                             VkAccessFlags2 source_access,
                             VkPipelineStageFlags2 destination_stage,
                             VkAccessFlags2 destination_access,
                             VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source_stage;
    barrier.srcAccessMask = source_access;
    barrier.dstStageMask = destination_stage;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.layerCount = array_layers;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
    layout = new_layout;
}

inline VkPipelineStageFlags2 gbuffer_sampled_stages(
    uint32_t attachment_index, bool native_ray_tracing_available) {
    VkPipelineStageFlags2 stages = 0;
    if (attachment_index < 4)
        stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (attachment_index == 1 || attachment_index == 3 ||
        attachment_index == 4 || attachment_index == 5)
        stages |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (native_ray_tracing_available && attachment_index < 3)
        stages |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    return stages;
}

inline VkPipelineStageFlags2 ray_depth_destination_stages(
    bool native_ray_tracing_available) noexcept {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (native_ray_tracing_available)
        stages |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    return stages;
}
}
