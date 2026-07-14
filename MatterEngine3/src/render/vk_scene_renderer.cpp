#include "vk_scene_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "gpu_matrix_pack.h"
#include "matter/vulkan_device.h"
#include "shaders_gen/embedded_spirv.h"

namespace viewer {
namespace {

struct alignas(16) FrameConstants {
    GpuMat4 world_to_clip;
    float frustum_planes[6][4];
    float camera_eye_pixel_budget[4];
    uint32_t counts[4];
    uint32_t capacities[4];
};

static_assert(sizeof(FrameConstants) == 208,
              "FrameConstants must match the std140 shader block");
static_assert(sizeof(VkCullStats) == 16,
              "VkCullStats must match the std430 stats block");
static_assert(sizeof(VkRasterVertex) == 56,
              "VkRasterVertex must match raster vertex bindings");

bool fail_vk(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

bool checked_u32_add(uint32_t a, uint32_t b, uint32_t& result,
                     const char* label, std::string& error) {
    if (b > std::numeric_limits<uint32_t>::max() - a) {
        error = std::string(label) + " exceeds uint32_t capacity";
        return false;
    }
    result = a + b;
    return true;
}

VkDescriptorSetLayoutBinding descriptor_binding(
    uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = 1;
    result.stageFlags = stages;
    return result;
}

bool create_shader_module(VkDevice device, const char* name,
                          VkShaderModule& shader, std::string& error) {
    const matter::EmbeddedSpirvView spirv = matter::find_spirv(name);
    if (!spirv.words || spirv.word_count == 0) {
        error = std::string("embedded SPIR-V not found: ") + name;
        return false;
    }
    VkShaderModuleCreateInfo create{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create.codeSize = spirv.word_count * sizeof(uint32_t);
    create.pCode = spirv.words;
    const VkResult result =
        vkCreateShaderModule(device, &create, nullptr, &shader);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateShaderModule", result, error);
}

void transition_for_use(VkCommandBuffer command_buffer,
                        matter::VkImageResource& image,
                        VkImageLayout new_layout,
                        VkPipelineStageFlags2 destination_stage,
                        VkAccessFlags2 destination_access,
                        VkImageAspectFlags aspect) {
    const bool undefined = image.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    matter::record_image_transition(
        command_buffer, image, new_layout,
        undefined ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        undefined ? 0 : VK_ACCESS_2_MEMORY_READ_BIT |
                            VK_ACCESS_2_MEMORY_WRITE_BIT,
        destination_stage, destination_access, aspect);
}

struct CullDispatchRecord {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t group_count;
};

void record_cull_dispatch(VkCommandBuffer command_buffer, void* user_data) {
    const auto& dispatch = *static_cast<CullDispatchRecord*>(user_data);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      dispatch.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            dispatch.layout, 0, 2, dispatch.sets, 0, nullptr);
    vkCmdDispatch(command_buffer, dispatch.group_count, 1, 1);

    VkMemoryBarrier2 barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barriers[1].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[1].dstStageMask =
        VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].dstAccessMask =
        VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 2;
    dependency.pMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

struct RasterRecord {
    matter::VkImageResource* albedo;
    matter::VkImageResource* normal;
    matter::VkImageResource* orm;
    matter::VkImageResource* depth;
    matter::VkImageResource* hdr;
    VkExtent2D extent;
    VkPipeline raster_pipeline;
    VkPipelineLayout raster_layout;
    VkDescriptorSet raster_sets[2];
    VkPipeline composite_pipeline;
    VkPipelineLayout composite_layout;
    VkDescriptorSet composite_set;
    VkBuffer vertex_buffer;
    VkBuffer indirect_buffer;
    uint32_t indirect_count;
    const uint8_t* command_enabled;
    VkSceneLighting lighting;
};

void record_raster(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterRecord*>(user_data);
    matter::VkImageResource* colors[] = {
        record.albedo, record.normal, record.orm};
    for (auto* color : colors) {
        transition_for_use(command_buffer, *color,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    }
    transition_for_use(command_buffer, *record.depth,
                       VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
    transition_for_use(command_buffer, *record.hdr,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);

    const VkClearValue clear_color{{{0.0f, 0.0f, 0.0f, 0.0f}}};
    VkRenderingAttachmentInfo color_attachments[3]{};
    for (size_t i = 0; i < 3; ++i) {
        color_attachments[i].sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[i].imageView = colors[i]->view;
        color_attachments[i].imageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachments[i].clearValue = clear_color;
    }
    VkRenderingAttachmentInfo depth_attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_attachment.imageView = record.depth->view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = record.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 3;
    rendering.pColorAttachments = color_attachments;
    rendering.pDepthAttachment = &depth_attachment;
    vkCmdBeginRendering(command_buffer, &rendering);

    // A negative height preserves the engine's top-left framebuffer
    // convention without flipping the canonical Vulkan-ZO projection.
    const VkViewport raster_viewport{
        0.0f, static_cast<float>(record.extent.height),
        static_cast<float>(record.extent.width),
        -static_cast<float>(record.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, record.extent};
    vkCmdSetViewport(command_buffer, 0, 1, &raster_viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      record.raster_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            record.raster_layout, 0, 2,
                            record.raster_sets, 0, nullptr);
    const VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &record.vertex_buffer,
                           &vertex_offset);
    // multiDrawIndirect is not required by VulkanDevice. Submit each Task 7
    // command independently, so maxDrawIndirectCount applies to drawCount=1
    // for every call rather than to the size of the command table. Any future
    // multi-draw path must validate its per-call drawCount against that limit.
    for (uint32_t i = 0; i < record.indirect_count; ++i) {
        if (record.command_enabled[i] == 0) continue;
        vkCmdDrawIndirect(command_buffer, record.indirect_buffer,
                          static_cast<VkDeviceSize>(i) * sizeof(DrawCommand),
                          1, sizeof(DrawCommand));
    }
    vkCmdEndRendering(command_buffer);

    for (auto* color : colors) {
        matter::record_image_transition(
            command_buffer, *color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }

    VkRenderingAttachmentInfo hdr_attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdr_attachment.imageView = record.hdr->view;
    hdr_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdr_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdr_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    hdr_attachment.clearValue = clear_color;
    VkRenderingInfo composite{VK_STRUCTURE_TYPE_RENDERING_INFO};
    composite.renderArea.extent = record.extent;
    composite.layerCount = 1;
    composite.colorAttachmentCount = 1;
    composite.pColorAttachments = &hdr_attachment;
    vkCmdBeginRendering(command_buffer, &composite);
    const VkViewport composite_viewport{
        0.0f, 0.0f, static_cast<float>(record.extent.width),
        static_cast<float>(record.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(command_buffer, 0, 1, &composite_viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      record.composite_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            record.composite_layout, 0, 1,
                            &record.composite_set, 0, nullptr);
    vkCmdPushConstants(command_buffer, record.composite_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(record.lighting), &record.lighting);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRendering(command_buffer);
}

struct RasterReadbackRecord {
    matter::VkImageResource* images[5];
    VkImageAspectFlags aspects[5];
    VkBuffer destination;
    uint32_t x;
    uint32_t y;
};

void record_raster_readback(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterReadbackRecord*>(user_data);
    // Each offset is aligned to its format's texel-block size (4 or 8 bytes).
    constexpr VkDeviceSize offsets[5] = {0, 8, 16, 20, 24};
    for (size_t i = 0; i < 5; ++i) {
        transition_for_use(command_buffer, *record.images[i],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT, record.aspects[i]);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offsets[i];
        copy.imageSubresource.aspectMask = record.aspects[i];
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {static_cast<int32_t>(record.x),
                            static_cast<int32_t>(record.y), 0};
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(command_buffer, record.images[i]->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               record.destination, 1, &copy);
    }
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

float half_to_float(uint16_t value) {
    const float sign = (value & 0x8000u) ? -1.0f : 1.0f;
    const uint32_t exponent = (value >> 10) & 0x1fu;
    const uint32_t mantissa = value & 0x3ffu;
    if (exponent == 0)
        return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31)
        return mantissa == 0 ? sign * std::numeric_limits<float>::infinity()
                             : std::numeric_limits<float>::quiet_NaN();
    return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                             static_cast<int>(exponent) - 15);
}

}  // namespace

namespace vk_scene_detail {

bool checked_mul_to_device_size(size_t count, size_t element_size,
                                VkDeviceSize& result, const char* label,
                                std::string& error) {
    error.clear();
    if (element_size != 0 &&
        count > std::numeric_limits<VkDeviceSize>::max() / element_size) {
        error = std::string(label) + " byte-size multiplication overflow";
        return false;
    }
    result = static_cast<VkDeviceSize>(count) *
             static_cast<VkDeviceSize>(element_size);
    return true;
}

bool checked_grown_capacity(VkDeviceSize current, VkDeviceSize required,
                            VkDeviceSize limit, VkDeviceSize& result,
                            const char* label, std::string& error) {
    error.clear();
    if (limit == 0 || required > limit || current > limit) {
        error = std::string(label) + " exceeds Vulkan device limit";
        return false;
    }
    VkDeviceSize capacity = current;
    if (capacity == 0) capacity = std::min<VkDeviceSize>(16, limit);
    while (capacity < required) {
        if (capacity > limit / 2) {
            capacity = limit;
            break;
        }
        capacity *= 2;
    }
    if (capacity < required) {
        error = std::string(label) + " capacity growth overflow";
        return false;
    }
    result = capacity;
    return true;
}

bool checked_dispatch_groups(uint32_t instance_count,
                             uint32_t max_clusters_per_instance,
                             uint32_t max_group_count_x, uint32_t& groups,
                             std::string& error) {
    error.clear();
    const uint64_t invocation_count =
        static_cast<uint64_t>(instance_count) * max_clusters_per_instance;
    if (invocation_count > std::numeric_limits<uint32_t>::max()) {
        error = "Vulkan cull dispatch exceeds uint32_t shader invocation capacity";
        return false;
    }
    const uint64_t group_count = (invocation_count + 63u) / 64u;
    if (group_count > std::numeric_limits<uint32_t>::max() ||
        group_count > max_group_count_x) {
        error = "Vulkan cull dispatch exceeds maxComputeWorkGroupCount[0]";
        return false;
    }
    groups = static_cast<uint32_t>(group_count);
    return true;
}

bool checked_size_to_int(size_t count, int& result, const char* label,
                         std::string& error) {
    error.clear();
    if (count > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = std::string(label) + " exceeds INT_MAX";
        return false;
    }
    result = static_cast<int>(count);
    return true;
}

}  // namespace vk_scene_detail

VkSceneRenderer::VkSceneRenderer(matter::VulkanDevice& vulkan)
    : vulkan_(&vulkan) {}

VkSceneRenderer::~VkSceneRenderer() {
    if (vulkan_) vulkan_->wait_idle();
    destroy_pipeline();
}

void VkSceneRenderer::destroy_pipeline() {
    if (!vulkan_) return;
    const VkDevice device = vulkan_->device();
    if (composite_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, composite_sampler_, nullptr);
    if (composite_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, composite_pipeline_, nullptr);
    if (composite_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, composite_pipeline_layout_, nullptr);
    if (composite_descriptor_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device, composite_descriptor_pool_, nullptr);
    if (composite_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, composite_set_layout_, nullptr);
    if (raster_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, raster_pipeline_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
    if (descriptor_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    for (VkDescriptorSetLayout& layout : set_layouts_) {
        if (layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    pipeline_ = VK_NULL_HANDLE;
    raster_pipeline_ = VK_NULL_HANDLE;
    composite_set_layout_ = VK_NULL_HANDLE;
    composite_pipeline_layout_ = VK_NULL_HANDLE;
    composite_pipeline_ = VK_NULL_HANDLE;
    composite_descriptor_pool_ = VK_NULL_HANDLE;
    composite_descriptor_set_ = VK_NULL_HANDLE;
    composite_sampler_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_sets_[0] = descriptor_sets_[1] = VK_NULL_HANDLE;
    initialized_ = false;
}

bool VkSceneRenderer::create_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    const VkDescriptorSetLayoutBinding frame_binding =
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_COMPUTE_BIT |
                               VK_SHADER_STAGE_VERTEX_BIT);
    VkDescriptorSetLayoutCreateInfo frame_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    frame_layout.bindingCount = 1;
    frame_layout.pBindings = &frame_binding;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &frame_layout, nullptr, &set_layouts_[0]);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(frame)", result, error);

    std::array<VkDescriptorSetLayoutBinding, 5> scene_bindings{};
    for (uint32_t i = 0; i < scene_bindings.size(); ++i)
        scene_bindings[i] =
            descriptor_binding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               VK_SHADER_STAGE_COMPUTE_BIT |
                                   (i == 3 ? VK_SHADER_STAGE_VERTEX_BIT : 0));
    VkDescriptorSetLayoutCreateInfo scene_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    scene_layout.bindingCount =
        static_cast<uint32_t>(scene_bindings.size());
    scene_layout.pBindings = scene_bindings.data();
    result = vkCreateDescriptorSetLayout(device, &scene_layout, nullptr,
                                         &set_layouts_[1]);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(scene)", result, error);

    VkPipelineLayoutCreateInfo pipeline_layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 2;
    pipeline_layout.pSetLayouts = set_layouts_;
    result = vkCreatePipelineLayout(device, &pipeline_layout, nullptr,
                                    &pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(cull)", result, error);

    const matter::EmbeddedSpirvView spirv =
        matter::find_spirv("cull.comp.spv");
    if (!spirv.words || spirv.word_count == 0) {
        error = "embedded SPIR-V not found: cull.comp.spv";
        return false;
    }
    VkShaderModule shader = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_create{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_create.codeSize = spirv.word_count * sizeof(uint32_t);
    shader_create.pCode = spirv.words;
    result = vkCreateShaderModule(device, &shader_create, nullptr, &shader);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateShaderModule(cull)", result, error);
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipeline_create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_create.stage = stage;
    pipeline_create.layout = pipeline_layout_;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &pipeline_create, nullptr, &pipeline_);
    vkDestroyShaderModule(device, shader, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateComputePipelines(cull)", result, error);

    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 2;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorPool(cull)", result, error);
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 2;
    allocate.pSetLayouts = set_layouts_;
    result = vkAllocateDescriptorSets(device, &allocate, descriptor_sets_);
    if (result != VK_SUCCESS)
        return fail_vk("vkAllocateDescriptorSets(cull)", result, error);
    return create_raster_pipelines(error);
}

bool VkSceneRenderer::create_raster_pipelines(std::string& error) {
    const VkDevice device = vulkan_->device();
    VkShaderModule raster_vertex = VK_NULL_HANDLE;
    VkShaderModule raster_fragment = VK_NULL_HANDLE;
    if (!create_shader_module(device, "raster.vert.spv", raster_vertex,
                              error) ||
        !create_shader_module(device, "gbuffer.frag.spv", raster_fragment,
                              error)) {
        if (raster_vertex != VK_NULL_HANDLE)
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
    VkVertexInputBindingDescription vertex_binding{
        0, sizeof(VkRasterVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, normal))},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, albedo))},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, orm))}};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
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
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend_attachments[3]{};
    for (auto& blend : blend_attachments) {
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 3;
    color_blend.pAttachments = blend_attachments;
    const VkDynamicState dynamic_values[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_values;
    const VkFormat gbuffer_formats[] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 3;
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
    raster_create.layout = pipeline_layout_;
    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &raster_create, nullptr, &raster_pipeline_);
    vkDestroyShaderModule(device, raster_fragment, nullptr);
    vkDestroyShaderModule(device, raster_vertex, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateGraphicsPipelines(raster)", result, error);

    std::array<VkDescriptorSetLayoutBinding, 3> sampled_bindings{};
    for (uint32_t i = 0; i < sampled_bindings.size(); ++i) {
        sampled_bindings[i] = descriptor_binding(
            i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    VkDescriptorSetLayoutCreateInfo sampled_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sampled_layout.bindingCount =
        static_cast<uint32_t>(sampled_bindings.size());
    sampled_layout.pBindings = sampled_bindings.data();
    result = vkCreateDescriptorSetLayout(device, &sampled_layout, nullptr,
                                         &composite_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(composite)", result,
                       error);
    VkPipelineLayoutCreateInfo composite_layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    composite_layout.setLayoutCount = 1;
    composite_layout.pSetLayouts = &composite_set_layout_;
    VkPushConstantRange lighting_range{};
    lighting_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lighting_range.size = sizeof(VkSceneLighting);
    composite_layout.pushConstantRangeCount = 1;
    composite_layout.pPushConstantRanges = &lighting_range;
    result = vkCreatePipelineLayout(device, &composite_layout, nullptr,
                                    &composite_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(composite)", result, error);

    VkShaderModule composite_vertex = VK_NULL_HANDLE;
    VkShaderModule composite_fragment = VK_NULL_HANDLE;
    if (!create_shader_module(device, "composite.vert.spv", composite_vertex,
                              error) ||
        !create_shader_module(device, "composite.frag.spv",
                              composite_fragment, error)) {
        if (composite_vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, composite_vertex, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo composite_stages[2]{};
    composite_stages[0].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    composite_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    composite_stages[0].module = composite_vertex;
    composite_stages[0].pName = "main";
    composite_stages[1].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    composite_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    composite_stages[1].module = composite_fragment;
    composite_stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo no_vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState hdr_blend{};
    hdr_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo hdr_color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    hdr_color_blend.attachmentCount = 1;
    hdr_color_blend.pAttachments = &hdr_blend;
    const VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipelineRenderingCreateInfo hdr_rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    hdr_rendering.colorAttachmentCount = 1;
    hdr_rendering.pColorAttachmentFormats = &hdr_format;
    VkGraphicsPipelineCreateInfo composite_create{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    composite_create.pNext = &hdr_rendering;
    composite_create.stageCount = 2;
    composite_create.pStages = composite_stages;
    composite_create.pVertexInputState = &no_vertex_input;
    composite_create.pInputAssemblyState = &input_assembly;
    composite_create.pViewportState = &viewport_state;
    composite_create.pRasterizationState = &rasterization;
    composite_create.pMultisampleState = &multisample;
    composite_create.pColorBlendState = &hdr_color_blend;
    composite_create.pDynamicState = &dynamic;
    composite_create.layout = composite_pipeline_layout_;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                       &composite_create, nullptr,
                                       &composite_pipeline_);
    vkDestroyShaderModule(device, composite_fragment, nullptr);
    vkDestroyShaderModule(device, composite_vertex, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateGraphicsPipelines(composite)", result, error);

    VkDescriptorPoolSize sampled_pool{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &sampled_pool;
    result = vkCreateDescriptorPool(device, &pool, nullptr,
                                    &composite_descriptor_pool_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorPool(composite)", result, error);
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = composite_descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &composite_set_layout_;
    result = vkAllocateDescriptorSets(device, &allocate,
                                      &composite_descriptor_set_);
    if (result != VK_SUCCESS)
        return fail_vk("vkAllocateDescriptorSets(composite)", result, error);
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    result = vkCreateSampler(device, &sampler, nullptr, &composite_sampler_);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateSampler(composite)", result, error);
}

void VkSceneRenderer::update_descriptor(
    uint32_t set_index, uint32_t binding,
    const matter::VkBufferResource& buffer) {
    VkDescriptorBufferInfo info{buffer.buffer, 0, buffer.size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_sets_[set_index];
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = set_index == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(vulkan_->device(), 1, &write, 0, nullptr);
}

bool VkSceneRenderer::ensure_buffer(matter::VkBufferResource& buffer,
                                    VkDeviceSize required_size,
                                    VkBufferUsageFlags usage,
                                    uint32_t set_index, uint32_t binding,
                                    std::string& error, bool* replaced) {
    if (replaced) *replaced = false;
    const VkDeviceSize descriptor_limit =
        set_index == 0 ? limits_.max_uniform_buffer_range
                       : limits_.max_storage_buffer_range;
    const VkDeviceSize limit =
        std::min(descriptor_limit, limits_.max_buffer_size);
    required_size = std::max<VkDeviceSize>(required_size, 1);
    if (buffer.size >= required_size) return true;
    VkDeviceSize capacity = 0;
    if (!vk_scene_detail::checked_grown_capacity(
            buffer.size, required_size, limit, capacity,
            set_index == 0 ? "uniform buffer range" : "storage buffer range",
            error)) {
        return false;
    }
    matter::VkBufferResource replacement;
    if (!matter::create_buffer(
            *vulkan_, capacity,
            usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            replacement, error)) {
        return false;
    }
    buffer = std::move(replacement);
    update_descriptor(set_index, binding, buffer);
    if (replaced) *replaced = true;
    return true;
}

bool VkSceneRenderer::ensure_vertex_buffer(VkDeviceSize required_size,
                                           std::string& error,
                                           bool* replaced) {
    if (replaced) *replaced = false;
    required_size = std::max<VkDeviceSize>(required_size, 1);
    if (vertices_.size >= required_size) return true;
    VkDeviceSize capacity = 0;
    if (!vk_scene_detail::checked_grown_capacity(
            vertices_.size, required_size, limits_.max_buffer_size, capacity,
            "vertex buffer", error)) {
        return false;
    }
    matter::VkBufferResource replacement;
    if (!matter::create_buffer(
            *vulkan_, capacity,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            replacement, error)) {
        return false;
    }
    vertices_ = std::move(replacement);
    if (replaced) *replaced = true;
    return true;
}

bool VkSceneRenderer::fail_if_poisoned(std::string& error) const {
    if (!poisoned()) return false;
    error = poison_reason_;
    return true;
}

bool VkSceneRenderer::poison(std::string& error) {
    raster_attachments_ready_ = false;
    if (!poisoned()) {
        const std::string cause =
            error.empty() ? "unknown Vulkan scene mutation failure" : error;
        poison_reason_ =
            "VkSceneRenderer poisoned after partial GPU mutation: " + cause;
    }
    error = poison_reason_;
    return false;
}

bool VkSceneRenderer::load_device_limits(std::string& error) {
    VkPhysicalDeviceMaintenance4Properties maintenance4{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &maintenance4;
    vkGetPhysicalDeviceProperties2(vulkan_->physical_device(), &properties2);
    const VkPhysicalDeviceLimits& vk_limits = properties2.properties.limits;
    physical_limits_.max_storage_buffer_range = vk_limits.maxStorageBufferRange;
    physical_limits_.max_uniform_buffer_range = vk_limits.maxUniformBufferRange;
    physical_limits_.max_dispatch_group_count_x =
        vk_limits.maxComputeWorkGroupCount[0];
    physical_limits_.max_draw_indirect_count = vk_limits.maxDrawIndirectCount;
    physical_limits_.max_buffer_size = maintenance4.maxBufferSize;
    if (physical_limits_.max_buffer_size == 0)
        physical_limits_.max_buffer_size =
            std::numeric_limits<VkDeviceSize>::max();
    limits_ = physical_limits_;
    if (limits_.max_draw_indirect_count < 1) {
        error =
            "Vulkan maxDrawIndirectCount cannot support per-call drawCount=1";
        return false;
    }
    if (limits_.max_storage_buffer_range == 0 ||
        limits_.max_uniform_buffer_range == 0 ||
        limits_.max_dispatch_group_count_x == 0) {
        error = "Vulkan device reports unusable scene buffer or dispatch limits";
        return false;
    }
    if (vk_limits.maxBoundDescriptorSets < 2 ||
        vk_limits.maxPerStageDescriptorUniformBuffers < 1 ||
        vk_limits.maxDescriptorSetUniformBuffers < 1 ||
        vk_limits.maxPerStageDescriptorStorageBuffers < 5 ||
        vk_limits.maxDescriptorSetStorageBuffers < 5) {
        error = "Vulkan device descriptor limits cannot support scene culling";
        return false;
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (use_test_limits_) {
        limits_.max_storage_buffer_range = std::min(
            limits_.max_storage_buffer_range,
            test_limits_.max_storage_buffer_range);
        limits_.max_uniform_buffer_range = std::min(
            limits_.max_uniform_buffer_range,
            test_limits_.max_uniform_buffer_range);
        limits_.max_buffer_size =
            std::min(limits_.max_buffer_size, test_limits_.max_buffer_size);
        limits_.max_dispatch_group_count_x = std::min(
            limits_.max_dispatch_group_count_x,
            test_limits_.max_dispatch_group_count_x);
        limits_.max_draw_indirect_count = std::min(
            limits_.max_draw_indirect_count,
            test_limits_.max_draw_indirect_count);
    }
#endif
    return true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void VkSceneRenderer::set_test_device_limits(
    VkDeviceSize max_storage_buffer_range,
    VkDeviceSize max_uniform_buffer_range, VkDeviceSize max_buffer_size,
    uint32_t max_dispatch_group_count_x,
    uint32_t max_draw_indirect_count) {
    if (poisoned()) return;
    test_limits_.max_storage_buffer_range = max_storage_buffer_range;
    test_limits_.max_uniform_buffer_range = max_uniform_buffer_range;
    test_limits_.max_buffer_size = max_buffer_size;
    test_limits_.max_dispatch_group_count_x = max_dispatch_group_count_x;
    test_limits_.max_draw_indirect_count = max_draw_indirect_count;
    use_test_limits_ = true;
    if (initialized_) {
        limits_.max_storage_buffer_range = std::min(
            physical_limits_.max_storage_buffer_range,
            test_limits_.max_storage_buffer_range);
        limits_.max_uniform_buffer_range = std::min(
            physical_limits_.max_uniform_buffer_range,
            test_limits_.max_uniform_buffer_range);
        limits_.max_buffer_size = std::min(physical_limits_.max_buffer_size,
                                           test_limits_.max_buffer_size);
        limits_.max_dispatch_group_count_x = std::min(
            physical_limits_.max_dispatch_group_count_x,
            test_limits_.max_dispatch_group_count_x);
        limits_.max_draw_indirect_count = std::min(
            physical_limits_.max_draw_indirect_count,
            test_limits_.max_draw_indirect_count);
    }
}

void VkSceneRenderer::clear_test_device_limits(std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return;
    use_test_limits_ = false;
    limits_ = physical_limits_;
}

bool VkSceneRenderer::set_test_command_first_instance(
    uint32_t command_index, uint32_t first_instance, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (command_index >= command_template_.size()) {
        error = "test command index is outside the command table";
        return false;
    }
    std::vector<DrawCommand> candidate = command_template_;
    candidate[command_index].first_instance = first_instance;
    uint32_t previous = 0;
    for (size_t i = 0; i < candidate.size(); ++i) {
        const uint32_t offset = candidate[i].first_instance;
        if ((i != 0 && offset < previous) || offset > draw_transform_slots_) {
            error = "draw command transform regions must be monotonic and bounded";
            return false;
        }
        previous = offset;
    }
    command_template_ = std::move(candidate);
    return true;
}

void VkSceneRenderer::set_test_scene_failure(
    uint32_t fail_after_replacements, uint32_t fail_after_uploads) {
    if (poisoned()) return;
    test_fail_after_replacements_ = fail_after_replacements;
    test_fail_after_uploads_ = fail_after_uploads;
}
#endif

bool VkSceneRenderer::init(std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (initialized_) return true;
    if (pipeline_ != VK_NULL_HANDLE) destroy_pipeline();
    if (!load_device_limits(error)) return false;
    if (!create_pipeline(error)) {
        destroy_pipeline();
        return false;
    }
    initialized_ =
        ensure_buffer(frame_constants_, sizeof(FrameConstants),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0, 0, error) &&
        ensure_buffer(clusters_, sizeof(GpuCluster),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 0, error) &&
        ensure_buffer(instances_, sizeof(GpuInstance),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 1, error) &&
        ensure_buffer(commands_, sizeof(DrawCommand),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      1, 2, error) &&
        ensure_buffer(draw_transforms_, sizeof(GpuMat4),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 3, error) &&
        ensure_buffer(stats_, sizeof(VkCullStats),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 4, error) &&
        ensure_vertex_buffer(sizeof(VkRasterVertex), error);
    if (!initialized_) {
        destroy_pipeline();
        frame_constants_.reset();
        clusters_.reset();
        instances_.reset();
        commands_.reset();
        draw_transforms_.reset();
        stats_.reset();
        vertices_.reset();
        albedo_.reset();
        normal_.reset();
        orm_.reset();
        depth_.reset();
        hdr_.reset();
        raster_extent_ = {};
    }
    return initialized_;
}

int VkSceneRenderer::ensure_part(const VkScenePart& part,
                                 std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return -1;
    const auto existing = slot_of_.find(part.part_hash);
    if (existing != slot_of_.end()) return existing->second;
    if (part.clusters.empty()) {
        error = "VkScenePart requires at least one cluster";
        return -1;
    }
    if (parts_.size() >= static_cast<size_t>(std::numeric_limits<int>::max()) ||
        part.clusters.size() > std::numeric_limits<uint32_t>::max() ||
        cluster_staging_.size() > std::numeric_limits<uint32_t>::max() -
                                      part.clusters.size()) {
        error = "VkScenePart exceeds uint32_t scene indexing capacity";
        return -1;
    }
    const size_t combined_clusters =
        cluster_staging_.size() + part.clusters.size();
    if (combined_clusters >
        std::numeric_limits<uint32_t>::max() / kVkMaxLod) {
        error = "VkScenePart exceeds uint32_t draw-command capacity";
        return -1;
    }
    if (part.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        vertex_staging_.size() > std::numeric_limits<uint32_t>::max() -
                                     part.vertices.size()) {
        error = "VkScenePart exceeds uint32_t raster vertex capacity";
        return -1;
    }
    for (const auto& cluster : part.clusters) {
        if (cluster.lods.empty() || cluster.lods.size() > kVkMaxLod) {
            error = "VkSceneCluster LOD count must be in [1, kVkMaxLod]";
            return -1;
        }
        if (!part.vertices.empty()) {
            for (const auto& lod : cluster.lods) {
                if (lod.first_vertex > part.vertices.size() ||
                    lod.vertex_count >
                        part.vertices.size() - lod.first_vertex) {
                    error = "VkSceneCluster LOD exceeds part-local vertices";
                    return -1;
                }
            }
        }
    }
    const uint32_t vertex_base =
        static_cast<uint32_t>(vertex_staging_.size());
    vertex_staging_.insert(vertex_staging_.end(), part.vertices.begin(),
                           part.vertices.end());
    const int slot = static_cast<int>(parts_.size());
    PartRecord record{};
    record.hash = part.part_hash;
    record.cluster_start = static_cast<uint32_t>(cluster_staging_.size());
    record.cluster_count = static_cast<uint32_t>(part.clusters.size());
    record.vertex_start = vertex_base;
    record.vertex_count = static_cast<uint32_t>(part.vertices.size());
    record.live = true;
    for (size_t i = 0; i < part.clusters.size(); ++i) {
        const auto& source = part.clusters[i];
        GpuCluster cluster{};
        cluster.aabb_min[0] = source.aabb_min.x;
        cluster.aabb_min[1] = source.aabb_min.y;
        cluster.aabb_min[2] = source.aabb_min.z;
        cluster.aabb_max[0] = source.aabb_max.x;
        cluster.aabb_max[1] = source.aabb_max.y;
        cluster.aabb_max[2] = source.aabb_max.z;
        cluster.radius = source.radius;
        cluster.lod_count = static_cast<uint32_t>(source.lods.size());
        cluster.part_slot = static_cast<uint32_t>(slot);
        cluster.cluster_index = static_cast<uint32_t>(i);
        for (uint32_t lod = 0; lod < kVkMaxLod; ++lod) {
            cluster.thresholds[lod] =
                lod < source.lods.size()
                    ? source.lods[lod].threshold
                    : std::numeric_limits<float>::max();
            cluster.lod_mesh_idx[lod] = lod;
        }
        cluster_staging_.push_back(cluster);
        std::vector<VkSceneLod> lods = source.lods;
        if (!part.vertices.empty()) {
            for (auto& lod : lods) lod.first_vertex += vertex_base;
        }
        cluster_lods_.push_back(std::move(lods));
    }
    parts_.push_back(record);
    slot_of_[part.part_hash] = slot;
    if (!rebuild_command_template(error)) {
        slot_of_.erase(part.part_hash);
        parts_.pop_back();
        cluster_staging_.resize(record.cluster_start);
        cluster_lods_.resize(record.cluster_start);
        vertex_staging_.resize(vertex_base);
        std::string ignored_error;
        rebuild_command_template(ignored_error);
        return -1;
    }
    return slot;
}

void VkSceneRenderer::release_part(uint64_t part_hash) {
    if (poisoned()) return;
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return;
    const uint32_t released_slot = static_cast<uint32_t>(found->second);
    std::vector<uint32_t> remap(parts_.size(),
                                std::numeric_limits<uint32_t>::max());
    std::vector<PartRecord> compact_parts;
    std::vector<GpuCluster> compact_clusters;
    std::vector<std::vector<VkSceneLod>> compact_lods;
    std::vector<VkRasterVertex> compact_vertices;
    std::map<uint64_t, int> compact_slots;
    compact_parts.reserve(parts_.size() - 1);
    compact_clusters.reserve(cluster_staging_.size() -
                             parts_[released_slot].cluster_count);
    compact_lods.reserve(compact_clusters.capacity());
    compact_vertices.reserve(vertex_staging_.size() -
                             parts_[released_slot].vertex_count);
    for (uint32_t old_slot = 0; old_slot < parts_.size(); ++old_slot) {
        if (old_slot == released_slot) continue;
        const PartRecord& old_part = parts_[old_slot];
        const uint32_t new_slot = static_cast<uint32_t>(compact_parts.size());
        remap[old_slot] = new_slot;
        PartRecord new_part = old_part;
        new_part.cluster_start = static_cast<uint32_t>(compact_clusters.size());
        new_part.vertex_start = static_cast<uint32_t>(compact_vertices.size());
        if (old_part.vertex_count != 0) {
            compact_vertices.insert(
                compact_vertices.end(),
                vertex_staging_.begin() + old_part.vertex_start,
                vertex_staging_.begin() + old_part.vertex_start +
                    old_part.vertex_count);
        }
        new_part.live = true;
        for (uint32_t i = 0; i < old_part.cluster_count; ++i) {
            GpuCluster cluster =
                cluster_staging_[old_part.cluster_start + i];
            cluster.part_slot = new_slot;
            compact_clusters.push_back(cluster);
            std::vector<VkSceneLod> lods =
                cluster_lods_[old_part.cluster_start + i];
            if (old_part.vertex_count != 0) {
                for (auto& lod : lods) {
                    lod.first_vertex =
                        new_part.vertex_start +
                        (lod.first_vertex - old_part.vertex_start);
                }
            }
            compact_lods.push_back(std::move(lods));
        }
        compact_slots[new_part.hash] = static_cast<int>(new_slot);
        compact_parts.push_back(new_part);
    }
    std::vector<GpuInstance> kept_instances;
    std::vector<uint32_t> kept_slots;
    kept_instances.reserve(instance_staging_.size());
    kept_slots.reserve(instance_part_slots_.size());
    for (size_t i = 0; i < instance_staging_.size(); ++i) {
        const uint32_t old_slot = instance_part_slots_[i];
        if (old_slot == released_slot) continue;
        const uint32_t new_slot = remap[old_slot];
        GpuInstance instance = instance_staging_[i];
        instance.part_slot = new_slot;
        instance.cluster_start = compact_parts[new_slot].cluster_start;
        instance.cluster_count = compact_parts[new_slot].cluster_count;
        kept_instances.push_back(instance);
        kept_slots.push_back(new_slot);
    }
    parts_ = std::move(compact_parts);
    slot_of_ = std::move(compact_slots);
    cluster_staging_ = std::move(compact_clusters);
    cluster_lods_ = std::move(compact_lods);
    vertex_staging_ = std::move(compact_vertices);
    instance_staging_ = std::move(kept_instances);
    instance_part_slots_ = std::move(kept_slots);
    rt_instances_.erase(
        std::remove_if(rt_instances_.begin(), rt_instances_.end(),
                       [part_hash](const RtInstance& instance) {
                           return instance.part_hash == part_hash;
                       }),
        rt_instances_.end());
    max_clusters_per_instance_ = 0;
    for (const auto& instance : instance_staging_)
        max_clusters_per_instance_ =
            std::max(max_clusters_per_instance_, instance.cluster_count);
    std::string ignored_error;
    if (!rebuild_command_template(ignored_error)) {
        instance_staging_.clear();
        instance_part_slots_.clear();
        command_template_.clear();
        raster_command_enabled_.clear();
        raster_draw_command_count_ = 0;
        draw_transform_slots_ = 0;
    }
}

bool VkSceneRenderer::update_instances(
    const std::vector<VkSceneInstance>& instances, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    int public_count = 0;
    if (!vk_scene_detail::checked_size_to_int(
            instances.size(), public_count, "VkSceneInstance count", error)) {
        return false;
    }
    (void)public_count;
    auto old_instances = std::move(instance_staging_);
    auto old_slots = std::move(instance_part_slots_);
    auto old_rt = std::move(rt_instances_);
    auto old_commands = std::move(command_template_);
    auto old_raster_enabled = std::move(raster_command_enabled_);
    const uint32_t old_raster_count = raster_draw_command_count_;
    const uint32_t old_max_clusters = max_clusters_per_instance_;
    const uint32_t old_transform_slots = draw_transform_slots_;
    instance_staging_.clear();
    instance_part_slots_.clear();
    rt_instances_.clear();
    max_clusters_per_instance_ = 0;
    for (const VkSceneInstance& source : instances) {
        const auto found = slot_of_.find(source.part_hash);
        if (found == slot_of_.end()) continue;
        const PartRecord& part = parts_[found->second];
        GpuInstance instance{};
        instance.object_to_world = pack_glsl_mat4(source.object_to_world);
        instance.part_slot = static_cast<uint32_t>(found->second);
        instance.cluster_start = part.cluster_start;
        instance.cluster_count = part.cluster_count;
        instance_staging_.push_back(instance);
        instance_part_slots_.push_back(instance.part_slot);
        max_clusters_per_instance_ =
            std::max(max_clusters_per_instance_, part.cluster_count);
        RtInstance rt{};
        rt.part_hash = source.part_hash;
        std::memcpy(rt.transform, source.object_to_world.m, sizeof(rt.transform));
        rt_instances_.push_back(rt);
    }
    if (!rebuild_command_template(error)) {
        instance_staging_ = std::move(old_instances);
        instance_part_slots_ = std::move(old_slots);
        rt_instances_ = std::move(old_rt);
        command_template_ = std::move(old_commands);
        raster_command_enabled_ = std::move(old_raster_enabled);
        raster_draw_command_count_ = old_raster_count;
        max_clusters_per_instance_ = old_max_clusters;
        draw_transform_slots_ = old_transform_slots;
        return false;
    }
    return true;
}

bool VkSceneRenderer::rebuild_command_template(std::string& error) {
    VkDeviceSize command_count = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            cluster_staging_.size(), kVkMaxLod, command_count,
            "draw-command count", error) ||
        command_count > std::numeric_limits<uint32_t>::max()) {
        if (error.empty()) error = "draw-command count exceeds uint32_t capacity";
        return false;
    }
    VkDeviceSize command_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            static_cast<size_t>(command_count), sizeof(DrawCommand),
            command_bytes, "draw-command buffer", error)) {
        return false;
    }
    const VkDeviceSize storage_limit =
        std::min(limits_.max_storage_buffer_range, limits_.max_buffer_size);
    if (storage_limit != 0 && command_bytes > storage_limit) {
        error = "draw-command buffer exceeds Vulkan storage descriptor limit";
        return false;
    }
    std::vector<uint32_t> per_part(parts_.size(), 0);
    for (uint32_t slot : instance_part_slots_) {
        if (slot >= per_part.size() || per_part[slot] ==
                                           std::numeric_limits<uint32_t>::max()) {
            error = "instance part bucket exceeds uint32_t capacity";
            return false;
        }
        ++per_part[slot];
    }
    uint32_t first_instance = 0;
    for (size_t cluster_index = 0; cluster_index < cluster_staging_.size();
         ++cluster_index) {
        const GpuCluster& cluster = cluster_staging_[cluster_index];
        const auto& lods = cluster_lods_[cluster_index];
        if (cluster.part_slot >= per_part.size()) {
            error = "cluster part bucket is outside the active part table";
            return false;
        }
        for (size_t lod = 0; lod < lods.size(); ++lod) {
            if (!checked_u32_add(first_instance, per_part[cluster.part_slot],
                                 first_instance, "draw transform slots",
                                 error)) {
                return false;
            }
        }
    }
    VkDeviceSize transform_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            first_instance, sizeof(GpuMat4), transform_bytes,
            "draw-transform buffer", error)) {
        return false;
    }
    if (storage_limit != 0 && transform_bytes > storage_limit) {
        error = "draw-transform buffer exceeds Vulkan storage descriptor limit";
        return false;
    }

    command_template_.assign(static_cast<size_t>(command_count), {});
    raster_command_enabled_.assign(static_cast<size_t>(command_count), 0);
    raster_draw_command_count_ = 0;
    uint32_t command_first_instance = 0;
    for (size_t cluster_index = 0; cluster_index < cluster_staging_.size();
         ++cluster_index) {
        const GpuCluster& cluster = cluster_staging_[cluster_index];
        const auto& lods = cluster_lods_[cluster_index];
        for (size_t lod = 0; lod < kVkMaxLod; ++lod) {
            DrawCommand& command =
                command_template_[cluster_index * kVkMaxLod + lod];
            command.first_instance = command_first_instance;
            if (lod < lods.size()) {
                command.vertex_count = lods[lod].vertex_count;
                command.first_vertex = lods[lod].first_vertex;
                if (parts_[cluster.part_slot].vertex_count != 0) {
                    raster_command_enabled_[cluster_index * kVkMaxLod + lod] =
                        1;
                    ++raster_draw_command_count_;
                }
                if (!checked_u32_add(command_first_instance,
                                     per_part[cluster.part_slot],
                                     command_first_instance,
                                     "draw transform slots", error)) {
                    command_template_.clear();
                    raster_command_enabled_.clear();
                    raster_draw_command_count_ = 0;
                    return false;
                }
            }
        }
    }
    draw_transform_slots_ = first_instance;
    return true;
}

bool VkSceneRenderer::upload_scene_buffers(std::string& error) {
    VkDeviceSize cluster_bytes = 0;
    VkDeviceSize instance_bytes = 0;
    VkDeviceSize command_bytes = 0;
    VkDeviceSize transform_bytes = 0;
    VkDeviceSize vertex_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            cluster_staging_.size(), sizeof(GpuCluster), cluster_bytes,
            "cluster buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            instance_staging_.size(), sizeof(GpuInstance), instance_bytes,
            "instance buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            command_template_.size(), sizeof(DrawCommand), command_bytes,
            "draw-command buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            draw_transform_slots_, sizeof(GpuMat4), transform_bytes,
            "draw-transform buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            vertex_staging_.size(), sizeof(VkRasterVertex), vertex_bytes,
            "vertex buffer", error)) {
        return false;
    }
    const auto storage_size_ok = [&](VkDeviceSize size, const char* label) {
        const VkDeviceSize required = std::max<VkDeviceSize>(size, 1);
        if (required > limits_.max_storage_buffer_range) {
            error = std::string(label) +
                    " exceeds Vulkan maxStorageBufferRange";
            return false;
        }
        if (required > limits_.max_buffer_size) {
            error = std::string(label) + " exceeds Vulkan maxBufferSize";
            return false;
        }
        return true;
    };
    if (!storage_size_ok(cluster_bytes, "cluster buffer") ||
        !storage_size_ok(instance_bytes, "instance buffer") ||
        !storage_size_ok(command_bytes, "draw-command buffer") ||
        !storage_size_ok(transform_bytes, "draw-transform buffer")) {
        return false;
    }
    if (std::max<VkDeviceSize>(vertex_bytes, 1) > limits_.max_buffer_size) {
        error = "vertex buffer exceeds Vulkan maxBufferSize";
        return false;
    }
    std::vector<RtInstance> candidate_rt_instances = rt_instances_;
    std::vector<uint8_t> candidate_raster_commands =
        raster_command_enabled_;
    const uint32_t candidate_raster_draw_count =
        raster_draw_command_count_;
    const uint32_t candidate_cluster_count =
        static_cast<uint32_t>(cluster_staging_.size());
    bool gpu_mutated = false;
    uint32_t replacements = 0;
    const auto ensure_scene_buffer = [&](matter::VkBufferResource& buffer,
                                         VkDeviceSize size,
                                         VkBufferUsageFlags usage,
                                         uint32_t binding) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (replacements == test_fail_after_replacements_) {
            error = "forced scene buffer replacement failure";
            return gpu_mutated ? poison(error) : false;
        }
#endif
        bool replaced = false;
        if (!ensure_buffer(buffer, size, usage, 1, binding, error, &replaced))
            return gpu_mutated ? poison(error) : false;
        if (replaced) {
            gpu_mutated = true;
            ++replacements;
        }
        return true;
    };
    if (!ensure_scene_buffer(clusters_, cluster_bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !ensure_scene_buffer(instances_, instance_bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) ||
        !ensure_scene_buffer(commands_, command_bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                             2) ||
        !ensure_scene_buffer(draw_transforms_, transform_bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 3)) {
        return false;
    }
    bool vertex_replaced = false;
    if (!ensure_vertex_buffer(vertex_bytes, error, &vertex_replaced))
        return gpu_mutated ? poison(error) : false;
    if (vertex_replaced) gpu_mutated = true;
    const VkCullStats zero_stats{};
    uint32_t uploads = 0;
    const auto upload_scene_buffer = [&](matter::VkBufferResource& buffer,
                                         const void* data, VkDeviceSize size) {
        if (size == 0) return true;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (uploads == test_fail_after_uploads_) {
            error = "forced scene buffer upload failure";
            return poison(error);
        }
#endif
        gpu_mutated = true;
        if (!matter::upload_buffer(*vulkan_, buffer, data, size, 0, error))
            return poison(error);
        ++uploads;
        return true;
    };
    const bool uploaded =
        upload_scene_buffer(clusters_, cluster_staging_.data(), cluster_bytes) &&
        upload_scene_buffer(instances_, instance_staging_.data(), instance_bytes) &&
        upload_scene_buffer(commands_, command_template_.data(), command_bytes) &&
        upload_scene_buffer(vertices_, vertex_staging_.data(), vertex_bytes) &&
        upload_scene_buffer(stats_, &zero_stats, sizeof(zero_stats));
    if (uploaded) {
        uploaded_command_count_ =
            static_cast<uint32_t>(command_template_.size());
        uploaded_transform_slots_ = draw_transform_slots_;
        uploaded_cluster_count_ = candidate_cluster_count;
        uploaded_vertex_count_ =
            static_cast<uint32_t>(vertex_staging_.size());
        uploaded_raster_command_enabled_ =
            std::move(candidate_raster_commands);
        uploaded_raster_draw_command_count_ = candidate_raster_draw_count;
        uploaded_rt_instances_ = std::move(candidate_rt_instances);
    }
    return uploaded;
}

bool VkSceneRenderer::dispatch_culling(const FrameMatrices& frame,
                                       matter::Float3 camera_eye,
                                       float pixel_budget,
                                       std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (!initialized_ && !init(error)) return false;
    if (limits_.max_draw_indirect_count < 1) {
        error =
            "Vulkan maxDrawIndirectCount cannot support per-call drawCount=1";
        return false;
    }
    uint32_t previous_first = 0;
    for (size_t i = 0; i < command_template_.size(); ++i) {
        const uint32_t first = command_template_[i].first_instance;
        if ((i != 0 && first < previous_first) ||
            first > draw_transform_slots_) {
            error = "draw command transform regions must be monotonic and bounded";
            return false;
        }
        previous_first = first;
    }
    uint32_t group_count = 0;
    if (!vk_scene_detail::checked_dispatch_groups(
            static_cast<uint32_t>(instance_staging_.size()),
            max_clusters_per_instance_, limits_.max_dispatch_group_count_x,
            group_count, error)) {
        return false;
    }
    if (!upload_scene_buffers(error)) return false;
    FrameConstants constants{};
    constants.world_to_clip = pack_glsl_mat4(frame.world_to_clip);
    std::memcpy(constants.frustum_planes, frame.frustum_planes,
                sizeof(constants.frustum_planes));
    constants.camera_eye_pixel_budget[0] = camera_eye.x;
    constants.camera_eye_pixel_budget[1] = camera_eye.y;
    constants.camera_eye_pixel_budget[2] = camera_eye.z;
    constants.camera_eye_pixel_budget[3] = pixel_budget;
    constants.counts[0] = static_cast<uint32_t>(instance_staging_.size());
    constants.counts[1] = max_clusters_per_instance_;
    constants.capacities[0] = static_cast<uint32_t>(cluster_staging_.size());
    constants.capacities[1] = static_cast<uint32_t>(instance_staging_.size());
    constants.capacities[2] = static_cast<uint32_t>(command_template_.size());
    constants.capacities[3] = draw_transform_slots_;
    if (!matter::upload_buffer(*vulkan_, frame_constants_, &constants,
                               sizeof(constants), 0, error)) {
        return poison(error);
    }
    if (instance_staging_.empty() || max_clusters_per_instance_ == 0)
        return true;
    CullDispatchRecord dispatch{pipeline_, pipeline_layout_,
                                {descriptor_sets_[0], descriptor_sets_[1]},
                                group_count};
    std::vector<std::shared_ptr<void>> dependencies{
        frame_constants_.lifetime, clusters_.lifetime, instances_.lifetime,
        commands_.lifetime, draw_transforms_.lifetime, stats_.lifetime};
    if (!matter::submit_immediate(
        *vulkan_, record_cull_dispatch, &dispatch, error,
        matter::ImmediateSubmitPhase::compute_dispatch,
        std::move(dependencies))) {
        return poison(error);
    }
    return true;
}

bool VkSceneRenderer::cull_stats(VkCullStats& stats,
                                 std::string& error) {
    stats = {};
    if (fail_if_poisoned(error)) return false;
    return matter::readback_buffer(*vulkan_, stats_,
                                   &stats, sizeof(stats), 0, error);
}

bool VkSceneRenderer::readback_commands(
    std::vector<DrawCommand>& commands, std::string& error) {
    if (fail_if_poisoned(error)) {
        commands.clear();
        return false;
    }
    commands.resize(uploaded_command_count_);
    if (commands.empty()) return true;
    VkDeviceSize bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            commands.size(), sizeof(DrawCommand), bytes,
            "draw-command readback", error)) return false;
    return matter::readback_buffer(
        *vulkan_, commands_,
        commands.data(), bytes, 0, error);
}

bool VkSceneRenderer::readback_draw_transforms(
    std::vector<GpuMat4>& transforms, std::string& error) {
    if (fail_if_poisoned(error)) {
        transforms.clear();
        return false;
    }
    transforms.resize(uploaded_transform_slots_);
    if (transforms.empty()) return true;
    VkDeviceSize bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            transforms.size(), sizeof(GpuMat4), bytes,
            "draw-transform readback", error)) return false;
    return matter::readback_buffer(
        *vulkan_, draw_transforms_, transforms.data(),
        bytes, 0, error);
}

bool VkSceneRenderer::ensure_raster_targets(uint32_t width, uint32_t height,
                                            std::string& error) {
    if (width == 0 || height == 0) {
        error = "raster attachment extent must be nonzero";
        return false;
    }
    if (raster_extent_.width == width && raster_extent_.height == height &&
        albedo_.image != VK_NULL_HANDLE && normal_.image != VK_NULL_HANDLE &&
        orm_.image != VK_NULL_HANDLE && depth_.image != VK_NULL_HANDLE &&
        hdr_.image != VK_NULL_HANDLE) {
        return true;
    }
    matter::VkImageResource albedo;
    matter::VkImageResource normal;
    matter::VkImageResource orm;
    matter::VkImageResource depth;
    matter::VkImageResource hdr;
    const VkExtent3D extent{width, height, 1};
    const VkImageUsageFlags gbuffer_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!matter::create_image(*vulkan_, VK_IMAGE_TYPE_2D,
                              VK_FORMAT_R8G8B8A8_UNORM, extent,
                              gbuffer_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, albedo,
                              error) ||
        !matter::create_image(*vulkan_, VK_IMAGE_TYPE_2D,
                              VK_FORMAT_R16G16B16A16_SFLOAT, extent,
                              gbuffer_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, normal,
                              error) ||
        !matter::create_image(*vulkan_, VK_IMAGE_TYPE_2D,
                              VK_FORMAT_R8G8B8A8_UNORM, extent,
                              gbuffer_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, orm,
                              error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, extent,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            depth, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            hdr, error)) {
        return false;
    }
    albedo_ = std::move(albedo);
    normal_ = std::move(normal);
    orm_ = std::move(orm);
    depth_ = std::move(depth);
    hdr_ = std::move(hdr);
    raster_extent_ = {width, height};
    raster_attachments_ready_ = false;

    matter::VkImageResource* sampled[] = {&albedo_, &normal_, &orm_};
    VkDescriptorImageInfo image_infos[3]{};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        image_infos[i].sampler = composite_sampler_;
        image_infos[i].imageView = sampled[i]->view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = composite_descriptor_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }
    vkUpdateDescriptorSets(vulkan_->device(), 3, writes, 0, nullptr);
    return true;
}

bool VkSceneRenderer::render_gbuffer_and_composite(uint32_t width,
                                                   uint32_t height,
                                                   std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (!initialized_ && !init(error)) return false;
    if (uploaded_command_count_ == 0 || uploaded_vertex_count_ == 0) {
        error = "raster render requires uploaded draw commands and vertices";
        return false;
    }
    if (uploaded_raster_command_enabled_.size() !=
        uploaded_command_count_) {
        error = "uploaded raster command mask is inconsistent";
        return false;
    }
    if (!ensure_raster_targets(width, height, error)) return false;
    RasterRecord record{&albedo_,
                        &normal_,
                        &orm_,
                        &depth_,
                        &hdr_,
                        raster_extent_,
                        raster_pipeline_,
                        pipeline_layout_,
                        {descriptor_sets_[0], descriptor_sets_[1]},
                        composite_pipeline_,
                        composite_pipeline_layout_,
                        composite_descriptor_set_,
                        vertices_.buffer,
                        commands_.buffer,
                        uploaded_command_count_,
                        uploaded_raster_command_enabled_.data(),
                        lighting_};
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, depth_.lifetime,
        hdr_.lifetime, vertices_.lifetime, commands_.lifetime,
        frame_constants_.lifetime, draw_transforms_.lifetime};
    raster_attachments_ready_ = false;
    if (!matter::submit_immediate(
            *vulkan_, record_raster, &record, error,
            matter::ImmediateSubmitPhase::raster_submission,
            std::move(dependencies))) {
        return poison(error);
    }
    raster_attachments_ready_ = true;
    return true;
}

