bool VkSceneRenderer::create_water_forward_pipelines(std::string& error) {
    const VkDevice device = vulkan_->device();

    const VkDescriptorSetLayoutBinding bindings[] = {
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_FRAGMENT_BIT),
        descriptor_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_FRAGMENT_BIT),
        descriptor_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_FRAGMENT_BIT)};
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount =
        static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &set_info, nullptr, &water_forward_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(water forward)", result,
                       error);

    const VkDescriptorSetLayout layouts[] = {
        set_layouts_[0], set_layouts_[1], water_forward_set_layout_,
        environment_set_layout_};
    const VkPushConstantRange raster_push{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(RasterDebugPushConstants)};
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount =
        static_cast<uint32_t>(sizeof(layouts) / sizeof(layouts[0]));
    layout_info.pSetLayouts = layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &raster_push;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr,
                                    &water_forward_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(water forward)", result,
                       error);

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    result = vkCreateSampler(device, &sampler, nullptr,
                             &water_forward_sampler_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateSampler(water forward)", result, error);

    VkShaderModule static_vertex = VK_NULL_HANDLE;
    VkShaderModule direct_vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    if (!create_shader_module(device, "raster.vert.spv", static_vertex,
                              error) ||
        !create_shader_module(device, "raster_water.vert.spv", direct_vertex,
                              error) ||
        !create_shader_module(device, "water_forward.frag.spv", fragment,
                              error)) {
        if (static_vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, static_vertex, nullptr);
        if (direct_vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, direct_vertex, nullptr);
        if (fragment != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, fragment, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = static_vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription static_binding{
        0, sizeof(VkRasterVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription static_attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, normal))},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, tint))},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, surface))},
        {4, 0, VK_FORMAT_R32_UINT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, material_index))},
        {6, 0, VK_FORMAT_R32G32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, warp_uv))},
        {7, 0, VK_FORMAT_R32G32_UINT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, warp_tangent))}};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &static_binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(sizeof(static_attributes) /
                              sizeof(static_attributes[0]));
    vertex_input.pVertexAttributeDescriptions = static_attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    if (const char* cull_env = std::getenv("MATTER_RASTER_CULL")) {
        if (std::strcmp(cull_env, "none") == 0)
            rasterization.cullMode = VK_CULL_MODE_NONE;
        else if (std::strcmp(cull_env, "front") == 0)
            rasterization.cullMode = VK_CULL_MODE_FRONT_BIT;
    }
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend_attachments[4]{};
    for (auto& blend : blend_attachments) {
        blend.blendEnable = VK_FALSE;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 4;
    color_blend.pAttachments = blend_attachments;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount =
        static_cast<uint32_t>(sizeof(dynamic_states) / sizeof(dynamic_states[0]));
    dynamic.pDynamicStates = dynamic_states;
    const VkFormat color_formats[] = {
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R8_UNORM, VK_FORMAT_R32G32_UINT};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount =
        static_cast<uint32_t>(sizeof(color_formats) / sizeof(color_formats[0]));
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo create{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create.pNext = &rendering;
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &vertex_input;
    create.pInputAssemblyState = &input_assembly;
    create.pViewportState = &viewport_state;
    create.pRasterizationState = &rasterization;
    create.pMultisampleState = &multisample;
    create.pDepthStencilState = &depth_stencil;
    create.pColorBlendState = &color_blend;
    create.pDynamicState = &dynamic;
    create.layout = water_forward_pipeline_layout_;

    result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &create, [&] {
        return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create,
                                       nullptr,
                                       &water_forward_static_pipeline_);
    });
    if (result == VK_SUCCESS) {
        stages[0].module = direct_vertex;
        const VkVertexInputBindingDescription direct_binding{
            0, kWaterAnimationRasterVertexStride,
            VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription direct_attribute{
            0, 0, VK_FORMAT_R32G32B32_UINT, 0};
        vertex_input.pVertexBindingDescriptions = &direct_binding;
        vertex_input.vertexAttributeDescriptionCount = 1;
        vertex_input.pVertexAttributeDescriptions = &direct_attribute;
        result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &create, [&] {
        return vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &create, nullptr,
            &water_forward_direct_pipeline_);
    });
    }
    vkDestroyShaderModule(device, fragment, nullptr);
    vkDestroyShaderModule(device, direct_vertex, nullptr);
    vkDestroyShaderModule(device, static_vertex, nullptr);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateGraphicsPipelines(water forward)", result,
                   error);
}
