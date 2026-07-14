#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <string_view>
#include <vector>

#include "gpu_matrix_pack.h"
#include "matter/math_types.h"

namespace matter {

class VulkanDevice;
struct VkBufferResource;

struct VkComputePipelineResource {
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

    VkComputePipelineResource() = default;
    ~VkComputePipelineResource();
    VkComputePipelineResource(const VkComputePipelineResource&) = delete;
    VkComputePipelineResource& operator=(const VkComputePipelineResource&) = delete;
    VkComputePipelineResource(VkComputePipelineResource&& other) noexcept;
    VkComputePipelineResource& operator=(VkComputePipelineResource&& other) noexcept;

    void reset();
};

bool create_compute_pipeline(
    VulkanDevice& vulkan, std::string_view embedded_spirv_name,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    VkComputePipelineResource& output, std::string& error);

void write_storage_buffer_descriptor(VkComputePipelineResource& pipeline,
                                     uint32_t binding,
                                     const VkBufferResource& buffer,
                                     VkDeviceSize offset,
                                     VkDeviceSize range);

bool dispatch_compute(VulkanDevice& vulkan,
                      const VkComputePipelineResource& pipeline,
                      uint32_t group_count_x, uint32_t group_count_y,
                      uint32_t group_count_z, std::string& error);

bool run_transform_probe(VulkanDevice& vulkan,
                         const viewer::GpuMat4& packed_matrix,
                         matter::Float4 input, matter::Float4& output);

}  // namespace matter
