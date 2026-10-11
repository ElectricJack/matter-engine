#pragma once
#include "vulkan_pass.h"
namespace matter::graphics::pass {
struct Compute {
  VulkanDeviceBinding binding;
  std::vector<VkDescriptorSetLayout> sets;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  explicit Compute(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~Compute() {
    if (pipeline)
      vkDestroyPipeline(binding.device, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(binding.device, layout, nullptr);
    if (shader)
      vkDestroyShaderModule(binding.device, shader, nullptr);
    for (auto set : sets)
      if (set)
        vkDestroyDescriptorSetLayout(binding.device, set, nullptr);
  }
  void init(const uint32_t *code, size_t bytes,
            const std::vector<std::vector<VkDescriptorSetLayoutBinding>> &bindings,
            const std::vector<uint32_t> &constants = {}, uint32_t push_bytes = 0) {
    sets.resize(bindings.size());
    for (size_t i = 0; i < sets.size(); ++i) {
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.bindingCount = uint32_t(bindings[i].size());
      info.pBindings = bindings[i].data();
      vkcheck(vkCreateDescriptorSetLayout(binding.device, &info, nullptr, &sets[i]),
              "compute descriptor layout");
    }
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = uint32_t(sets.size());
    li.pSetLayouts = sets.data();
    li.pushConstantRangeCount = push_bytes ? 1u : 0u;
    li.pPushConstantRanges = &push;
    vkcheck(vkCreatePipelineLayout(binding.device, &li, nullptr, &layout), "compute pipeline layout");
    VkShaderModuleCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = bytes;
    si.pCode = code;
    vkcheck(vkCreateShaderModule(binding.device, &si, nullptr, &shader), "compute shader");
    std::vector<VkSpecializationMapEntry> entries;
    for (uint32_t i = 0; i < constants.size(); ++i)
      entries.push_back({i, i * 4, 4});
    VkSpecializationInfo spec{uint32_t(entries.size()), entries.data(), constants.size() * 4,
                              constants.data()};
    VkComputePipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = shader;
    pi.stage.pName = "main";
    pi.stage.pSpecializationInfo = &spec;
    pi.layout = layout;
    vkcheck(vkCreateComputePipelines(binding.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline),
            "compute pipeline");
  }
};
inline void compute_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                    VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                       0, 1, &b, 0, nullptr, 0, nullptr);
}
inline void dispatch(VkCommandBuffer cmd, const Compute &pipeline, const std::vector<VkDescriptorSet> &sets,
                     uint32_t x, uint32_t y = 1) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, uint32_t(sets.size()),
                          sets.data(), 0, nullptr);
  vkCmdDispatch(cmd, x, y, 1);
}
} // namespace matter::graphics::pass
