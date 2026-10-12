#include "mesh_pipelines.h"
#include "vk_pipeline_stats.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
namespace viewer {
namespace {
bool fail_vk(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed: VkResult " + std::to_string(result);
    return false;
}
template<class Create, class Invoke>
VkResult profile_pipeline_call(const char*, const char*, int, uint32_t, const Create*, Invoke invoke) {
    return invoke();
}
}
bool create_mesh_raster_pipelines(VkDevice device, VkPipelineLayout layout, bool wireframe_available,
    const RasterShaderLoader& create_shader_module, const RasterPipelineCreator& create_pipeline,
    MeshRasterPipelines& output, std::string& error) {
    VkShaderModule raster_vertex = VK_NULL_HANDLE;
    VkShaderModule skinned_raster_vertex = VK_NULL_HANDLE;
    VkShaderModule raster_fragment = VK_NULL_HANDLE;
    if (!create_shader_module(device, "raster.vert.spv", raster_vertex,
                              error) ||
        !create_shader_module(device, "gbuffer.frag.spv", raster_fragment,
                              error)) {
        if (raster_vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, raster_vertex, nullptr);
        return false;
    }
    if (!create_shader_module(device, "raster_skin.vert.spv",
                              skinned_raster_vertex, error)) {
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo raster_stages[2]{};
    raster_stages[0].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    raster_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    raster_stages[0].module = raster_vertex;
    raster_stages[0].pName = "main";
    raster_stages[1].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    raster_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    raster_stages[1].module = raster_fragment;
    raster_stages[1].pName = "main";
    // Controlled terrain-cost diagnosis. Ordinary launches use the shader's
    // true default; chart-only deliberately omits connected relief and must
    // never be reported as visual/performance acceptance.
    const char* pom_path = std::getenv("MATTER_GBUFFER_POM_PATH");
    struct GbufferSpecialization {
        VkBool32 connected = VK_TRUE;
        VkBool32 work = VK_FALSE;
        int32_t profile_mode = 0;
    } specialization_data;
    const VkSpecializationMapEntry specialization_entries[] = {
        {0, offsetof(GbufferSpecialization, connected), sizeof(VkBool32)},
        {1, offsetof(GbufferSpecialization, work), sizeof(VkBool32)},
        {2, offsetof(GbufferSpecialization, profile_mode), sizeof(int32_t)}};
    const VkSpecializationInfo specialization{
        3, specialization_entries, sizeof(specialization_data), &specialization_data};
    bool use_specialization = false;
    if (pom_path && std::strcmp(pom_path, "chart_only") == 0) {
        specialization_data.connected = VK_FALSE;
        use_specialization = true;
        MATTER_LOGW("terrain-profile", "G-buffer POM path=chart_only (diagnostic; connected relief omitted)");
    } else if (pom_path && std::strcmp(pom_path, "work") == 0) {
        specialization_data.work = VK_TRUE;
        use_specialization = true;
        MATTER_LOGW("terrain-profile", "G-buffer POM path=work (diagnostic; raw albedo encodes log2(count+1)/24; horizon 9 selects walk work)");
    } else if (pom_path && std::strcmp(pom_path, "reference") != 0) {
        vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        error = "MATTER_GBUFFER_POM_PATH must be reference, chart_only or work";
        return false;
    }
    if (const char* profile = std::getenv("MATTER_GBUFFER_PROFILE_MODE")) {
        if (std::strcmp(profile, "geometry") == 0)
            specialization_data.profile_mode = 1;
        else if (std::strcmp(profile, "no_vt") == 0)
            specialization_data.profile_mode = 2;
        else if (std::strcmp(profile, "geometry_cutout") == 0)
            specialization_data.profile_mode = 3;
        else {
            vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
            vkDestroyShaderModule(device, raster_fragment, nullptr);
            vkDestroyShaderModule(device, raster_vertex, nullptr);
            error = "MATTER_GBUFFER_PROFILE_MODE must be geometry, no_vt or geometry_cutout";
            return false;
        }
        use_specialization = true;
        MATTER_LOGW("terrain-profile", "G-buffer profile mode=%s (diagnostic; image changed)", profile);
    }
    if (use_specialization)
        raster_stages[1].pSpecializationInfo = &specialization;
    VkVertexInputBindingDescription vertex_binding{
        0, sizeof(VkRasterVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
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
        // Warp field (VT Phase 2). Locations 6/7 (5 is reserved for the
        // skinned specialization's previous-position attribute).
        {6, 0, VK_FORMAT_R32G32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, warp_uv))},
        {7, 0, VK_FORMAT_R32G32_UINT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, warp_tangent))}};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 7;
    vertex_input.pVertexAttributeDescriptions = attributes;
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
    // Every mesh source (terrain surface-nets, marching-cubes scatter, skirt
    // strips) emits right-hand outward-normal geometry: counter-clockwise
    // seen from outside. The projection's y-flip and the negative-height
    // viewport cancel, so outside views land counter-clockwise in framebuffer
    // space too — hence COUNTER_CLOCKWISE front + backface culling by
    // default. MATTER_RASTER_CULL=none|front overrides for A/B comparison
    // and winding diagnosis.
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
    const VkDynamicState dynamic_values[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_values;

    // GBuffer pipeline: six surface channels, integer VT demand and depth.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable  = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    // Reversed-Z: nearer geometry has a larger NDC depth, so passing requires
    // depth >= existing (was <= under standard-Z).
    depth_stencil.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend_attachments[7]{};
    for (auto& blend : blend_attachments) {
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 7;
    color_blend.pAttachments = blend_attachments;
    const VkFormat gbuffer_formats[] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R32G32_UINT, VK_FORMAT_R8_UNORM, VK_FORMAT_R32G32B32A32_UINT};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 7;
    rendering.pColorAttachmentFormats = gbuffer_formats;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo raster_create{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    raster_create.pNext = &rendering;
    raster_create.stageCount = 2;
    raster_create.pStages = raster_stages;
    raster_create.pVertexInputState = &vertex_input;
    raster_create.pInputAssemblyState = &input_assembly;
    raster_create.pViewportState = &viewport_state;
    raster_create.pRasterizationState = &rasterization;
    raster_create.pMultisampleState = &multisample;
    raster_create.pDepthStencilState = &depth_stencil;
    raster_create.pColorBlendState = &color_blend;
    raster_create.pDynamicState = &dynamic;
    raster_create.layout = layout;
    if (std::getenv("MATTER_VK_PIPELINE_STATS") &&
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"))
        raster_create.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
            VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    VkResult result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &raster_create, [&] {
        return create_pipeline(raster_create, &output.raster);
    });
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        return fail_vk("vkCreateGraphicsPipelines(raster)", result, error);
    }
    log_vk_pipeline_stats(device, output.raster, "gbuffer.frag");

    // ---- the occlusion ID pipeline (M4) ----------------------------------
    //
    // Same vertex buffer, same indirect commands, same DrawTransform records
    // and the same pipeline layout as the gbuffer pipeline above -- so it draws
    // the identical set with the identical transforms, which is the property
    // that makes its answer mean anything about the real pass. What differs is
    // everything downstream: one uint2 attachment instead of five, a fragment
    // shader that stores a token and nothing else, and a target a fraction of
    // the size.
    //
    // Depth test AND write, because the whole mechanism is the depth test
    // deciding which instance owns each pixel. Cull mode, front face and the
    // reversed-Z compare are inherited from the state above rather than
    // restated: an ID pass that culled differently from the gbuffer would
    // answer a question about a scene that is not the one being drawn.
    {
        VkShaderModule id_vertex = VK_NULL_HANDLE;
        VkShaderModule id_fragment = VK_NULL_HANDLE;
        if (!create_shader_module(device, "visibility_id.vert.spv", id_vertex,
                                  error) ||
            !create_shader_module(device, "visibility_id.frag.spv",
                                  id_fragment, error)) {
            if (id_vertex != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, id_vertex, nullptr);
            vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
            vkDestroyShaderModule(device, raster_fragment, nullptr);
            vkDestroyShaderModule(device, raster_vertex, nullptr);
            return false;
        }
        VkPipelineShaderStageCreateInfo id_stages[2]{};
        id_stages[0].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        id_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        id_stages[0].module = id_vertex;
        id_stages[0].pName = "main";
        id_stages[1].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        id_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        id_stages[1].module = id_fragment;
        id_stages[1].pName = "main";
        VkPipelineColorBlendAttachmentState id_blend{};
        id_blend.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        VkPipelineColorBlendStateCreateInfo id_color_blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        id_color_blend.attachmentCount = 1;
        id_color_blend.pAttachments = &id_blend;
        const VkFormat id_format = VK_FORMAT_R32G32_UINT;
        VkPipelineRenderingCreateInfo id_rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        id_rendering.colorAttachmentCount = 1;
        id_rendering.pColorAttachmentFormats = &id_format;
        id_rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo id_create{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        id_create.pNext = &id_rendering;
        id_create.stageCount = 2;
        id_create.pStages = id_stages;
        id_create.pVertexInputState = &vertex_input;
        id_create.pInputAssemblyState = &input_assembly;
        id_create.pViewportState = &viewport_state;
        id_create.pRasterizationState = &rasterization;
        id_create.pMultisampleState = &multisample;
        id_create.pDepthStencilState = &depth_stencil;
        id_create.pColorBlendState = &id_color_blend;
        id_create.pDynamicState = &dynamic;
        id_create.layout = layout;
        const VkResult id_result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &id_create, [&] {
        return create_pipeline(id_create, &output.visibility);
    });
        vkDestroyShaderModule(device, id_fragment, nullptr);
        vkDestroyShaderModule(device, id_vertex, nullptr);
        if (id_result != VK_SUCCESS) {
            vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
            vkDestroyShaderModule(device, raster_fragment, nullptr);
            vkDestroyShaderModule(device, raster_vertex, nullptr);
            return fail_vk("vkCreateGraphicsPipelines(visibility id)",
                           id_result, error);
        }
    }

    // The skinned shader is a distinct SPIR-V specialization: it consumes
    // the 96-byte compute record directly and therefore cannot accidentally
    // interpret a static 72-byte VkRasterVertex as history-bearing data.
    raster_stages[0].module = skinned_raster_vertex;
    VkVertexInputBindingDescription skin_vertex_binding{
        0, sizeof(VkSkinVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription skin_attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkSkinVertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkSkinVertex, normal))},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkSkinVertex, tint))},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkSkinVertex, surface))},
        {4, 0, VK_FORMAT_R32_UINT,
         static_cast<uint32_t>(offsetof(VkSkinVertex, material_index))},
        {5, 1, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkSkinVertex, position))}};
    const VkVertexInputBindingDescription skin_vertex_bindings[] = {
        skin_vertex_binding, {1, sizeof(VkSkinVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    vertex_input.vertexBindingDescriptionCount = 2;
    vertex_input.pVertexBindingDescriptions = skin_vertex_bindings;
    vertex_input.vertexAttributeDescriptionCount = 6;
    vertex_input.pVertexAttributeDescriptions = skin_attributes;
    result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &raster_create, [&] {
        return create_pipeline(raster_create, &output.skin);
    });
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        return fail_vk("vkCreateGraphicsPipelines(skinned raster)", result,
                       error);
    }

    // Animated water consumes the packed 12-byte artifact vertex directly.
    // The vertex specialization decodes quantized position/normal values and
    // still shares the static fragment stage, water field, optics, and foam.
    VkShaderModule water_raster_vertex = VK_NULL_HANDLE;
    if (!create_shader_module(device, "raster_water.vert.spv",
                              water_raster_vertex, error)) {
        vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        return false;
    }
    raster_stages[0].module = water_raster_vertex;
    const VkVertexInputBindingDescription water_vertex_binding{
        0u, 12u, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription water_attributes[] = {
        {0u, 0u, VK_FORMAT_R32G32B32_UINT, 0u}};
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &water_vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 1u;
    vertex_input.pVertexAttributeDescriptions = water_attributes;
    result = profile_pipeline_call("graphics", __func__, __LINE__, 1u, &raster_create, [&] {
        return create_pipeline(raster_create, &output.water);
    });
    vkDestroyShaderModule(device, water_raster_vertex, nullptr);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        return fail_vk("vkCreateGraphicsPipelines(water animation raster)",
                       result, error);
    }

    // Wireframe debug view. Devices that did not enable fillModeNonSolid keep
    // the complete fill set above and leave these null; select_raster_pipelines
    // then refuses to claim wireframe, and the editor says so instead of
    // showing a filled frame under a control labelled "wireframe".
    //
    // Both variants are built from COPIES of the fill create-info. The
    // rasterization and input-assembly structs are aliased by the pipelines
    // created after this point (the composite pass among them), so mutating
    // them in place to add a line mode is how a fullscreen triangle ends up
    // rendered as three lines.
    if (wireframe_available) {
        const VkPipelineRasterizationStateCreateInfo wireframe_rasterization =
            make_wireframe_rasterization(rasterization);
        VkGraphicsPipelineCreateInfo wireframe_create = raster_create;
        wireframe_create.pRasterizationState = &wireframe_rasterization;

        VkPipelineShaderStageCreateInfo wireframe_stages[2] = {raster_stages[0],
                                                               raster_stages[1]};
        wireframe_stages[0].module = raster_vertex;
        wireframe_create.pStages = wireframe_stages;

        VkPipelineVertexInputStateCreateInfo wireframe_vertex_input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        wireframe_vertex_input.vertexBindingDescriptionCount = 1;
        wireframe_vertex_input.pVertexBindingDescriptions = &vertex_binding;
        wireframe_vertex_input.vertexAttributeDescriptionCount = 7;
        wireframe_vertex_input.pVertexAttributeDescriptions = attributes;
        wireframe_create.pVertexInputState = &wireframe_vertex_input;
        result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &wireframe_create, [&] {
        return create_pipeline(wireframe_create, &output.wireframe);
    });
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
            vkDestroyShaderModule(device, raster_fragment, nullptr);
            vkDestroyShaderModule(device, raster_vertex, nullptr);
            return fail_vk("vkCreateGraphicsPipelines(wireframe raster)",
                           result, error);
        }

        wireframe_stages[0].module = skinned_raster_vertex;
        wireframe_vertex_input.vertexBindingDescriptionCount = 2;
        wireframe_vertex_input.pVertexBindingDescriptions = skin_vertex_bindings;
        wireframe_vertex_input.vertexAttributeDescriptionCount = 6;
        wireframe_vertex_input.pVertexAttributeDescriptions = skin_attributes;
        result = profile_pipeline_call("graphics", __func__, __LINE__, 1, &wireframe_create, [&] {
        return create_pipeline(wireframe_create, &output.wireframe_skin);
    });
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
            vkDestroyShaderModule(device, raster_fragment, nullptr);
            vkDestroyShaderModule(device, raster_vertex, nullptr);
            return fail_vk(
                "vkCreateGraphicsPipelines(wireframe skinned raster)", result,
                error);
        }
    }
    vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
    vkDestroyShaderModule(device, raster_fragment, nullptr);
    vkDestroyShaderModule(device, raster_vertex, nullptr);
    return true;
}
}
