#pragma once

#include "shaders_gen/embedded_spirv.h"

namespace quick_smoke {

// Use the same device lifetime/retention mechanism as production resources.
// A failed submission must not destroy objects that might still be in use.
struct Pipeline final : matter::detail::DeviceLifetimeControl {
    explicit Pipeline(matter::VulkanDevice& device)
        : DeviceLifetimeControl(matter::detail::DeviceLifetimeAccess::token(device)) {}
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~Pipeline() override { release_device_objects(); }

protected:
    void release_device_objects() noexcept override {
        const VkDevice device = live_device();
        if (device != VK_NULL_HANDLE) {
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
            if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
        }
        pipeline = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        vertex = fragment = VK_NULL_HANDLE;
    }
};

inline void run(matter::VulkanDevice& vulkan) {
    const auto resource = std::make_shared<Pipeline>(vulkan);
    const VkDevice device = vulkan.device();
    const auto checked = [](VkResult result, const char* label) {
        CHECK(result == VK_SUCCESS, label);
        return result == VK_SUCCESS;
    };
    const auto shader = [&](const char* name, VkShaderModule& module) {
        const auto code = matter::find_spirv(name);
        CHECK(code.words != nullptr, "quick smoke shader is embedded");
        if (!code.words) return false;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.word_count * sizeof(uint32_t);
        info.pCode = code.words;
        return checked(vkCreateShaderModule(device, &info, nullptr, &module),
                       "create quick smoke shader");
    };
    if (!shader("smoke_triangle.vert.spv", resource->vertex) ||
        !shader("smoke_triangle.frag.spv", resource->fragment)) return;
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (!checked(vkCreatePipelineLayout(device, &layout, nullptr, &resource->layout),
                 "create quick smoke pipeline layout")) return;
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, resource->vertex, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, resource->fragment, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vertices{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo samples{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    const VkFormat format = vulkan.swapchain_format();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertices;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = resource->layout;
    if (!checked(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline,
                                          nullptr, &resource->pipeline),
                 "create quick smoke triangle pipeline")) return;

    std::string error;
    for (unsigned i = 0; i < 3; ++i) {
        glfwPollEvents();
        matter::VulkanFrame frame{};
        const bool began = vulkan.begin_frame(frame, error);
        CHECK(began, error.empty() ? "quick smoke begin frame" : error.c_str());
        if (!began) return;
        CHECK(frame.extent.width == 256 && frame.extent.height == 256,
              "quick smoke uses a 256x256 framebuffer");
        if (!vulkan.retain_for_frame(frame, {resource}, error)) {
            CHECK(false, error.c_str());
            return;
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = frame.swapchain_image_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo draw{VK_STRUCTURE_TYPE_RENDERING_INFO};
        draw.renderArea = {{0, 0}, frame.extent};
        draw.layerCount = draw.colorAttachmentCount = 1;
        draw.pColorAttachments = &color;
        vkCmdBeginRendering(frame.command_buffer, &draw);
        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          resource->pipeline);
        const VkViewport view{0.0f, 0.0f, float(frame.extent.width),
                              float(frame.extent.height), 0.0f, 1.0f};
        vkCmdSetViewport(frame.command_buffer, 0, 1, &view);
        vkCmdSetScissor(frame.command_buffer, 0, 1, &draw.renderArea);
        vkCmdDraw(frame.command_buffer, 3, 1, 0, 0);
        vkCmdEndRendering(frame.command_buffer);
        std::vector<uint8_t> rgba;
        const bool readback = vulkan.readback_swapchain_rgba8(frame, rgba, error);
        CHECK(readback, error.empty() ? "quick smoke queue readback" : error.c_str());
        const bool ended = vulkan.end_frame(frame, error);
        CHECK(ended, error.empty() ? "quick smoke submit and present" : error.c_str());
        if (!ended || !readback) return;
        CHECK(rgba.size() == 256u * 256u * 4u, "quick smoke readback size");
        if (rgba.size() != 256u * 256u * 4u) return;
        const size_t center = (128u * 256u + 128u) * 4u;
        CHECK(rgba[center] == 255 && rgba[center + 1] == 0 &&
                  rgba[center + 2] == 0 && rgba[center + 3] == 255,
              "quick smoke rendered triangle has a red center");
        CHECK(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
              "quick smoke background stays black outside the triangle");
    }
    std::printf("quick smoke: 256x256, 3 frames, triangle and background readback\n");
}

} // namespace quick_smoke
