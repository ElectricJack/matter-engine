#pragma once
#include "vulkan_pass.h"
namespace matter::graphics::pass {
struct DrawPipeline {
  VulkanDeviceBinding binding;
  VkDescriptorSetLayout set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  std::array<VkShaderModule, 2> shaders{};
  explicit DrawPipeline(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~DrawPipeline() {
    if (pipeline)
      vkDestroyPipeline(binding.device, pipeline, nullptr);
    for (auto s : shaders)
      if (s)
        vkDestroyShaderModule(binding.device, s, nullptr);
    if (layout)
      vkDestroyPipelineLayout(binding.device, layout, nullptr);
    if (set)
      vkDestroyDescriptorSetLayout(binding.device, set, nullptr);
    if (render_pass)
      vkDestroyRenderPass(binding.device, render_pass, nullptr);
  }
  void init(const uint32_t *vert, size_t vert_bytes, const uint32_t *frag, size_t frag_bytes,
            const std::vector<VkDescriptorSetLayoutBinding> &bindings, const std::vector<VkFormat> &formats,
            uint32_t push_bytes, uint32_t specialization = 0, uint32_t vertex_stride = 0,
            const std::vector<VkVertexInputAttributeDescription> &attrs = {}, bool lines = false) {
    VkDescriptorSetLayoutCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    si.bindingCount = uint32_t(bindings.size());
    si.pBindings = bindings.data();
    vkcheck(vkCreateDescriptorSetLayout(binding.device, &si, nullptr, &set), "draw descriptor layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &set;
    li.pushConstantRangeCount = push_bytes ? 1u : 0u;
    li.pPushConstantRanges = &push;
    vkcheck(vkCreatePipelineLayout(binding.device, &li, nullptr, &layout), "draw pipeline layout");
    render_pass = pass::render_pass(binding.device, formats);
    const uint32_t *code[] = {vert, frag};
    const size_t sizes[] = {vert_bytes, frag_bytes};
    for (size_t i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo m{};
      m.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      m.codeSize = sizes[i];
      m.pCode = code[i];
      vkcheck(vkCreateShaderModule(binding.device, &m, nullptr, &shaders[i]), "draw shader");
    }
    VkSpecializationMapEntry entry{0, 0, 4};
    VkSpecializationInfo spec{1, &entry, 4, &specialization};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (size_t i = 0; i < 2; ++i) {
      stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
      stages[i].module = shaders[i];
      stages[i].pName = "main";
      stages[i].pSpecializationInfo = &spec;
    }
    VkVertexInputBindingDescription vb{0, vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = vertex_stride ? 1u : 0u;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const bool depth = formats.back() == VK_FORMAT_D32_SFLOAT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = ds.depthWriteEnable = depth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    std::vector<VkPipelineColorBlendAttachmentState> colors(formats.size() - (depth ? 1 : 0));
    for (auto &c : colors)
      c.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = uint32_t(colors.size());
    blend.pAttachments = colors.data();
    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dyn;
    info.layout = layout;
    info.renderPass = render_pass;
    vkcheck(vkCreateGraphicsPipelines(binding.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "draw pipeline");
  }
};
inline VkFramebuffer framebuffer(Frame &frame, VkRenderPass pass,
                                 const std::vector<std::shared_ptr<Image>> &images) {
  std::vector<VkImageView> views;
  for (const auto &im : images)
    views.push_back(im->view);
  VkFramebufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = pass;
  info.attachmentCount = uint32_t(views.size());
  info.pAttachments = views.data();
  info.width = images[0]->extent.width;
  info.height = images[0]->extent.height;
  info.layers = 1;
  VkFramebuffer result = VK_NULL_HANDLE;
  vkcheck(vkCreateFramebuffer(frame.binding.device, &info, nullptr, &result), "draw framebuffer");
  frame.framebuffers.push_back(result);
  return result;
}
inline void begin_draw(VkCommandBuffer cmd, const DrawPipeline &p, VkFramebuffer fb, uint32_t width,
                       uint32_t height, uint32_t attachments) {
  std::vector<VkClearValue> clears(attachments);
  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = p.render_pass;
  begin.framebuffer = fb;
  begin.renderArea.extent = {width, height};
  begin.clearValueCount = attachments;
  begin.pClearValues = clears.data();
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0, float(height), float(width), -float(height), 0, 1};
  VkRect2D scissor{{0, 0}, {width, height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
}
} // namespace matter::graphics::pass