bool VkSceneRenderer::record_composite_to_swapchain(
    const matter::VulkanFrame& frame, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (!raster_attachments_ready_ || hdr_.image == VK_NULL_HANDLE) {
        error = "world composite is unavailable for presentation";
        return false;
    }
    if (frame.command_buffer == VK_NULL_HANDLE ||
        frame.swapchain_image == VK_NULL_HANDLE ||
        frame.extent.width == 0 || frame.extent.height == 0) {
        error = "invalid acquired swapchain frame";
        return false;
    }

    matter::record_image_transition(
        frame.command_buffer, hdr_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageMemoryBarrier2 swap_to_transfer{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    swap_to_transfer.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_to_transfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swap_to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    swap_to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    swap_to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swap_to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swap_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swap_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swap_to_transfer.image = frame.swapchain_image;
    swap_to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    swap_to_transfer.subresourceRange.levelCount = 1;
    swap_to_transfer.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &swap_to_transfer;
    vkCmdPipelineBarrier2(frame.command_buffer, &dependency);

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {static_cast<int32_t>(raster_extent_.width),
                          static_cast<int32_t>(raster_extent_.height), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = {static_cast<int32_t>(frame.extent.width),
                          static_cast<int32_t>(frame.extent.height), 1};
    vkCmdBlitImage(frame.command_buffer, hdr_.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   frame.swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    std::swap(swap_to_transfer.srcStageMask, swap_to_transfer.dstStageMask);
    std::swap(swap_to_transfer.srcAccessMask, swap_to_transfer.dstAccessMask);
    swap_to_transfer.dstStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_to_transfer.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    std::swap(swap_to_transfer.oldLayout, swap_to_transfer.newLayout);
    vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    return true;
}

VkRasterAttachments VkSceneRenderer::raster_attachments() const {
    if (poisoned() || !raster_attachments_ready_) return {};
    return {{albedo_.image, albedo_.format},
            {normal_.image, normal_.format},
            {orm_.image, orm_.format},
            {depth_.image, depth_.format},
            {hdr_.image, hdr_.format},
            raster_extent_};
}

bool VkSceneRenderer::readback_raster_pixel(uint32_t x, uint32_t y,
                                            VkRasterPixel& pixel,
                                            std::string& error) {
    error.clear();
    pixel = {};
    pixel.depth = 1.0f;
    if (fail_if_poisoned(error)) return false;
    if (!raster_attachments_ready_) {
        error = "raster attachments are unavailable until a render completes";
        return false;
    }
    if (x >= raster_extent_.width || y >= raster_extent_.height ||
        albedo_.image == VK_NULL_HANDLE) {
        error = "raster readback pixel is outside the rendered extent";
        return false;
    }
    matter::VkBufferResource staging;
    constexpr VkDeviceSize readback_size = 32;
    if (!matter::create_buffer(
            *vulkan_, readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            staging, error)) {
        return false;
    }
    RasterReadbackRecord record{{&albedo_, &normal_, &orm_, &depth_, &hdr_},
                                {VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT},
                                staging.buffer,
                                x,
                                y};
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, depth_.lifetime,
        hdr_.lifetime, staging.lifetime};
    if (!matter::submit_immediate(
            *vulkan_, record_raster_readback, &record, error,
            matter::ImmediateSubmitPhase::compute_dispatch,
            std::move(dependencies))) {
        return poison(error);
    }
    std::array<uint8_t, readback_size> bytes{};
    if (!matter::readback_buffer(*vulkan_, staging, bytes.data(), bytes.size(),
                                 0, error)) {
        return false;
    }
    const auto unorm4 = [&](size_t offset) {
        return matter::Float4{bytes[offset] / 255.0f,
                              bytes[offset + 1] / 255.0f,
                              bytes[offset + 2] / 255.0f,
                              bytes[offset + 3] / 255.0f};
    };
    pixel.albedo = unorm4(0);
    pixel.orm = unorm4(16);
    uint16_t normal_half[4]{};
    uint16_t hdr_half[4]{};
    std::memcpy(normal_half, bytes.data() + 8, sizeof(normal_half));
    std::memcpy(&pixel.depth, bytes.data() + 20, sizeof(pixel.depth));
    std::memcpy(hdr_half, bytes.data() + 24, sizeof(hdr_half));
    pixel.normal = {half_to_float(normal_half[0]),
                    half_to_float(normal_half[1]),
                    half_to_float(normal_half[2]),
                    half_to_float(normal_half[3])};
    pixel.hdr = {half_to_float(hdr_half[0]), half_to_float(hdr_half[1]),
                 half_to_float(hdr_half[2]), half_to_float(hdr_half[3])};
    return true;
}

int VkSceneRenderer::fill_rt_instances(
    std::vector<RtInstance>& output) const {
    if (poisoned()) {
        output.clear();
        return 0;
    }
    int count = 0;
    std::string error;
    if (!vk_scene_detail::checked_size_to_int(uploaded_rt_instances_.size(), count,
                                               "RT instance count", error)) {
        output.clear();
        return 0;
    }
    output = uploaded_rt_instances_;
    return count;
}

void VkSceneRenderer::reset() {
    const bool full_reset = poisoned();
    if (full_reset) {
        if (vulkan_) vulkan_->wait_idle();
        destroy_pipeline();
        frame_constants_.reset();
        clusters_.reset();
        instances_.reset();
        commands_.reset();
        draw_transforms_.reset();
        stats_.reset();
        vertices_.reset();
        albedo_.reset();
        normal_.reset();
        orm_.reset();
        depth_.reset();
        hdr_.reset();
        raster_extent_ = {};
    }
    parts_.clear();
    slot_of_.clear();
    cluster_staging_.clear();
    cluster_lods_.clear();
    instance_staging_.clear();
    instance_part_slots_.clear();
    command_template_.clear();
    raster_command_enabled_.clear();
    uploaded_raster_command_enabled_.clear();
    rt_instances_.clear();
    vertex_staging_.clear();
    max_clusters_per_instance_ = 0;
    draw_transform_slots_ = 0;
    uploaded_command_count_ = 0;
    uploaded_transform_slots_ = 0;
    uploaded_cluster_count_ = 0;
    uploaded_vertex_count_ = 0;
    raster_draw_command_count_ = 0;
    uploaded_raster_draw_command_count_ = 0;
    uploaded_rt_instances_.clear();
    raster_attachments_ready_ = false;
    poison_reason_.clear();
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    test_fail_after_replacements_ = std::numeric_limits<uint32_t>::max();
    test_fail_after_uploads_ = std::numeric_limits<uint32_t>::max();
#endif
}

}  // namespace viewer
