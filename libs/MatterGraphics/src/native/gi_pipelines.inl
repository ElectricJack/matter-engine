bool VkSceneRenderer::create_gi_temporal_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    std::array<VkDescriptorSetLayoutBinding, 22> bindings{};
    for (uint32_t binding = 0; binding <= 10; ++binding)
        bindings[binding] = descriptor_binding(
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[18] = descriptor_binding(18,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[19] = descriptor_binding(19,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_COMPUTE_BIT);
    for (uint32_t binding = 11; binding <= 17; ++binding)
        bindings[binding] = descriptor_binding(
            binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[20] = descriptor_binding(20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[21] = descriptor_binding(21,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_COMPUTE_BIT);
    VkDescriptorSetLayoutCreateInfo set_create{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_create.bindingCount = static_cast<uint32_t>(bindings.size());
    set_create.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(
        device, &set_create, nullptr, &gi_temporal_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(GI temporal)", result,
                       error);
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(VulkanGiTemporalConstants);
    VkPipelineLayoutCreateInfo layout_create{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_create.setLayoutCount = 1;
    layout_create.pSetLayouts = &gi_temporal_set_layout_;
    layout_create.pushConstantRangeCount = 1;
    layout_create.pPushConstantRanges = &range;
    result = vkCreatePipelineLayout(device, &layout_create, nullptr,
                                    &gi_temporal_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(GI temporal)", result, error);
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!create_shader_module(device, "gi_temporal.comp.spv", shader, error))
        return false;
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = stage;
    create.layout = gi_temporal_pipeline_layout_;
    result = profile_pipeline_call("compute", __func__, __LINE__, 1, &create, [&] {
        return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &gi_temporal_pipeline_);
    });
    vkDestroyShaderModule(device, shader, nullptr);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateComputePipelines(GI temporal)", result, error);
}

bool VkSceneRenderer::create_gi_atrous_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
    for (uint32_t binding = 0; binding < 6; ++binding)
        bindings[binding] = descriptor_binding(
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[6] = descriptor_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[7] = descriptor_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[8] = descriptor_binding(8,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_COMPUTE_BIT);
    bindings[9] = descriptor_binding(9,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_COMPUTE_BIT);
    VkDescriptorSetLayoutCreateInfo set_create{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_create.bindingCount = static_cast<uint32_t>(bindings.size());
    set_create.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(
        device, &set_create, nullptr, &gi_atrous_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(GI A-trous)", result,
                       error);
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(VulkanGiAtrousConstants);
    VkPipelineLayoutCreateInfo layout_create{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_create.setLayoutCount = 1;
    layout_create.pSetLayouts = &gi_atrous_set_layout_;
    layout_create.pushConstantRangeCount = 1;
    layout_create.pPushConstantRanges = &range;
    result = vkCreatePipelineLayout(device, &layout_create, nullptr,
                                    &gi_atrous_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(GI A-trous)", result, error);
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!create_shader_module(device, "gi_atrous.comp.spv", shader, error))
        return false;
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = stage;
    create.layout = gi_atrous_pipeline_layout_;
    result = profile_pipeline_call("compute", __func__, __LINE__, 1, &create, [&] {
        return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &gi_atrous_pipeline_);
    });
    vkDestroyShaderModule(device, shader, nullptr);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateComputePipelines(GI A-trous)", result, error);
}
