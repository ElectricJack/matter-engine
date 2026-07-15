#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vk_scene_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "gpu_matrix_pack.h"
#include "matrix_math.h"
#include "matter/vulkan_device.h"
#include "shaders_gen/embedded_spirv.h"
#include "streamline_bridge.h"

namespace viewer {
namespace {

struct alignas(16) FrameConstants {
    GpuMat4 world_to_clip;
    GpuMat4 previous_world_to_clip;
    float frustum_planes[6][4];
    float camera_eye_pixel_budget[4];
    uint32_t counts[4];
    uint32_t capacities[4];
    uint32_t temporal[4];
};

static_assert(sizeof(FrameConstants) == 288,
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

void record_cull_dispatch_commands(VkCommandBuffer command_buffer,
                                   const CullDispatchRecord& dispatch) {
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      dispatch.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            dispatch.layout, 0, 2, dispatch.sets, 0, nullptr);
    vkCmdDispatch(command_buffer, dispatch.group_count, 1, 1);

    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    memory.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void record_cull_dispatch(VkCommandBuffer command_buffer, void* user_data) {
    const auto& dispatch = *static_cast<CullDispatchRecord*>(user_data);
    record_cull_dispatch_commands(command_buffer, dispatch);

    VkMemoryBarrier2 readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    readback.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    readback.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    readback.dstStageMask =
        VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    readback.dstAccessMask =
        VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &readback;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}
#endif

struct RasterRecord {
    matter::VkImageResource* albedo;
    matter::VkImageResource* normal;
    matter::VkImageResource* orm;
    matter::VkImageResource* velocity;
    matter::VkImageResource* depth;
    matter::VkImageResource* hdr;
    matter::VkImageResource* visibility;
    VkExtent2D extent;
    VkPipeline raster_pipeline;
    VkPipelineLayout raster_layout;
    VkDescriptorSet raster_sets[2];
    VkPipeline composite_pipeline;
    VkPipelineLayout composite_layout;
    VkDescriptorSet composite_set;
    VkBuffer vertex_buffer;
    VkBuffer indirect_buffer;
    const PartCommandRange* draw_ranges;
    uint32_t draw_range_count;
    uint32_t max_draw_indirect_count;
    std::vector<PartCommandRange>* recorded_draw_ranges;
    VkSceneLighting lighting;
    bool native_ray_tracing_available;
    VkSceneRenderer* renderer;
    const matter::VulkanFrame* frame;
    const FrameMatrices* matrices;
    std::string* error;
    bool* ray_trace_ok;
};

void record_raster(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterRecord*>(user_data);
    matter::VkImageResource* colors[] = {
        record.albedo, record.normal, record.orm, record.velocity};
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
    VkRenderingAttachmentInfo color_attachments[4]{};
    for (size_t i = 0; i < 4; ++i) {
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
    rendering.colorAttachmentCount = 4;
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
    for (uint32_t i = 0; i < record.draw_range_count; ++i) {
        const PartCommandRange& range = record.draw_ranges[i];
        uint32_t remaining = range.command_count;
        uint32_t first = range.first_command;
        while (remaining != 0) {
            const uint32_t count =
                std::min(remaining, record.max_draw_indirect_count);
            vkCmdDrawIndirect(command_buffer, record.indirect_buffer,
                              static_cast<VkDeviceSize>(first) *
                                  sizeof(DrawCommand),
                              count, sizeof(DrawCommand));
            if (record.recorded_draw_ranges) {
                record.recorded_draw_ranges->push_back(
                    {first, count, range.part_slot});
            }
            first += count;
            remaining -= count;
        }
    }
    vkCmdEndRendering(command_buffer);

    for (auto* color : colors) {
        const VkPipelineStageFlags2 sampled_stages =
            color == record.velocity
                ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        matter::record_image_transition(
            command_buffer, *color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            sampled_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    matter::record_image_transition(
        command_buffer, *record.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        vk_scene_detail::ray_depth_destination_stages(
            record.native_ray_tracing_available),
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    if (record.renderer) {
        *record.ray_trace_ok = record.renderer->record_ray_traced_shadows(
            *record.frame, *record.matrices, record.extent, *record.error);
        if (!*record.ray_trace_ok) return;
    } else {
        transition_for_use(command_buffer, *record.visibility,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                             0, 1};
        vkCmdClearColorImage(command_buffer, record.visibility->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1,
                             &range);
        matter::record_image_transition(
            command_buffer, *record.visibility,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
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

#ifdef MATTER_VK_TEST_FAULT_INJECTION
struct RasterReadbackRecord {
    matter::VkImageResource* images[7];
    VkImageAspectFlags aspects[7];
    VkBuffer destination;
    uint32_t x;
    uint32_t y;
};

void record_raster_readback(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterReadbackRecord*>(user_data);
    // Each offset is aligned to its format's texel-block size (4 or 8 bytes).
    constexpr VkDeviceSize offsets[7] = {0, 8, 16, 20, 24, 32, 40};
    for (size_t i = 0; i < 7; ++i) {
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
#endif

}  // namespace

namespace vk_scene_detail {

size_t frame_constants_size_for_test() noexcept {
    return sizeof(FrameConstants);
}

VkPipelineStageFlags2 ray_depth_destination_stages(
    bool native_ray_tracing_available) noexcept {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (native_ray_tracing_available)
        stages |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    return stages;
}

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
    : vulkan_(&vulkan), dlss_bridge_(&vulkan.streamline_bridge()) {}

matter::DlssMode VkSceneRenderer::active_dlss_mode() const {
    return dlss_bridge_->active_dlss_mode();
}

const std::string& VkSceneRenderer::dlss_reason() const {
    return dlss_bridge_->dlss_unavailable_reason();
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void VkSceneRenderer::set_test_dlss_bridge(matter::StreamlineBridge bridge) {
    test_dlss_bridge_override_ =
        std::make_unique<matter::StreamlineBridge>(std::move(bridge));
    dlss_bridge_ = test_dlss_bridge_override_.get();
}

bool VkSceneRenderer::test_uses_device_streamline_bridge() const {
    return vulkan_ && dlss_bridge_ == &vulkan_->streamline_bridge();
}

std::weak_ptr<void> VkSceneRenderer::test_dlss_output_lifetime(
    uint32_t frame_slot) const {
    return frame_slot < frames_.size()
               ? std::weak_ptr<void>(frames_[frame_slot].dlss_output.lifetime)
               : std::weak_ptr<void>{};
}

bool VkSceneRenderer::test_replace_dlss_output(uint32_t frame_slot,
                                               VkExtent2D extent,
                                               std::string& error) {
    if (frame_slot >= frames_.size()) {
        error = "test DLSS output frame slot is out of range";
        return false;
    }
    return ensure_dlss_output(frames_[frame_slot], extent, error);
}
#endif

VkExtent2D VkSceneRenderer::dlss_internal_extent(
    VkExtent2D output_extent) const {
    if (!dlss_bridge_->supports_dlss_mode(selected_dlss_mode_))
        return output_extent;
    matter::DlssOptimalSettings settings{};
    std::string ignored_error;
    if (dlss_bridge_->query_dlss_optimal_settings(
            {selected_dlss_mode_, output_extent, true, true}, settings,
            ignored_error))
        return settings.render_extent;
    return output_extent;
}

bool VkSceneRenderer::consume_dlss_history_reset() {
    const bool pending = dlss_history_reset_pending_;
    dlss_history_reset_pending_ = false;
    return pending;
}

VkSceneRenderer::~VkSceneRenderer() {
    if (vulkan_) vulkan_->wait_idle();
    destroy_pipeline();
}

void VkSceneRenderer::destroy_pipeline() {
    if (!vulkan_) return;
    const VkDevice device = vulkan_->device();
    rt_sbt_.reset();
    visibility_.reset();
    if (rt_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, rt_pipeline_, nullptr);
    if (rt_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, rt_pipeline_layout_, nullptr);
    if (rt_descriptor_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device, rt_descriptor_pool_, nullptr);
    if (rt_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, rt_set_layout_, nullptr);
    if (composite_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, composite_sampler_, nullptr);
    if (composite_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, composite_pipeline_, nullptr);
    if (composite_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, composite_pipeline_layout_, nullptr);
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
    composite_sampler_ = VK_NULL_HANDLE;
    rt_pipeline_ = VK_NULL_HANDLE;
    rt_pipeline_layout_ = VK_NULL_HANDLE;
    rt_descriptor_pool_ = VK_NULL_HANDLE;
    rt_set_layout_ = VK_NULL_HANDLE;
    rt_descriptor_sets_.clear();
    rt_sbt_address_ = 0;
    rt_sbt_stride_ = 0;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    frames_.clear();
    active_frame_index_ = 0;
    frame_resource_slot_capacity_ = 0;
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

    if (!create_raster_pipelines(error)) return false;
    return !vulkan_->ray_tracing_available() ||
           create_ray_tracing_pipeline(error);
}

bool VkSceneRenderer::create_ray_tracing_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    const VkDescriptorSetLayoutBinding bindings[] = {
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR)};
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 3;
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                                   &rt_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(ray tracing)", result,
                       error);
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    push.size = 96;
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &rt_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr,
                                    &rt_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(ray tracing)", result, error);
    const char* names[] = {"rt_shadow.rgen.spv", "rt_shadow.rmiss.spv",
                           "rt_shadow.rchit.spv"};
    const VkShaderStageFlagBits stages_bits[] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    VkShaderModule modules[3]{};
    VkPipelineShaderStageCreateInfo stages[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        if (!create_shader_module(device, names[i], modules[i], error)) {
            for (VkShaderModule module : modules)
                if (module) vkDestroyShaderModule(device, module, nullptr);
            return false;
        }
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = stages_bits[i];
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
    for (auto& group : groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;
    VkRayTracingPipelineCreateInfoKHR create{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    create.stageCount = 3;
    create.pStages = stages;
    create.groupCount = 3;
    create.pGroups = groups;
    create.maxPipelineRayRecursionDepth = 1;
    create.layout = rt_pipeline_layout_;
    const auto create_pipeline = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    const auto get_handles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
    if (!create_pipeline || !get_handles) {
        error = "native ray tracing pipeline entry points are unavailable";
        result = VK_ERROR_EXTENSION_NOT_PRESENT;
    } else {
        result = create_pipeline(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                 &create, nullptr, &rt_pipeline_);
    }
    for (VkShaderModule module : modules)
        vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateRayTracingPipelinesKHR", result, error);

    const auto& props = vulkan_->ray_tracing_properties();
    if (props.shader_group_handle_size == 0 ||
        props.shader_group_handle_alignment == 0 ||
        props.shader_group_base_alignment == 0 ||
        props.max_shader_group_stride == 0 ||
        props.max_ray_dispatch_invocation_count == 0) {
        error = "Vulkan device reported unusable ray tracing pipeline limits";
        return false;
    }
    const VkDeviceSize handle_size = props.shader_group_handle_size;
    const VkDeviceSize handle_stride =
        (handle_size + props.shader_group_handle_alignment - 1) /
        props.shader_group_handle_alignment *
        props.shader_group_handle_alignment;
    if (handle_stride > props.max_shader_group_stride) {
        error = "ray tracing SBT stride exceeds maxShaderGroupStride";
        return false;
    }
    const VkDeviceSize region_stride =
        (handle_stride + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    std::vector<uint8_t> handles(static_cast<size_t>(3 * handle_size));
    result = get_handles(device, rt_pipeline_, 0, 3, handles.size(),
                         handles.data());
    if (result != VK_SUCCESS)
        return fail_vk("vkGetRayTracingShaderGroupHandlesKHR", result, error);
    if (!matter::create_buffer(
            *vulkan_, 3 * region_stride +
                           props.shader_group_base_alignment - 1,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rt_sbt_, error) ||
        !matter::map_buffer(rt_sbt_, error)) return false;
    rt_sbt_address_ =
        (rt_sbt_.address + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    rt_sbt_stride_ = handle_stride;
    const VkDeviceSize mapped_offset = rt_sbt_address_ - rt_sbt_.address;
    std::memset(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset, 0,
                static_cast<size_t>(3 * region_stride));
    for (uint32_t i = 0; i < 3; ++i)
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        i * region_stride,
                    handles.data() + i * handle_size,
                    static_cast<size_t>(handle_size));
    return matter::flush_buffer(rt_sbt_, mapped_offset, 3 * region_stride,
                                error);
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
    VkPipelineColorBlendAttachmentState blend_attachments[4]{};
    for (auto& blend : blend_attachments) {
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 4;
    color_blend.pAttachments = blend_attachments;
    const VkDynamicState dynamic_values[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_values;
    const VkFormat gbuffer_formats[] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 4;
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

    std::array<VkDescriptorSetLayoutBinding, 4> sampled_bindings{};
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
    VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
    const matter::VkBufferResource& buffer) {
    VkDescriptorBufferInfo info{buffer.buffer, 0, buffer.size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(vulkan_->device(), 1, &write, 0, nullptr);
}

bool VkSceneRenderer::ensure_buffer(matter::VkBufferResource& buffer,
                                     VkDeviceSize required_size,
                                     VkBufferUsageFlags usage,
                                     std::string& error, bool* replaced) {
    if (replaced) *replaced = false;
    const bool uniform = (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0;
    const VkDeviceSize descriptor_limit =
        uniform ? limits_.max_uniform_buffer_range
                : limits_.max_storage_buffer_range;
    const VkDeviceSize limit =
        std::min(descriptor_limit, limits_.max_buffer_size);
    required_size = std::max<VkDeviceSize>(required_size, 1);
    if (buffer.size >= required_size) return true;
    VkDeviceSize capacity = 0;
    if (!vk_scene_detail::checked_grown_capacity(
            buffer.size, required_size, limit, capacity,
            uniform ? "uniform buffer range" : "storage buffer range",
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
    if (replaced) *replaced = true;
    return true;
}

bool VkSceneRenderer::ensure_build_buffer(
    matter::VkBufferResource& buffer, VkDeviceSize required_size,
    VkBufferUsageFlags usage, std::string& error) {
    required_size = std::max<VkDeviceSize>(required_size, 1);
    if (buffer.size >= required_size) return true;
    VkDeviceSize capacity = 0;
    if (!vk_scene_detail::checked_grown_capacity(
            buffer.size, required_size, limits_.max_buffer_size, capacity,
            "acceleration-structure build buffer", error)) return false;
    matter::VkBufferResource replacement;
    if (!matter::create_buffer(
            *vulkan_, capacity,
            usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            replacement, error)) return false;
    buffer = std::move(replacement);
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

bool VkSceneRenderer::ensure_frame_resources(uint32_t frame_slot_count,
                                             std::string& error) {
    if (frame_slot_count == 0) {
        error = "Vulkan frame reports zero frame slots";
        return false;
    }
    if (frames_.size() >= frame_slot_count &&
        frame_resource_slot_capacity_ >= frame_slot_count) {
        return true;
    }
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame_slot_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_slot_count * 5},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         frame_slot_count * 4}};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = frame_slot_count * 3;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = pool_sizes;
    VkDescriptorPool next_pool = VK_NULL_HANDLE;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (test_fail_after_frame_resource_allocations_ == 0) {
        error = "forced frame resource allocation failure";
        return false;
    }
#endif
    VkResult result =
        vkCreateDescriptorPool(vulkan_->device(), &pool, nullptr, &next_pool);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorPool(cull)", result, error);
    std::vector<FrameResources> next_frames(frame_slot_count);
    std::vector<VkDescriptorSetLayout> layouts;
    layouts.reserve(frame_slot_count * 3);
    for (size_t index = 0; index < frame_slot_count; ++index) {
        layouts.push_back(set_layouts_[0]);
        layouts.push_back(set_layouts_[1]);
        layouts.push_back(composite_set_layout_);
    }
    std::vector<VkDescriptorSet> sets(layouts.size());
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = next_pool;
    allocate.descriptorSetCount = static_cast<uint32_t>(sets.size());
    allocate.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(vulkan_->device(), &allocate, sets.data());
    if (result != VK_SUCCESS) {
        vkDestroyDescriptorPool(vulkan_->device(), next_pool, nullptr);
        return fail_vk("vkAllocateDescriptorSets(cull)", result, error);
    }
    uint32_t allocations = 1;
    const auto ensure_candidate_buffer = [&](matter::VkBufferResource& buffer,
                                             VkDeviceSize size,
                                             VkBufferUsageFlags usage) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (allocations == test_fail_after_frame_resource_allocations_) {
            error = "forced frame resource allocation failure";
            return false;
        }
#endif
        if (!ensure_buffer(buffer, size, usage, error)) return false;
        ++allocations;
        return true;
    };
    for (size_t index = 0; index < frame_slot_count; ++index) {
        FrameResources& frame = next_frames[index];
        frame.descriptor_sets[0] = sets[index * 3];
        frame.descriptor_sets[1] = sets[index * 3 + 1];
        frame.composite_descriptor_set = sets[index * 3 + 2];
        if (!ensure_candidate_buffer(frame.frame_constants,
                                     sizeof(FrameConstants),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.instances, sizeof(GpuInstance),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(
                frame.commands, sizeof(DrawCommand),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.draw_transforms, sizeof(GpuDrawTransform),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.stats, sizeof(VkCullStats),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
            vkDestroyDescriptorPool(vulkan_->device(), next_pool, nullptr);
            return false;
        }
        update_frame_descriptors(frame);
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vulkan_->wait_idle();
        vkDestroyDescriptorPool(vulkan_->device(), descriptor_pool_, nullptr);
    }
    frames_ = std::move(next_frames);
    descriptor_pool_ = next_pool;
    if (vulkan_->ray_tracing_available()) {
        if (rt_descriptor_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(vulkan_->device(), rt_descriptor_pool_,
                                    nullptr);
        const VkDescriptorPoolSize rt_sizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, frame_slot_count},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frame_slot_count},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count}};
        VkDescriptorPoolCreateInfo rt_pool{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        rt_pool.maxSets = frame_slot_count;
        rt_pool.poolSizeCount = 3;
        rt_pool.pPoolSizes = rt_sizes;
        VkResult rt_result = vkCreateDescriptorPool(
            vulkan_->device(), &rt_pool, nullptr, &rt_descriptor_pool_);
        if (rt_result != VK_SUCCESS)
            return fail_vk("vkCreateDescriptorPool(ray tracing)", rt_result,
                           error);
        rt_descriptor_sets_.resize(frame_slot_count);
        std::vector<VkDescriptorSetLayout> rt_layouts(frame_slot_count,
                                                       rt_set_layout_);
        VkDescriptorSetAllocateInfo rt_allocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        rt_allocate.descriptorPool = rt_descriptor_pool_;
        rt_allocate.descriptorSetCount = frame_slot_count;
        rt_allocate.pSetLayouts = rt_layouts.data();
        rt_result = vkAllocateDescriptorSets(vulkan_->device(), &rt_allocate,
                                             rt_descriptor_sets_.data());
        if (rt_result != VK_SUCCESS)
            return fail_vk("vkAllocateDescriptorSets(ray tracing)", rt_result,
                           error);
    }
    frame_resource_slot_capacity_ = frame_slot_count;
    active_frame_index_ = 0;
    return true;
}

void VkSceneRenderer::update_frame_descriptors(FrameResources& frame) {
    update_descriptor(frame.descriptor_sets[0], 0,
                      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                      frame.frame_constants);
    update_descriptor(frame.descriptor_sets[1], 0,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, clusters_);
    update_descriptor(frame.descriptor_sets[1], 1,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.instances);
    update_descriptor(frame.descriptor_sets[1], 2,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.commands);
    update_descriptor(frame.descriptor_sets[1], 3,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      frame.draw_transforms);
    update_descriptor(frame.descriptor_sets[1], 4,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.stats);
}

void VkSceneRenderer::update_composite_descriptor(FrameResources& frame) {
    matter::VkImageResource* sampled[] = {&albedo_, &normal_, &orm_,
                                          &visibility_};
    VkDescriptorImageInfo image_infos[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        image_infos[i].sampler = composite_sampler_;
        image_infos[i].imageView = sampled[i]->view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frame.composite_descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }
    vkUpdateDescriptorSets(vulkan_->device(), 4, writes, 0, nullptr);
}

void VkSceneRenderer::note_command_layout_rebuild() {
    ++command_generation_;
    ++upload_counters_.command_layout_rebuilds;
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
    ++command_generation_;
    return true;
}

void VkSceneRenderer::set_test_scene_failure(
    uint32_t fail_after_replacements, uint32_t fail_after_uploads) {
    if (poisoned()) return;
    test_fail_after_replacements_ = fail_after_replacements;
    test_fail_after_uploads_ = fail_after_uploads;
}

void VkSceneRenderer::set_test_frame_resource_failure(
    uint32_t fail_after_allocations) {
    if (poisoned()) return;
    test_fail_after_frame_resource_allocations_ = fail_after_allocations;
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
    if (sizeof(FrameConstants) >
        std::min(limits_.max_uniform_buffer_range, limits_.max_buffer_size)) {
        error = "uniform buffer range exceeds Vulkan device limit";
        destroy_pipeline();
        return false;
    }
    initialized_ =
        ensure_buffer(clusters_, sizeof(GpuCluster),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) &&
        ensure_vertex_buffer(sizeof(VkRasterVertex), error);
    if (!initialized_) {
        destroy_pipeline();
        clusters_.reset();
        vertices_.reset();
        albedo_.reset();
        normal_.reset();
        orm_.reset();
        velocity_.reset();
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
    std::shared_ptr<matter::VkBufferResource> rt_geometry;
    std::shared_ptr<matter::VkAccelerationStructureResource> rt_blas;
    uint32_t rt_primitive_count = 0;
    if (vulkan_->ray_tracing_available() && !part.vertices.empty()) {
        rt_geometry = std::make_shared<matter::VkBufferResource>();
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(part.vertices.size()) *
            sizeof(VkRasterVertex);
        if (!matter::create_buffer(
                *vulkan_, bytes,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, *rt_geometry, error) ||
            !matter::map_buffer(*rt_geometry, error)) {
            return -1;
        }
        std::memcpy(rt_geometry->mapped, part.vertices.data(),
                    static_cast<size_t>(bytes));
        if (!matter::flush_buffer(*rt_geometry, 0, bytes, error)) return -1;
        rt_primitive_count = static_cast<uint32_t>(part.vertices.size() / 3);
        if (rt_primitive_count != 0) {
            VkAccelerationStructureGeometryTrianglesDataKHR triangles{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = rt_geometry->address;
            triangles.vertexStride = sizeof(VkRasterVertex);
            triangles.maxVertex = static_cast<uint32_t>(part.vertices.size() - 1);
            triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
            VkAccelerationStructureGeometryKHR geometry{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            geometry.geometry.triangles = triangles;
            VkAccelerationStructureBuildGeometryInfoKHR build{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags =
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries = &geometry;
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            const auto get_sizes = reinterpret_cast<
                PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(
                vulkan_->device(), "vkGetAccelerationStructureBuildSizesKHR"));
            if (!get_sizes) {
                error = "vkGetAccelerationStructureBuildSizesKHR unavailable";
                return -1;
            }
            get_sizes(vulkan_->device(),
                      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                      &build, &rt_primitive_count, &sizes);
            rt_blas = std::make_shared<matter::VkAccelerationStructureResource>();
            if (!matter::create_acceleration_structure(
                    *vulkan_, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                    sizes.accelerationStructureSize, *rt_blas, error)) return -1;
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
    record.rt_geometry = std::move(rt_geometry);
    record.rt_blas = std::move(rt_blas);
    record.rt_primitive_count = rt_primitive_count;
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
    ++static_generation_;
    static_upload_dirty_ = true;
    note_command_layout_rebuild();
    return slot;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
VkDeviceAddress VkSceneRenderer::test_rt_geometry_address(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return 0;
    const PartRecord& part = parts_[static_cast<size_t>(found->second)];
    return part.rt_geometry ? part.rt_geometry->address : 0;
}

bool VkSceneRenderer::test_rt_blas_built(uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    return found != slot_of_.end() &&
           parts_[static_cast<size_t>(found->second)].rt_blas_built;
}

uint64_t VkSceneRenderer::test_rt_blas_candidate_serial(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    return found == slot_of_.end()
               ? 0
               : parts_[static_cast<size_t>(found->second)]
                     .rt_blas_candidate_serial;
}

VkDeviceAddress VkSceneRenderer::test_rt_scratch_address(
    uint32_t frame_slot) const {
    if (frame_slot >= frames_.size()) return 0;
    const VkDeviceAddress address = frames_[frame_slot].rt_scratch.address;
    const VkDeviceSize alignment = vulkan_->ray_tracing_properties()
                                       .min_acceleration_structure_scratch_offset_alignment;
    return alignment == 0 ? address
                          : (address + alignment - 1) / alignment * alignment;
}
#endif

void VkSceneRenderer::finish_ray_tracing_frame(uint64_t frame_serial,
                                               bool succeeded) {
    if (frame_serial == 0) return;
    for (auto& part : parts_) {
        if (part.rt_blas_candidate_serial != frame_serial) continue;
        part.rt_blas_built = succeeded;
        part.rt_blas_candidate_serial = 0;
    }
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
        part_instance_counts_.clear();
        command_template_.clear();
        part_command_ranges_.clear();
        raster_command_enabled_.clear();
        raster_draw_command_count_ = 0;
        draw_transform_slots_ = 0;
    } else {
        ++static_generation_;
        ++instance_generation_;
        static_upload_dirty_ = true;
        note_command_layout_rebuild();
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
    std::vector<GpuInstance> candidate_instances;
    std::vector<uint32_t> candidate_slots;
    std::vector<RtInstance> candidate_rt;
    candidate_instances.reserve(instances.size());
    candidate_slots.reserve(instances.size());
    candidate_rt.reserve(instances.size());
    uint32_t candidate_max_clusters = 0;
    for (size_t source_index = 0; source_index < instances.size();
         ++source_index) {
        const VkSceneInstance& source = instances[source_index];
        const auto found = slot_of_.find(source.part_hash);
        if (found == slot_of_.end()) continue;
        const PartRecord& part = parts_[found->second];
        GpuInstance instance{};
        instance.object_to_world = pack_glsl_mat4(source.object_to_world);
        instance.previous_object_to_world = instance.object_to_world;
        const uint64_t stable_id = source.instance_id;
        const auto temporal = std::find_if(
            temporal_frame_.instances.begin(), temporal_frame_.instances.end(),
            [stable_id](const TemporalInstanceFrame& item) {
                return item.instance_id == stable_id;
            });
        if (!temporal_frame_.reset && temporal != temporal_frame_.instances.end() &&
            temporal->history_valid) {
            instance.previous_object_to_world =
                pack_glsl_mat4(temporal->previous_object_to_world);
            instance.history_valid = 1;
        }
        instance.part_slot = static_cast<uint32_t>(found->second);
        instance.cluster_start = part.cluster_start;
        instance.cluster_count = part.cluster_count;
        candidate_instances.push_back(instance);
        candidate_slots.push_back(instance.part_slot);
        candidate_max_clusters =
            std::max(candidate_max_clusters, part.cluster_count);
        RtInstance rt{};
        rt.part_hash = source.part_hash;
        std::memcpy(rt.transform, source.object_to_world.m, sizeof(rt.transform));
        candidate_rt.push_back(rt);
    }
    const bool identical =
        candidate_instances.size() == instance_staging_.size() &&
        candidate_slots == instance_part_slots_ &&
        std::equal(candidate_instances.begin(), candidate_instances.end(),
                   instance_staging_.begin(),
                   [](const GpuInstance& left, const GpuInstance& right) {
                       return std::memcmp(&left, &right, sizeof(left)) == 0;
                   });
    if (identical) return true;

    const bool layout_changed = candidate_slots != instance_part_slots_;
    if (!layout_changed) {
        instance_staging_ = std::move(candidate_instances);
        instance_part_slots_ = std::move(candidate_slots);
        rt_instances_ = std::move(candidate_rt);
        max_clusters_per_instance_ = candidate_max_clusters;
        ++instance_generation_;
        return true;
    }
    auto old_instances = std::move(instance_staging_);
    auto old_slots = std::move(instance_part_slots_);
    auto old_rt = std::move(rt_instances_);
    auto old_commands = std::move(command_template_);
    auto old_raster_enabled = std::move(raster_command_enabled_);
    auto old_part_counts = std::move(part_instance_counts_);
    auto old_part_ranges = std::move(part_command_ranges_);
    const uint32_t old_raster_count = raster_draw_command_count_;
    const uint32_t old_max_clusters = max_clusters_per_instance_;
    const uint32_t old_transform_slots = draw_transform_slots_;
    instance_staging_ = std::move(candidate_instances);
    instance_part_slots_ = std::move(candidate_slots);
    rt_instances_ = std::move(candidate_rt);
    max_clusters_per_instance_ = candidate_max_clusters;
    if (layout_changed && !rebuild_command_template(error)) {
        instance_staging_ = std::move(old_instances);
        instance_part_slots_ = std::move(old_slots);
        rt_instances_ = std::move(old_rt);
        command_template_ = std::move(old_commands);
        raster_command_enabled_ = std::move(old_raster_enabled);
        part_instance_counts_ = std::move(old_part_counts);
        part_command_ranges_ = std::move(old_part_ranges);
        raster_draw_command_count_ = old_raster_count;
        max_clusters_per_instance_ = old_max_clusters;
        draw_transform_slots_ = old_transform_slots;
        return false;
    }
    ++instance_generation_;
    if (layout_changed) note_command_layout_rebuild();
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
    std::vector<PartCommandRange> next_part_ranges;
    next_part_ranges.reserve(parts_.size());
    for (uint32_t slot = 0; slot < parts_.size(); ++slot) {
        const PartRecord& part = parts_[slot];
        if (!part.live || per_part[slot] == 0 || part.cluster_count == 0 ||
            part.vertex_count == 0)
            continue;
        next_part_ranges.push_back(
            {part.cluster_start * kVkMaxLod,
             part.cluster_count * kVkMaxLod, slot});
    }
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
    part_instance_counts_ = std::move(per_part);
    part_command_ranges_ = std::move(next_part_ranges);
    return true;
}

bool VkSceneRenderer::upload_scene_buffers(FrameResources& frame,
                                           bool reset_stats,
                                           std::string& error) {
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
            draw_transform_slots_, sizeof(GpuDrawTransform), transform_bytes,
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
    uint32_t replacements = 0;
#ifndef MATTER_VK_TEST_FAULT_INJECTION
    (void)replacements;
#endif
    uint32_t uploads = 0;
    const auto allow_replacement = [&] {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (replacements == test_fail_after_replacements_) {
            error = "forced scene buffer replacement failure";
            return replacements == 0 ? false : poison(error);
        }
#endif
        return true;
    };
    const auto upload = [&](matter::VkBufferResource& buffer, const void* data,
                            VkDeviceSize size) {
        if (size == 0) return true;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (uploads == test_fail_after_uploads_) {
            error = "forced scene buffer upload failure";
            return poison(error);
        }
#endif
        if (!matter::upload_buffer(*vulkan_, buffer, data, size, 0, error))
            return poison(error);
        ++uploads;
        return true;
    };
    if (static_upload_dirty_) {
        const auto replacement_capacity = [&](VkDeviceSize current,
                                              VkDeviceSize required,
                                              const char* label,
                                              VkDeviceSize& capacity) {
            required = std::max<VkDeviceSize>(required, 1);
            if (current >= required) {
                capacity = current;
                return true;
            }
            return vk_scene_detail::checked_grown_capacity(
                current, required, limits_.max_buffer_size, capacity, label,
                error);
        };
        VkDeviceSize cluster_capacity = 0;
        VkDeviceSize vertex_capacity = 0;
        if (!replacement_capacity(clusters_.size, cluster_bytes, "cluster buffer",
                                  cluster_capacity) ||
            !replacement_capacity(vertices_.size, vertex_bytes, "vertex buffer",
                                  vertex_capacity)) {
            return false;
        }
        matter::VkBufferResource next_clusters;
        matter::VkBufferResource next_vertices;
        if (!allow_replacement()) return false;
        if (!matter::create_buffer(
                *vulkan_, cluster_capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                next_clusters, error)) {
            return false;
        }
        ++replacements;
        if (!allow_replacement()) return false;
        if (!matter::create_buffer(
                *vulkan_, vertex_capacity,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                next_vertices, error)) {
            return false;
        }
        ++replacements;
        if (!upload(next_clusters, cluster_staging_.data(), cluster_bytes) ||
            !upload(next_vertices, vertex_staging_.data(), vertex_bytes)) {
            return false;
        }
        clusters_ = std::move(next_clusters);
        vertices_ = std::move(next_vertices);
        static_upload_dirty_ = false;
        if (cluster_bytes != 0) ++upload_counters_.cluster_uploads;
        if (vertex_bytes != 0) ++upload_counters_.vertex_uploads;
        uploaded_cluster_count_ =
            static_cast<uint32_t>(cluster_staging_.size());
        uploaded_vertex_count_ =
            static_cast<uint32_t>(vertex_staging_.size());
    }

    if (frame.static_generation != static_generation_) {
        update_descriptor(frame.descriptor_sets[1], 0,
                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, clusters_);
        frame.static_generation = static_generation_;
    }
    bool descriptors_changed = false;
    bool replaced = false;
    if (!ensure_buffer(frame.instances, instance_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error, &replaced))
        return poison(error);
    descriptors_changed |= replaced;
    if (!ensure_buffer(frame.commands, command_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                       error, &replaced))
        return poison(error);
    descriptors_changed |= replaced;
    if (!ensure_buffer(frame.draw_transforms, transform_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error, &replaced))
        return poison(error);
    descriptors_changed |= replaced;
    if (!ensure_buffer(frame.stats, sizeof(VkCullStats),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error, &replaced))
        return poison(error);
    descriptors_changed |= replaced;
    if (descriptors_changed) update_frame_descriptors(frame);

    if (frame.instance_generation != instance_generation_) {
        if (!upload(frame.instances, instance_staging_.data(), instance_bytes))
            return false;
        if (instance_bytes != 0) ++upload_counters_.instance_uploads;
        frame.instance_generation = instance_generation_;
    }
    if (frame.command_generation != command_generation_)
        frame.command_generation = command_generation_;
    if (!upload(frame.commands, command_template_.data(), command_bytes))
        return false;
    if (command_bytes != 0) ++upload_counters_.command_uploads;
    if (reset_stats) {
        const VkCullStats zero_stats{};
        if (!upload(frame.stats, &zero_stats, sizeof(zero_stats))) return false;
        frame.stats_valid = false;
    }
    uploaded_command_count_ = static_cast<uint32_t>(command_template_.size());
    uploaded_transform_slots_ = draw_transform_slots_;
    uploaded_raster_command_enabled_ = raster_command_enabled_;
    uploaded_raster_draw_command_count_ = raster_draw_command_count_;
    uploaded_rt_instances_ = rt_instances_;
    return true;
}

bool VkSceneRenderer::upload_frame_constants(FrameResources& frame,
                                              const FrameMatrices& matrices,
                                              matter::Float3 camera_eye,
                                              float pixel_budget,
                                              std::string& error) {
    FrameConstants constants{};
    constants.world_to_clip = pack_glsl_mat4(matrices.world_to_clip);
    constants.previous_world_to_clip = pack_glsl_mat4(
        temporal_frame_.attempt_token != 0
            ? temporal_frame_.previous_jittered.world_to_clip
            : matrices.world_to_clip);
    std::memcpy(constants.frustum_planes, matrices.frustum_planes,
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
    constants.temporal[0] = matrices.jitter_pixels[0] != 0.0f ||
                                    matrices.jitter_pixels[1] != 0.0f
                                ? 1u
                                : 0u;
    constants.temporal[1] = temporal_frame_.reset ? 1u : 0u;
    constants.temporal[2] = temporal_frame_.internal_extent.width;
    constants.temporal[3] = temporal_frame_.internal_extent.height;
    return matter::upload_buffer(*vulkan_, frame.frame_constants, &constants,
                                 sizeof(constants), 0, error);
}

bool VkSceneRenderer::prepare_frame(const matter::VulkanFrame& frame,
                                    const FrameMatrices& matrices,
                                    matter::Float3 camera_eye,
                                    float pixel_budget, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (!initialized_ && !init(error)) return false;
    if (frame.frame_slot >= frame.frame_slot_count) {
        error = "Vulkan frame slot is outside its reported slot count";
        return false;
    }
    if (!ensure_frame_resources(frame.frame_slot_count, error)) return false;
    FrameResources& selected = frames_[frame.frame_slot];
    if (!upload_scene_buffers(selected, false, error) ||
        !upload_frame_constants(selected, matrices, camera_eye, pixel_budget,
                                error)) {
        return false;
    }
    if (!matter::map_buffer(selected.stats, error)) return poison(error);
    if (selected.stats_valid) {
        if (!matter::invalidate_buffer(selected.stats, 0, sizeof(VkCullStats),
                                       error)) {
            return poison(error);
        }
        std::memcpy(&cached_stats_, selected.stats.mapped,
                    sizeof(cached_stats_));
    }
    // A slot becomes publishable only when this frame records culling
    // successfully. A later record failure must not publish the cleared buffer.
    selected.stats_valid = false;
    std::memset(selected.stats.mapped, 0, sizeof(VkCullStats));
    if (!matter::flush_buffer(selected.stats, 0, sizeof(VkCullStats), error))
        return poison(error);
    active_frame_index_ = frame.frame_slot;
    std::vector<std::shared_ptr<void>> resources{
        clusters_.lifetime,
        vertices_.lifetime,
        selected.frame_constants.lifetime,
        selected.instances.lifetime,
        selected.commands.lifetime,
        selected.draw_transforms.lifetime,
        selected.stats.lifetime,
        albedo_.lifetime,
        normal_.lifetime,
        orm_.lifetime,
        depth_.lifetime,
        hdr_.lifetime};
    return vulkan_->retain_for_frame(frame, std::move(resources), error);
}

bool VkSceneRenderer::validate_draw_command_regions(std::string& error) const {
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
    return true;
}

bool VkSceneRenderer::record_ray_traced_shadows(
    const matter::VulkanFrame& frame, const FrameMatrices& matrices,
    VkExtent2D trace_extent, std::string& error) {
    auto clear_visibility = [&]() {
        transition_for_use(frame.command_buffer, visibility_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                             0, 1};
        vkCmdClearColorImage(frame.command_buffer, visibility_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1,
                             &range);
        matter::record_image_transition(
            frame.command_buffer, visibility_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    };
    const bool native_trace_enabled =
        ray_tracing_settings_.enabled && vulkan_->ray_tracing_available();
    if (!native_trace_enabled || rt_instances_.empty()) {
        clear_visibility();
        return true;
    }
    const auto& rt_properties = vulkan_->ray_tracing_properties();
    const uint64_t dispatch_invocations =
        static_cast<uint64_t>(trace_extent.width) * trace_extent.height;
    if (dispatch_invocations >
        rt_properties.max_ray_dispatch_invocation_count) {
        error = "ray tracing dispatch exceeds maxRayDispatchInvocationCount";
        return false;
    }
    if (frame.frame_slot >= frames_.size() ||
        frame.frame_slot >= rt_descriptor_sets_.size()) {
        error = "ray tracing requires prepared per-frame resources";
        return false;
    }
    const auto get_sizes = reinterpret_cast<
        PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(
        vulkan_->device(), "vkGetAccelerationStructureBuildSizesKHR"));
    const auto cmd_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(vulkan_->device(),
                            "vkCmdBuildAccelerationStructuresKHR"));
    const auto cmd_trace = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        vkGetDeviceProcAddr(vulkan_->device(), "vkCmdTraceRaysKHR"));
    if (!get_sizes || !cmd_build || !cmd_trace) {
        error = "native ray tracing command entry points are unavailable";
        return false;
    }
    FrameResources& selected = frames_[frame.frame_slot];
    VkDeviceSize scratch_size = 1;
    struct PendingBlas {
        PartRecord* part;
        VkAccelerationStructureGeometryKHR geometry;
        VkAccelerationStructureBuildGeometryInfoKHR build;
        VkAccelerationStructureBuildRangeInfoKHR range;
    };
    std::vector<PendingBlas> pending;
    for (auto& part : parts_) {
        if (!part.live || part.rt_blas_built ||
            part.rt_blas_candidate_serial != 0 || !part.rt_geometry ||
            !part.rt_blas || part.rt_primitive_count == 0) continue;
        const bool active = std::any_of(
            rt_instances_.begin(), rt_instances_.end(),
            [&](const RtInstance& instance) {
                return instance.part_hash == part.hash;
            });
        if (!active) continue;
        PendingBlas item{};
        item.part = &part;
        auto& triangles = item.geometry.geometry.triangles;
        triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = part.rt_geometry->address;
        triangles.vertexStride = sizeof(VkRasterVertex);
        triangles.maxVertex = part.rt_primitive_count * 3 - 1;
        triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        item.geometry.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        item.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        item.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        item.build.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        item.build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        item.build.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        item.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        item.build.dstAccelerationStructure = part.rt_blas->handle;
        item.build.geometryCount = 1;
        item.build.pGeometries = &item.geometry;
        item.range.primitiveCount = part.rt_primitive_count;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        get_sizes(vulkan_->device(),
                  VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                  &item.build, &item.range.primitiveCount, &sizes);
        scratch_size = std::max(scratch_size, sizes.buildScratchSize);
        pending.push_back(item);
    }
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        1, vulkan_->ray_tracing_properties()
               .min_acceleration_structure_scratch_offset_alignment);
    if (!ensure_build_buffer(
            selected.rt_scratch, scratch_size + scratch_alignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            error)) return false;
    const VkDeviceAddress blas_scratch_address =
        (selected.rt_scratch.address + scratch_alignment - 1) /
        scratch_alignment * scratch_alignment;
    for (auto& item : pending) {
        item.build.pGeometries = &item.geometry;
        item.build.scratchData.deviceAddress = blas_scratch_address;
        const VkAccelerationStructureBuildRangeInfoKHR* range = &item.range;
        cmd_build(frame.command_buffer, 1, &item.build, &range);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(rt_instances_.size());
    for (const RtInstance& source : rt_instances_) {
        const auto found = slot_of_.find(source.part_hash);
        if (found == slot_of_.end()) continue;
        const PartRecord& part = parts_[static_cast<size_t>(found->second)];
        if (!part.rt_blas) continue;
        VkAccelerationStructureInstanceKHR instance{};
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                instance.transform.matrix[row][col] =
                    source.transform[col * 4 + row];
        instance.instanceCustomIndex = static_cast<uint32_t>(found->second);
        instance.mask = 0xff;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = part.rt_blas->address;
        instances.push_back(instance);
    }
    if (instances.empty()) {
        clear_visibility();
        return true;
    }
    const VkDeviceSize instance_bytes =
        instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    if (!ensure_build_buffer(selected.rt_instances, instance_bytes,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       error) ||
        !matter::map_buffer(selected.rt_instances, error)) return false;
    std::memcpy(selected.rt_instances.mapped, instances.data(),
                static_cast<size_t>(instance_bytes));
    if (!matter::flush_buffer(selected.rt_instances, 0, instance_bytes, error))
        return false;
    VkAccelerationStructureGeometryInstancesDataKHR instance_data{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instance_data.data.deviceAddress = selected.rt_instances.address;
    VkAccelerationStructureGeometryKHR tlas_geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.geometry.instances = instance_data;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build.geometryCount = 1;
    tlas_build.pGeometries = &tlas_geometry;
    const uint32_t instance_count = static_cast<uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(vulkan_->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
              &tlas_build, &instance_count, &tlas_sizes);
    if (selected.rt_tlas.size < tlas_sizes.accelerationStructureSize) {
        selected.rt_tlas.reset();
        if (!matter::create_acceleration_structure(
                *vulkan_, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                tlas_sizes.accelerationStructureSize, selected.rt_tlas,
                error)) return false;
    }
    if (!ensure_build_buffer(
            selected.rt_tlas_scratch,
            tlas_sizes.buildScratchSize + scratch_alignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            error)) return false;
    const VkDeviceAddress tlas_scratch_address =
        (selected.rt_tlas_scratch.address + scratch_alignment - 1) /
        scratch_alignment * scratch_alignment;
    tlas_build.dstAccelerationStructure = selected.rt_tlas.handle;
    tlas_build.scratchData.deviceAddress = tlas_scratch_address;
    VkAccelerationStructureBuildRangeInfoKHR tlas_range{};
    tlas_range.primitiveCount = instance_count;
    const VkAccelerationStructureBuildRangeInfoKHR* tlas_range_ptr = &tlas_range;
    cmd_build(frame.command_buffer, 1, &tlas_build, &tlas_range_ptr);
    VkMemoryBarrier2 as_to_ray{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    as_to_ray.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    as_to_ray.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    as_to_ray.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    as_to_ray.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo as_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    as_dependency.memoryBarrierCount = 1;
    as_dependency.pMemoryBarriers = &as_to_ray;
    vkCmdPipelineBarrier2(frame.command_buffer, &as_dependency);

    transition_for_use(frame.command_buffer, visibility_, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    VkWriteDescriptorSetAccelerationStructureKHR as_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &selected.rt_tlas.handle;
    VkDescriptorImageInfo depth_info{composite_sampler_, depth_.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo visibility_info{VK_NULL_HANDLE, visibility_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &as_write;
    writes[0].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (uint32_t i = 1; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 1
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = i == 1 ? &depth_info : &visibility_info;
    }
    vkUpdateDescriptorSets(vulkan_->device(), 3, writes, 0, nullptr);
    struct alignas(16) ShadowConstants {
        GpuMat4 clip_to_world;
        float to_sun_max_distance[4];
        float bias;
        uint32_t samples;
        uint32_t debug_view;
        uint32_t pad;
    } constants{};
    constants.clip_to_world = pack_glsl_mat4(matrices.clip_to_world);
    const float x = -lighting_.sun_direction.x;
    const float y = -lighting_.sun_direction.y;
    const float z = -lighting_.sun_direction.z;
    const float length = std::sqrt(x*x + y*y + z*z);
    constants.to_sun_max_distance[0] = length > 0 ? x / length : 0;
    constants.to_sun_max_distance[1] = length > 0 ? y / length : 1;
    constants.to_sun_max_distance[2] = length > 0 ? z / length : 0;
    constants.to_sun_max_distance[3] = ray_tracing_settings_.max_distance;
    constants.bias = ray_tracing_settings_.bias;
    constants.samples = std::max(1u, ray_tracing_settings_.samples);
    constants.debug_view = ray_tracing_settings_.debug_view ? 1u : 0u;
    last_rt_samples_ = constants.samples;
    last_rt_debug_view_ = constants.debug_view != 0;
    vkCmdBindPipeline(frame.command_buffer,
                      VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline_);
    const VkDescriptorSet rt_set = rt_descriptor_sets_[frame.frame_slot];
    vkCmdBindDescriptorSets(frame.command_buffer,
                            VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            rt_pipeline_layout_, 0, 1, &rt_set, 0, nullptr);
    vkCmdPushConstants(frame.command_buffer, rt_pipeline_layout_,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(constants),
                       &constants);
    const auto& props = vulkan_->ray_tracing_properties();
    const VkDeviceSize handle_stride =
        (props.shader_group_handle_size + props.shader_group_handle_alignment - 1) /
        props.shader_group_handle_alignment * props.shader_group_handle_alignment;
    const VkDeviceSize region_stride =
        (handle_stride + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    const VkStridedDeviceAddressRegionKHR raygen{rt_sbt_address_, handle_stride,
                                                  handle_stride};
    const VkStridedDeviceAddressRegionKHR miss{rt_sbt_address_ + region_stride,
                                                handle_stride, handle_stride};
    const VkStridedDeviceAddressRegionKHR hit{rt_sbt_address_ + 2 * region_stride,
                                               handle_stride, handle_stride};
    const VkStridedDeviceAddressRegionKHR callable{};
    cmd_trace(frame.command_buffer, &raygen, &miss, &hit, &callable,
              trace_extent.width, trace_extent.height, 1);
    last_rt_effective_ = true;
    ++last_rt_trace_dispatches_;
    last_rt_fallback_reason_.clear();
    matter::record_image_transition(
        frame.command_buffer, visibility_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    std::vector<std::shared_ptr<void>> retained{visibility_.lifetime,
        selected.rt_instances.lifetime, selected.rt_scratch.lifetime,
        selected.rt_tlas_scratch.lifetime,
        selected.rt_tlas.lifetime, rt_sbt_.lifetime};
    for (const auto& part : parts_) {
        if (part.rt_geometry) retained.push_back(part.rt_geometry->lifetime);
        if (part.rt_blas) retained.push_back(part.rt_blas->lifetime);
    }
    if (!vulkan_->retain_for_frame(frame, std::move(retained), error))
        return false;
    for (auto& item : pending)
        item.part->rt_blas_candidate_serial = frame.serial;
    return true;
}

bool VkSceneRenderer::record_cull_and_render(
    const matter::VulkanFrame& frame, const FrameMatrices& matrices,
    matter::Float3 camera_eye, float pixel_budget, std::string& error) {
    error.clear();
    recorded_draw_ranges_.clear();
    last_rt_available_ = vulkan_->ray_tracing_available();
    last_rt_effective_ = false;
    last_rt_trace_dispatches_ = 0;
    last_rt_samples_ = ray_tracing_settings_.samples;
    last_rt_debug_view_ = ray_tracing_settings_.debug_view;
    if (!ray_tracing_settings_.enabled) {
        last_rt_fallback_reason_ = "disabled by render options";
    } else if (!last_rt_available_) {
        last_rt_fallback_reason_ = vulkan_->ray_tracing_unavailable_reason();
    } else {
        last_rt_fallback_reason_ = "no traceable RT instances";
    }
    if (fail_if_poisoned(error)) return false;
    if (!initialized_ && !init(error)) return false;
    if (!vulkan_->multi_draw_indirect_enabled()) {
        error = "Vulkan multiDrawIndirect is required for grouped scene rasterization";
        return false;
    }
    if (frame.command_buffer == VK_NULL_HANDLE ||
        frame.frame_slot >= frame.frame_slot_count ||
        frame.frame_slot >= frames_.size() ||
        frame.frame_slot != active_frame_index_) {
        error = "record_cull_and_render requires prepared acquired frame resources";
        return false;
    }
    if (limits_.max_draw_indirect_count < 1) {
        error = "Vulkan maxDrawIndirectCount cannot support grouped indirect draws";
        return false;
    }
    if (!validate_draw_command_regions(error)) return false;
    const VkExtent2D internal_extent =
        temporal_frame_.internal_extent.width != 0
            ? temporal_frame_.internal_extent
            : frame.extent;
    if (!ensure_raster_targets(internal_extent.width, internal_extent.height,
                               error))
        return false;

    FrameResources& selected = frames_[frame.frame_slot];
    update_composite_descriptor(selected);
    // prepare_frame owns the existing scene resources for this slot. Newly
    // created/replaced attachments are retained here before commands reference
    // them, preserving the frame-lifetime contract.
    std::vector<std::shared_ptr<void>> attachments{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        depth_.lifetime, hdr_.lifetime, visibility_.lifetime};
    if (!vulkan_->retain_for_frame(frame, std::move(attachments), error))
        return false;

    uint32_t group_count = 0;
    if (!vk_scene_detail::checked_dispatch_groups(
            static_cast<uint32_t>(instance_staging_.size()),
            max_clusters_per_instance_, limits_.max_dispatch_group_count_x,
            group_count, error)) {
        return false;
    }
    if (group_count != 0) {
        const CullDispatchRecord dispatch{
            pipeline_, pipeline_layout_,
            {selected.descriptor_sets[0], selected.descriptor_sets[1]},
            group_count};
        record_cull_dispatch_commands(frame.command_buffer, dispatch);
    }

    bool ray_trace_ok = true;
    VkSceneLighting frame_lighting = lighting_;
    frame_lighting.pad1 =
        ray_tracing_settings_.enabled && vulkan_->ray_tracing_available() &&
                ray_tracing_settings_.debug_view
            ? 1.0f
            : 0.0f;
    RasterRecord record{&albedo_,
                        &normal_,
                        &orm_,
                        &velocity_,
                        &depth_,
                        &hdr_,
                        &visibility_,
                        raster_extent_,
                        raster_pipeline_,
                        pipeline_layout_,
                        {selected.descriptor_sets[0], selected.descriptor_sets[1]},
                        composite_pipeline_,
                        composite_pipeline_layout_,
                        selected.composite_descriptor_set,
                        vertices_.buffer,
                        selected.commands.buffer,
                        part_command_ranges_.data(),
                        static_cast<uint32_t>(part_command_ranges_.size()),
                        limits_.max_draw_indirect_count,
                        &recorded_draw_ranges_,
                        frame_lighting,
                        vulkan_->ray_tracing_available(),
                        this,
                        &frame,
                        &matrices,
                        &error,
                        &ray_trace_ok};
    record_raster(frame.command_buffer, &record);
    if (!ray_trace_ok) return false;
    raster_attachments_ready_ = true;
    selected.stats_valid = true;
    (void)camera_eye;
    (void)pixel_budget;
    return true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
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
    if (!validate_draw_command_regions(error)) return false;
    uint32_t group_count = 0;
    if (!vk_scene_detail::checked_dispatch_groups(
            static_cast<uint32_t>(instance_staging_.size()),
            max_clusters_per_instance_, limits_.max_dispatch_group_count_x,
            group_count, error)) {
        return false;
    }
    if (!ensure_frame_resources(1, error)) return false;
    FrameResources& selected = frames_[0];
    if (!upload_scene_buffers(selected, true, error)) return false;
    if (!upload_frame_constants(selected, frame, camera_eye, pixel_budget,
                                error))
        return poison(error);
    active_frame_index_ = 0;
    if (instance_staging_.empty() || max_clusters_per_instance_ == 0)
        return true;
    CullDispatchRecord dispatch{pipeline_, pipeline_layout_,
                                {selected.descriptor_sets[0],
                                 selected.descriptor_sets[1]},
                                group_count};
    std::vector<std::shared_ptr<void>> dependencies{
        selected.frame_constants.lifetime, clusters_.lifetime,
        selected.instances.lifetime, selected.commands.lifetime,
        selected.draw_transforms.lifetime, selected.stats.lifetime};
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
    if (frames_.empty()) {
        error = "Vulkan cull stats are unavailable before frame preparation";
        return false;
    }
    return matter::readback_buffer(*vulkan_, frames_[active_frame_index_].stats,
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
    if (frames_.empty()) {
        error = "Vulkan draw commands are unavailable before frame preparation";
        return false;
    }
    return matter::readback_buffer(
        *vulkan_, frames_[active_frame_index_].commands,
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
            transforms.size(), sizeof(GpuDrawTransform), bytes,
            "draw-transform readback", error)) return false;
    if (frames_.empty()) {
        error = "Vulkan draw transforms are unavailable before frame preparation";
        return false;
    }
    std::vector<GpuDrawTransform> packed(transforms.size());
    if (!matter::readback_buffer(
            *vulkan_, frames_[active_frame_index_].draw_transforms,
            packed.data(), bytes, 0, error)) {
        return false;
    }
    for (size_t index = 0; index < transforms.size(); ++index)
        transforms[index] = packed[index].current;
    return true;
}

#endif

bool VkSceneRenderer::ensure_raster_targets(uint32_t width, uint32_t height,
                                            std::string& error) {
    if (width == 0 || height == 0) {
        error = "raster attachment extent must be nonzero";
        return false;
    }
    if (raster_extent_.width == width && raster_extent_.height == height &&
        albedo_.image != VK_NULL_HANDLE && normal_.image != VK_NULL_HANDLE &&
        orm_.image != VK_NULL_HANDLE && depth_.image != VK_NULL_HANDLE &&
        velocity_.image != VK_NULL_HANDLE && hdr_.image != VK_NULL_HANDLE &&
        visibility_.image != VK_NULL_HANDLE) {
        return true;
    }
    matter::VkImageResource albedo;
    matter::VkImageResource normal;
    matter::VkImageResource orm;
    matter::VkImageResource velocity;
    matter::VkImageResource depth;
    matter::VkImageResource hdr;
    matter::VkImageResource visibility;
    const VkExtent3D extent{width, height, 1};
    VkImageUsageFlags visibility_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vulkan_->ray_tracing_available())
        visibility_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
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
        !matter::create_image(*vulkan_, VK_IMAGE_TYPE_2D,
                              VK_FORMAT_R16G16_SFLOAT, extent,
                              gbuffer_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, velocity,
                              error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, extent,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            depth, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            hdr, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, extent,
            visibility_usage,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            visibility, error)) {
        return false;
    }
    albedo_ = std::move(albedo);
    normal_ = std::move(normal);
    orm_ = std::move(orm);
    velocity_ = std::move(velocity);
    depth_ = std::move(depth);
    hdr_ = std::move(hdr);
    visibility_ = std::move(visibility);
    visibility_usage_ = visibility_usage;
    raster_extent_ = {width, height};
    raster_attachments_ready_ = false;

    return true;
}

bool VkSceneRenderer::ensure_dlss_output(FrameResources& frame,
                                         VkExtent2D output_extent,
                                         std::string& error) {
    if (output_extent.width == 0 || output_extent.height == 0) {
        error = "DLSS output extent must be nonzero";
        return false;
    }
    if (frame.dlss_output.image != VK_NULL_HANDLE &&
        frame.dlss_output_extent.width == output_extent.width &&
        frame.dlss_output_extent.height == output_extent.height) {
        return true;
    }
    matter::VkImageResource replacement;
    if (!matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT,
            {output_extent.width, output_extent.height, 1},
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            replacement, error)) {
        return false;
    }
    frame.dlss_output = std::move(replacement);
    frame.dlss_output_extent = output_extent;
    return true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
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
    if (frames_.empty()) {
        error = "raster render requires prepared frame resources";
        return false;
    }
    FrameResources& selected = frames_[active_frame_index_];
    update_composite_descriptor(selected);
    RasterRecord record{&albedo_,
                        &normal_,
                        &orm_,
                        &velocity_,
                        &depth_,
                        &hdr_,
                        &visibility_,
                        raster_extent_,
                        raster_pipeline_,
                        pipeline_layout_,
                        {selected.descriptor_sets[0], selected.descriptor_sets[1]},
                        composite_pipeline_,
                        composite_pipeline_layout_,
                        selected.composite_descriptor_set,
                        vertices_.buffer,
                        selected.commands.buffer,
                        part_command_ranges_.data(),
                        static_cast<uint32_t>(part_command_ranges_.size()),
                        limits_.max_draw_indirect_count,
                        nullptr,
                        lighting_,
                        false,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr};
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        depth_.lifetime, hdr_.lifetime, visibility_.lifetime,
        vertices_.lifetime, selected.commands.lifetime,
        selected.frame_constants.lifetime, selected.draw_transforms.lifetime};
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

#endif

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

    matter::VkImageResource* composite_source = &hdr_;
    VkPipelineStageFlags2 composite_source_stage =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags2 composite_source_access =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    const auto consume_bridge_reset = [this]() {
        if (!dlss_bridge_->consume_dlss_history_reset()) return;
        if (!dlss_history_reset_pending_) ++dlss_reset_count_;
        dlss_history_reset_pending_ = true;
    };
    if (selected_dlss_mode_ == matter::DlssMode::Native) {
        matter::DlssEvaluationOutput ignored_output{};
        matter::DlssConstants ignored_constants{};
        matter::DlssResources ignored_resources{};
        std::string transition_error;
        (void)dlss_bridge_->evaluate_dlss(
            frame.command_buffer, temporal_frame_.attempt_token,
            {matter::DlssMode::Native, frame.extent, true, true},
            ignored_constants, ignored_resources, ignored_output,
            transition_error);
        consume_bridge_reset();
    }
    if (selected_dlss_mode_ != matter::DlssMode::Native &&
        dlss_bridge_->supports_dlss_mode(selected_dlss_mode_) &&
        frame.frame_slot < frames_.size()) {
        FrameResources& slot = frames_[frame.frame_slot];
        if (!ensure_dlss_output(slot, frame.extent, error)) return false;
        if (!vulkan_->retain_for_frame(frame, {slot.dlss_output.lifetime}, error))
            return false;

        matter::record_image_transition(
            frame.command_buffer, hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        const bool output_was_presented =
            slot.dlss_output.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        matter::record_image_transition(
            frame.command_buffer, slot.dlss_output, VK_IMAGE_LAYOUT_GENERAL,
            output_was_presented ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                 : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            output_was_presented ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        matter::DlssConstants constants{};
        std::memcpy(constants.camera_view_to_clip,
                    temporal_frame_.current_unjittered.view_to_clip.m,
                    sizeof(constants.camera_view_to_clip));
        matter::Mat4f clip_to_view{};
        (void)mat4_inverse(temporal_frame_.current_unjittered.view_to_clip,
                           clip_to_view);
        std::memcpy(constants.clip_to_camera_view, clip_to_view.m,
                    sizeof(constants.clip_to_camera_view));
        const matter::Mat4f clip_to_prev = mat4_mul(
            temporal_frame_.previous_unjittered.world_to_clip,
            temporal_frame_.current_unjittered.clip_to_world);
        const matter::Mat4f prev_to_clip = mat4_mul(
            temporal_frame_.current_unjittered.world_to_clip,
            temporal_frame_.previous_unjittered.clip_to_world);
        std::memcpy(constants.clip_to_prev_clip, clip_to_prev.m,
                    sizeof(constants.clip_to_prev_clip));
        std::memcpy(constants.prev_clip_to_clip, prev_to_clip.m,
                    sizeof(constants.prev_clip_to_clip));
        constants.jitter_offset = {temporal_frame_.jitter_pixels[0],
                                   temporal_frame_.jitter_pixels[1]};
        constants.motion_vector_scale = {
            1.0f / static_cast<float>(raster_extent_.width),
            1.0f / static_cast<float>(raster_extent_.height)};
        matter::Mat4f view_to_world{};
        if (mat4_inverse(temporal_frame_.current_unjittered.world_to_view,
                         view_to_world)) {
            constants.camera_position[0] = view_to_world.m[3];
            constants.camera_position[1] = view_to_world.m[7];
            constants.camera_position[2] = view_to_world.m[11];
            constants.camera_right[0] = view_to_world.m[0];
            constants.camera_right[1] = view_to_world.m[4];
            constants.camera_right[2] = view_to_world.m[8];
            constants.camera_up[0] = view_to_world.m[1];
            constants.camera_up[1] = view_to_world.m[5];
            constants.camera_up[2] = view_to_world.m[9];
            constants.camera_forward[0] = -view_to_world.m[2];
            constants.camera_forward[1] = -view_to_world.m[6];
            constants.camera_forward[2] = -view_to_world.m[10];
        }
        const matter::Mat4f& projection =
            temporal_frame_.current_unjittered.view_to_clip;
        constants.camera_near = projection.m[11] / projection.m[10];
        constants.camera_far =
            projection.m[11] / (projection.m[10] + 1.0f);
        constants.camera_fov = 2.0f * std::atan(1.0f / projection.m[5]);
        constants.camera_aspect_ratio = projection.m[5] / projection.m[0];
        constants.depth_inverted = false;
        constants.camera_motion_included = true;
        constants.motion_vectors_jittered = true;
        constants.reset = temporal_frame_.reset;
        constants.internal_extent = raster_extent_;
        constants.output_extent = frame.extent;
        const matter::DlssResources resources{
            {hdr_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, hdr_.format,
             raster_extent_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, hdr_.view, hdr_.memory,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT},
            {depth_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             depth_.format, raster_extent_,
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, depth_.view, depth_.memory,
             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             VK_IMAGE_ASPECT_DEPTH_BIT},
            {velocity_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             velocity_.format, raster_extent_,
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, velocity_.view,
             velocity_.memory,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT},
            {slot.dlss_output.image, VK_IMAGE_LAYOUT_GENERAL,
             slot.dlss_output.format, frame.extent,
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, slot.dlss_output.view,
             slot.dlss_output.memory,
             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT}};
        std::string evaluation_error;
        matter::DlssEvaluationOutput evaluation_output{};
        if (dlss_bridge_->evaluate_dlss(
                frame.command_buffer, temporal_frame_.attempt_token,
                {selected_dlss_mode_, frame.extent, true, true}, constants,
                resources, evaluation_output, evaluation_error)) {
            composite_source = &slot.dlss_output;
            slot.dlss_output.layout = evaluation_output.layout;
            composite_source_stage = evaluation_output.stage;
            composite_source_access = evaluation_output.access;
        } else {
            composite_source_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            composite_source_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            consume_bridge_reset();
        }
    }
    matter::record_image_transition(
        frame.command_buffer, *composite_source,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        composite_source_stage, composite_source_access,
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
    blit.srcOffsets[1] = {
        static_cast<int32_t>(composite_source->extent.width),
        static_cast<int32_t>(composite_source->extent.height), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = {static_cast<int32_t>(frame.extent.width),
                          static_cast<int32_t>(frame.extent.height), 1};
    vkCmdBlitImage(frame.command_buffer, composite_source->image,
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
            {velocity_.image, velocity_.format},
            {depth_.image, depth_.format},
            {hdr_.image, hdr_.format},
            raster_extent_};
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
bool VkSceneRenderer::readback_raster_pixel(uint32_t x, uint32_t y,
                                            VkRasterPixel& pixel,
                                            std::string& error) {
    error.clear();
    pixel = {};
    pixel.depth = 1.0f;
    pixel.visibility = 1.0f;
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
    constexpr VkDeviceSize readback_size = 41;
    if (!matter::create_buffer(
            *vulkan_, readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            staging, error)) {
        return false;
    }
    RasterReadbackRecord record{{&albedo_, &normal_, &orm_, &velocity_, &depth_,
                                 &hdr_, &visibility_},
                                {VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT},
                                staging.buffer,
                                x,
                                y};
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        depth_.lifetime, hdr_.lifetime, visibility_.lifetime,
        staging.lifetime};
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
    uint16_t velocity_half[2]{};
    std::memcpy(velocity_half, bytes.data() + 20, sizeof(velocity_half));
    std::memcpy(&pixel.depth, bytes.data() + 24, sizeof(pixel.depth));
    std::memcpy(hdr_half, bytes.data() + 32, sizeof(hdr_half));
    pixel.normal = {half_to_float(normal_half[0]),
                    half_to_float(normal_half[1]),
                    half_to_float(normal_half[2]),
                    half_to_float(normal_half[3])};
    pixel.velocity = {half_to_float(velocity_half[0]),
                      half_to_float(velocity_half[1]), 0.0f};
    pixel.hdr = {half_to_float(hdr_half[0]), half_to_float(hdr_half[1]),
                 half_to_float(hdr_half[2]), half_to_float(hdr_half[3])};
    pixel.visibility = bytes[40] / 255.0f;
    return true;
}

#endif

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
        clusters_.reset();
        vertices_.reset();
        albedo_.reset();
        normal_.reset();
        orm_.reset();
        velocity_.reset();
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
    part_instance_counts_.clear();
    command_template_.clear();
    part_command_ranges_.clear();
    recorded_draw_ranges_.clear();
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
    cached_stats_ = {};
    for (FrameResources& frame : frames_) frame.stats_valid = false;
    raster_attachments_ready_ = false;
    ++static_generation_;
    ++instance_generation_;
    static_upload_dirty_ = true;
    std::string ignored_error;
    if (rebuild_command_template(ignored_error)) note_command_layout_rebuild();
    poison_reason_.clear();
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    test_fail_after_replacements_ = std::numeric_limits<uint32_t>::max();
    test_fail_after_uploads_ = std::numeric_limits<uint32_t>::max();
    test_fail_after_frame_resource_allocations_ =
        std::numeric_limits<uint32_t>::max();
#endif
}

}  // namespace viewer
