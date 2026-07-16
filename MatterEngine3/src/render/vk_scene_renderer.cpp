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
static_assert(sizeof(VkCullStats) == 24,
              "VkCullStats must match the std430 stats block");
static_assert(sizeof(VkRasterVertex) == 72,
              "VkRasterVertex must match raster vertex bindings");

struct GpuRtCounters {
    uint32_t invalid_part_records;
    uint32_t any_hit_invocations;
    uint32_t any_hit_layers;
    uint32_t capped_rays;
};
static_assert(sizeof(GpuRtCounters) == 16);

struct alignas(16) VulkanGiAtrousConstants {
    uint32_t extent[2];
    uint32_t step_width;
    uint32_t signal_mode;
    uint32_t kernel_radius;
    float phi_luminance;
    float phi_depth;
    float normal_power;
    uint32_t pass_index;
    uint32_t pad[3];
};
static_assert(sizeof(VulkanGiAtrousConstants) == 48);

// Named constants for values that appear as literals in this file.
// kGiTestTemporalToken / kGiTestAtrousToken are sentinel serials injected by
// test-fixture helpers so the GI temporal and A-trous shaders can be exercised
// without a real swapchain frame.  The values are ASCII "TEMP" and "ATRO"
// respectively — chosen to be visually distinctive in a debugger.  They have
// no shader-side counterpart and do not need to match any GLSL literal.
constexpr uint32_t kGiTestTemporalToken = 0x54454d50u;  // ASCII "TEMP"
constexpr uint32_t kGiTestAtrousToken   = 0x4154524fu;  // ASCII "ATRO"
// GI temporal history length is stored as uint16_t; this cap prevents
// truncation when the CPU-side counter (uint32_t) exceeds the field width.
constexpr uint32_t kGiHistoryLengthMax  = 65535u;       // UINT16_MAX
// Vulkan instanceCustomIndex is a 24-bit field in the TLAS instance record;
// part_records must stay below this limit.  Must match the Vulkan spec
// (VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits).
constexpr uint32_t kTlasCustomIndexMax  = 1u << 24;

bool rt_material_is_opaque(const MaterialGpuRecord& material) {
    // Thin-walled scatterers must sit in the non-opaque TLAS layer even
    // though they author shadowOpacity = 1.0: a backlit blob's sun ray
    // starts inside its own geometry, and the opaque layer would
    // self-shadow it to black instead of attenuating (rt_visibility.rahit).
    const bool thin_scattering =
        (material.flags_misc[0] & MATERIAL_THIN_WALLED) != 0u &&
        material.scattering[3] > 0.0f;
    return material.metal_opacity_spec_coat[1] >= 1.0f &&
           material.scattering_shape[3] >= 1.0f &&
           (material.flags_misc[0] & MATERIAL_ALPHA_TESTED) == 0u &&
           material.transmission[0] <= 0.0f && !thin_scattering;
}

bool rt_material_ids_are_opaque(
    const std::vector<MaterialGpuRecord>& materials,
    const std::vector<uint32_t>& material_ids) {
    return std::all_of(material_ids.begin(), material_ids.end(),
                       [&](uint32_t material_id) {
                           return material_id < materials.size() &&
                                  rt_material_is_opaque(
                                      materials[material_id]);
                       });
}

bool fail_vk(const char* operation, VkResult result, std::string& error) {
    error = std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

// Write a single timestamp query. Called from VkSceneRenderer methods below.
// Query index = zone_id * 2 + (is_end ? 1 : 0).
// Both begin and end use ALL_COMMANDS (drain) semantics: each stamp latches
// when all prior GPU work completes, so a zone interval is drain-to-drain —
// the zone's incremental wall-clock cost. With TOP_OF_PIPE begins, overlapping
// passes each report the same wide window (begin latches at command parse,
// end waits for full drain), double-counting shared time and summing past the
// frame total.
inline void write_ts(VkCommandBuffer cmd, VkQueryPool pool,
                     uint32_t zone_id, bool is_end) {
    const uint32_t query = zone_id * 2u + (is_end ? 1u : 0u);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool,
                         query);
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

void clear_color_image_for_use(VkCommandBuffer command_buffer,
                               matter::VkImageResource& image,
                               const VkClearColorValue& value,
                               VkImageLayout next_layout,
                               VkPipelineStageFlags2 next_stage,
                               VkAccessFlags2 next_access) {
    transition_for_use(command_buffer, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command_buffer, image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                         &range);
    matter::record_image_transition(command_buffer, image, next_layout,
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, next_stage,
                                    next_access, VK_IMAGE_ASPECT_COLOR_BIT);
}

VkPipelineStageFlags2 gbuffer_sampled_stages(
    uint32_t attachment_index, bool native_ray_tracing_available) {
    VkPipelineStageFlags2 stages = 0;
    if (attachment_index < 4)
        stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (attachment_index == 1 || attachment_index == 3 ||
        attachment_index == 4)
        stages |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (native_ray_tracing_available && attachment_index < 3)
        stages |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    return stages;
}

struct CullDispatchRecord {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t group_count;
    VkBuffer material_upload = VK_NULL_HANDLE;
    VkBuffer materials = VK_NULL_HANDLE;
    VkDeviceSize material_bytes = 0;
    uint64_t* material_upload_record_count = nullptr;
};

void record_material_upload_commands(VkCommandBuffer command_buffer,
                                     VkBuffer source, VkBuffer destination,
                                     VkDeviceSize size) {
    if (size == 0) return;
    const VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(command_buffer, source, destination, 1, &copy);

    VkBufferMemoryBarrier2 barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination;
    barrier.offset = 0;
    barrier.size = size;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

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
    record_material_upload_commands(
        command_buffer, dispatch.material_upload, dispatch.materials,
        dispatch.material_bytes);
    if (dispatch.material_bytes != 0 &&
        dispatch.material_upload_record_count != nullptr) {
        ++*dispatch.material_upload_record_count;
    }
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
    matter::VkImageResource* material_instance;
    matter::VkImageResource* depth;
    matter::VkImageResource* hdr;
    matter::VkImageResource* visibility;
    matter::VkImageResource* raw_diffuse;
    matter::VkImageResource* raw_specular;
    matter::VkImageResource* raw_transmission;
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
    matter::Float3 camera_eye;
    float pixel_budget;
    std::string* error;
    bool* ray_trace_ok;
    // GPU timestamp pool + written-bits for the GBuffer zone.
    // Null when timers are disabled or pool is unavailable.
    VkQueryPool ts_pool = VK_NULL_HANDLE;
    uint8_t* ts_written = nullptr;
    uint32_t gbuffer_zone = 0;
};

void record_raster(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterRecord*>(user_data);
    matter::VkImageResource* colors[] = {record.albedo, record.normal,
                                         record.orm, record.velocity,
                                         record.material_instance};
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

    // A negative height preserves the engine's top-left framebuffer
    // convention without flipping the canonical Vulkan-ZO projection.
    const VkViewport raster_viewport{
        0.0f, static_cast<float>(record.extent.height),
        static_cast<float>(record.extent.width),
        -static_cast<float>(record.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, record.extent};
    const VkDeviceSize vertex_offset = 0;

    // --- GBuffer pass: 5-color MRT + depth write ---
    const VkClearValue clear_color{{{0.0f, 0.0f, 0.0f, 0.0f}}};
    VkRenderingAttachmentInfo color_attachments[5]{};
    for (size_t i = 0; i < 5; ++i) {
        color_attachments[i].sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[i].imageView = colors[i]->view;
        color_attachments[i].imageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachments[i].clearValue = clear_color;
    }
    color_attachments[4].clearValue.color.uint32[0] = UINT32_MAX;
    color_attachments[4].clearValue.color.uint32[1] = UINT32_MAX;
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
    rendering.colorAttachmentCount = 5;
    rendering.pColorAttachments = color_attachments;
    rendering.pDepthAttachment = &depth_attachment;
    if (record.ts_pool != VK_NULL_HANDLE && record.ts_written) {
        write_ts(command_buffer, record.ts_pool, record.gbuffer_zone, false);
        record.ts_written[record.gbuffer_zone] |= 1u;
    }
    vkCmdBeginRendering(command_buffer, &rendering);
    vkCmdSetViewport(command_buffer, 0, 1, &raster_viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      record.raster_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            record.raster_layout, 0, 2,
                            record.raster_sets, 0, nullptr);
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
    if (record.ts_pool != VK_NULL_HANDLE && record.ts_written) {
        write_ts(command_buffer, record.ts_pool, record.gbuffer_zone, true);
        record.ts_written[record.gbuffer_zone] |= 2u;
    }

    for (uint32_t index = 0; index < 5; ++index) {
        auto* color = colors[index];
        const VkPipelineStageFlags2 sampled_stages = gbuffer_sampled_stages(
            index, record.native_ray_tracing_available);
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
            *record.frame, *record.matrices, record.camera_eye,
            record.pixel_budget, record.extent, *record.error);
        if (!*record.ray_trace_ok) return;
    } else {
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        clear_color_image_for_use(command_buffer, *record.visibility, one,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        for (auto* signal : {record.raw_diffuse, record.raw_specular,
                             record.raw_transmission})
            clear_color_image_for_use(
                command_buffer, *signal, zero,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
    matter::VkImageResource* images[12];
    VkImageAspectFlags aspects[12];
    VkBuffer destination;
    uint32_t x;
    uint32_t y;
    uint32_t raw_x;
    uint32_t raw_y;
};

void record_raster_readback(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterReadbackRecord*>(user_data);
    // Each offset is aligned to its format's texel-block size (4 or 8 bytes).
    constexpr VkDeviceSize offsets[12] = {0, 8, 16, 20, 24, 32,
                                          40, 48, 56, 64, 72, 80};
    for (size_t i = 0; i < 12; ++i) {
        transition_for_use(command_buffer, *record.images[i],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT, record.aspects[i]);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offsets[i];
        copy.imageSubresource.aspectMask = record.aspects[i];
        copy.imageSubresource.layerCount = 1;
        const uint32_t copy_x = i >= 8 ? record.raw_x : record.x;
        const uint32_t copy_y = i >= 8 ? record.raw_y : record.y;
        copy.imageOffset = {static_cast<int32_t>(copy_x),
                            static_cast<int32_t>(copy_y), 0};
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

uint16_t float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000u) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
    }
    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);
    mantissa += 0x1000u;
    if (mantissa & 0x800000u) {
        mantissa = 0;
        if (++exponent >= 31)
            return static_cast<uint16_t>(sign | 0x7c00u);
    }
    return static_cast<uint16_t>(sign |
        (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}
#endif

}  // namespace

namespace vk_scene_detail {

uint32_t select_scene_cluster_lod(const VkSceneCluster& cluster,
                                  const matter::Mat4f& object_to_world,
                                  matter::Float3 camera_eye,
                                  float pixel_budget) noexcept {
    if (cluster.lods.empty()) return 0;
    std::array<float, kVkMaxLod> thresholds{};
    for (uint32_t i = 0; i < cluster.lods.size(); ++i)
        thresholds[i] = cluster.lods[i].threshold;
    return select_cluster_lod_view(
        cluster.aabb_min, cluster.aabb_max, cluster.radius,
        thresholds.data(), static_cast<uint32_t>(cluster.lods.size()),
        object_to_world, camera_eye, pixel_budget);
}

uint32_t select_cluster_lod_view(const matter::Float3& aabb_min,
                                 const matter::Float3& aabb_max,
                                 float radius, const float* thresholds,
                                 uint32_t lod_count,
                                 const matter::Mat4f& object_to_world,
                                 matter::Float3 camera_eye,
                                 float pixel_budget) noexcept {
    if (lod_count == 0 || thresholds == nullptr) return 0;
    const matter::Float3 x_basis{object_to_world.m[0], object_to_world.m[4],
                                 object_to_world.m[8]};
    const matter::Float3 y_basis{object_to_world.m[1], object_to_world.m[5],
                                 object_to_world.m[9]};
    const matter::Float3 z_basis{object_to_world.m[2], object_to_world.m[6],
                                 object_to_world.m[10]};
    const auto length = [](matter::Float3 value) {
        return std::sqrt(value.x * value.x + value.y * value.y +
                         value.z * value.z);
    };
    const float scale =
        (length(x_basis) + length(y_basis) + length(z_basis)) / 3.0f;
    const matter::Float3 local_center{
        (aabb_min.x + aabb_max.x) * 0.5f,
        (aabb_min.y + aabb_max.y) * 0.5f,
        (aabb_min.z + aabb_max.z) * 0.5f};
    const matter::Float3 world_center =
        transform_point(object_to_world, local_center);
    const float dx = world_center.x - camera_eye.x;
    const float dy = world_center.y - camera_eye.y;
    const float dz = world_center.z - camera_eye.z;
    const float distance =
        std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.01f);
    const float projected_size =
        radius * scale / distance * pixel_budget;
    uint32_t selected = lod_count - 1;
    for (uint32_t lod = 0; lod < lod_count; ++lod) {
        if (projected_size >= thresholds[lod]) {
            selected = lod;
            break;
        }
    }
    return selected;
}

std::vector<uint32_t> dense_rt_lod_offsets(const VkScenePart& part) {
    std::vector<uint32_t> offsets;
    offsets.reserve(part.clusters.size() + 1);
    uint32_t total = 0;
    offsets.push_back(total);
    for (const VkSceneCluster& cluster : part.clusters) {
        total += static_cast<uint32_t>(cluster.lods.size());
        offsets.push_back(total);
    }
    return offsets;
}

bool dense_rt_lod_index(const std::vector<uint32_t>& offsets,
                        uint32_t cluster_index, uint32_t lod_index,
                        uint32_t& record_index) noexcept {
    if (cluster_index + 1 >= offsets.size()) return false;
    const uint32_t begin = offsets[cluster_index];
    const uint32_t end = offsets[cluster_index + 1];
    if (lod_index >= end - begin) return false;
    record_index = begin + lod_index;
    return true;
}

std::vector<RtGeometrySelection> select_rt_instance_geometry(
    const VkScenePart& part, const matter::Mat4f& object_to_world,
    matter::Float3 camera_eye, float pixel_budget) {
    std::vector<RtGeometrySelection> result;
    result.reserve(part.clusters.size());
    for (uint32_t cluster_index = 0; cluster_index < part.clusters.size();
         ++cluster_index) {
        const VkSceneCluster& cluster = part.clusters[cluster_index];
        if (cluster.lods.empty()) continue;
        const uint32_t lod_index = select_scene_cluster_lod(
            cluster, object_to_world, camera_eye, pixel_budget);
        const VkSceneLod& lod = cluster.lods[lod_index];
        result.push_back(
            {cluster_index, lod_index, lod.first_index, lod.index_count});
    }
    return result;
}

VkShaderStageFlags scene_binding_stage_flags(uint32_t binding) noexcept {
    if (binding == 5)
        return VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    return VK_SHADER_STAGE_COMPUTE_BIT |
           (binding == 3 ? VK_SHADER_STAGE_VERTEX_BIT : 0);
}

bool scene_storage_limits_supported(uint32_t max_per_stage,
                                    uint32_t max_per_set) noexcept {
    return max_per_stage >= 5 && max_per_set >= 6;
}

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

VkPipelineStageFlags2 gbuffer_sampled_stages_for_test(
    uint32_t attachment_index, bool native_ray_tracing_available) noexcept {
    return gbuffer_sampled_stages(attachment_index,
                                  native_ray_tracing_available);
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

void VkSceneRenderer::set_dlss_mode(matter::DlssMode mode) {
    if (selected_dlss_mode_ == mode) return;
    selected_dlss_mode_ = mode;
    gi_history_reset_pending_ = true;
}

void VkSceneRenderer::set_ray_tracing_settings(
    const matter::VulkanRayTracingSettings& settings) {
    if (ray_tracing_settings_.enabled != settings.enabled)
        gi_history_reset_pending_ = true;
    ray_tracing_settings_ = settings;
    ray_tracing_settings_.samples =
        std::max(1u, std::min(settings.samples, 16u));
}

bool VkSceneRenderer::consume_dlss_history_reset() {
    const bool pending = dlss_history_reset_pending_;
    dlss_history_reset_pending_ = false;
    // A mode switch that changed the internal extent has already reset the
    // current temporal candidate. Do not invalidate TemporalState again and
    // manufacture a second reset on the next presented frame.
    return pending && !gi_candidate_was_reset_;
}

VkSceneRenderer::~VkSceneRenderer() {
    if (vulkan_) {
        vulkan_->wait_idle();
        std::string ignored_error;
        (void)dlss_bridge_->free_dlss_resources(ignored_error);
    }
    destroy_pipeline();
}

void VkSceneRenderer::destroy_pipeline() {
    if (!vulkan_) return;
    const VkDevice device = vulkan_->device();
    rt_sbt_.reset();
    visibility_.reset();
    raw_diffuse_.reset();
    raw_specular_.reset();
    raw_specular_aux_.reset();
    raw_transmission_.reset();
    for (auto& image : gi_atrous_) image.reset();
    for (auto& image : gi_spec_atrous_) image.reset();
    for (auto* histories : {&gi_history_, &gi_spec_history_}) {
        for (auto& history : *histories) {
            history.radiance.reset();
            history.moments.reset();
            history.history_length.reset();
            history.depth.reset();
            history.normal.reset();
            history.identity.reset();
            history.rejection.reset();
            history.aux.reset();
        }
    }
    if (gi_temporal_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, gi_temporal_pipeline_, nullptr);
    if (gi_temporal_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, gi_temporal_pipeline_layout_, nullptr);
    if (gi_temporal_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, gi_temporal_set_layout_, nullptr);
    if (gi_atrous_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, gi_atrous_pipeline_, nullptr);
    if (gi_atrous_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, gi_atrous_pipeline_layout_, nullptr);
    if (gi_atrous_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, gi_atrous_set_layout_, nullptr);
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
    if (display_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, display_pipeline_, nullptr);
    if (display_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, display_pipeline_layout_, nullptr);
    if (display_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, display_set_layout_, nullptr);
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
    display_set_layout_ = VK_NULL_HANDLE;
    display_pipeline_layout_ = VK_NULL_HANDLE;
    display_pipeline_ = VK_NULL_HANDLE;
    display_pipeline_format_ = VK_FORMAT_UNDEFINED;
    gi_temporal_pipeline_ = VK_NULL_HANDLE;
    gi_temporal_pipeline_layout_ = VK_NULL_HANDLE;
    gi_temporal_set_layout_ = VK_NULL_HANDLE;
    gi_atrous_pipeline_ = VK_NULL_HANDLE;
    gi_atrous_pipeline_layout_ = VK_NULL_HANDLE;
    gi_atrous_set_layout_ = VK_NULL_HANDLE;
    rt_pipeline_ = VK_NULL_HANDLE;
    rt_pipeline_layout_ = VK_NULL_HANDLE;
    rt_descriptor_pool_ = VK_NULL_HANDLE;
    rt_set_layout_ = VK_NULL_HANDLE;
    rt_descriptor_sets_.clear();
    rt_sbt_address_ = 0;
    rt_sbt_test_raygen_address_ = 0;
    rt_sbt_lighting_raygen_address_ = 0;
    rt_sbt_miss_address_ = 0;
    rt_sbt_hit_address_ = 0;
    rt_sbt_stride_ = 0;
    rt_sbt_miss_size_ = 0;
    rt_sbt_hit_size_ = 0;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    for (auto& f : frames_) {
        if (f.ts_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, f.ts_pool, nullptr);
            f.ts_pool = VK_NULL_HANDLE;
        }
    }
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

    std::array<VkDescriptorSetLayoutBinding, 6> scene_bindings{};
    for (uint32_t i = 0; i < scene_bindings.size(); ++i)
        scene_bindings[i] =
            descriptor_binding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               vk_scene_detail::scene_binding_stage_flags(i));
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

    if (!create_raster_pipelines(error) || !create_display_pipeline(error) ||
        !create_gi_temporal_pipeline(error) ||
        !create_gi_atrous_pipeline(error))
        return false;
    return !vulkan_->ray_tracing_available() ||
           create_ray_tracing_pipeline(error);
}

bool VkSceneRenderer::create_gi_temporal_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    std::array<VkDescriptorSetLayoutBinding, 21> bindings{};
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
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &gi_temporal_pipeline_);
    vkDestroyShaderModule(device, shader, nullptr);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateComputePipelines(GI temporal)", result, error);
}

bool VkSceneRenderer::create_gi_atrous_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
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
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &gi_atrous_pipeline_);
    vkDestroyShaderModule(device, shader, nullptr);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateComputePipelines(GI A-trous)", result, error);
}

bool VkSceneRenderer::create_ray_tracing_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    const VkDescriptorSetLayoutBinding bindings[] = {
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
        descriptor_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
        descriptor_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
        descriptor_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR)};
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 15;
    set_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                                   &rt_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(ray tracing)", result,
                       error);
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    push.size = 144;
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
    const char* names[] = {"rt_shadow.rgen.spv", "rt_surface_test.rgen.spv",
                           "rt_lighting.rgen.spv",
                           "rt_visibility.rmiss.spv", "rt_radiance.rmiss.spv",
                           "rt_visibility.rchit.spv",
                           "rt_visibility.rahit.spv",
                           "rt_surface.rchit.spv"};
    const VkShaderStageFlagBits stages_bits[] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    VkShaderModule modules[8]{};
    VkPipelineShaderStageCreateInfo stages[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
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
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const uint32_t count_lobe_samples = 1u;
#else
    const uint32_t count_lobe_samples = 0u;
#endif
    const VkSpecializationMapEntry lobe_count_entry{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo lighting_specialization{
        1, &lobe_count_entry, sizeof(uint32_t), &count_lobe_samples};
    stages[2].pSpecializationInfo = &lighting_specialization;
    VkRayTracingShaderGroupCreateInfoKHR groups[7]{};
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
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 3;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[4].generalShader = 4;
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[5].closestHitShader = 5;
    groups[5].anyHitShader = 6;
    groups[6].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[6].closestHitShader = 7;
    VkRayTracingPipelineCreateInfoKHR create{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    create.stageCount = 8;
    create.pStages = stages;
    create.groupCount = 7;
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
    std::vector<uint8_t> handles(static_cast<size_t>(7 * handle_size));
    result = get_handles(device, rt_pipeline_, 0, 7, handles.size(),
                         handles.data());
    if (result != VK_SUCCESS)
        return fail_vk("vkGetRayTracingShaderGroupHandlesKHR", result, error);
    const VkDeviceSize category_size = 2 * handle_stride;
    const VkDeviceSize category_span =
        (category_size + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    const VkDeviceSize raygen_record_stride =
        (handle_stride + props.shader_group_base_alignment - 1) /
        props.shader_group_base_alignment * props.shader_group_base_alignment;
    const VkDeviceSize raygen_span = 3 * raygen_record_stride;
    if (!matter::create_buffer(
            *vulkan_, raygen_span + 2 * category_span +
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
    rt_sbt_test_raygen_address_ = rt_sbt_address_ + raygen_record_stride;
    rt_sbt_lighting_raygen_address_ = rt_sbt_address_ + 2 * raygen_record_stride;
    rt_sbt_miss_address_ = rt_sbt_address_ + raygen_span;
    rt_sbt_hit_address_ = rt_sbt_miss_address_ + category_span;
    rt_sbt_miss_size_ = category_size;
    rt_sbt_hit_size_ = category_size;
    const VkDeviceSize mapped_offset = rt_sbt_address_ - rt_sbt_.address;
    std::memset(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset, 0,
                static_cast<size_t>(raygen_span + 2 * category_span));
    for (uint32_t i = 0; i < 3; ++i) {
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        i * raygen_record_stride,
                    handles.data() + i * handle_size,
                    static_cast<size_t>(handle_size));
    }
    for (uint32_t i = 0; i < 2; ++i) {
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        raygen_span + i * handle_stride,
                    handles.data() + (3 + i) * handle_size,
                    static_cast<size_t>(handle_size));
        std::memcpy(static_cast<uint8_t*>(rt_sbt_.mapped) + mapped_offset +
                        raygen_span + category_span + i * handle_stride,
                    handles.data() + (5 + i) * handle_size,
                    static_cast<size_t>(handle_size));
    }
    return matter::flush_buffer(rt_sbt_, mapped_offset,
                                raygen_span + 2 * category_span, error);
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
         static_cast<uint32_t>(offsetof(VkRasterVertex, tint))},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, surface))},
        {4, 0, VK_FORMAT_R32_UINT,
         static_cast<uint32_t>(offsetof(VkRasterVertex, material_index))}};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 5;
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
    const VkDynamicState dynamic_values[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_values;

    // GBuffer pipeline: 5-color MRT + depth write.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable  = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend_attachments[5]{};
    for (auto& blend : blend_attachments) {
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 5;
    color_blend.pAttachments = blend_attachments;
    const VkFormat gbuffer_formats[] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R32G32_UINT};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 5;
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

    std::array<VkDescriptorSetLayoutBinding, 9> sampled_bindings{};
    for (uint32_t i = 0; i < 7; ++i) {
        sampled_bindings[i] = descriptor_binding(
            i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    sampled_bindings[7] = descriptor_binding(
        7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    sampled_bindings[8] = descriptor_binding(
        8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_FRAGMENT_BIT);
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

bool VkSceneRenderer::create_display_pipeline(std::string& error) {
    const VkDevice device = vulkan_->device();
    const VkDescriptorSetLayoutBinding binding = descriptor_binding(
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayoutCreateInfo set_create{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_create.bindingCount = 1;
    set_create.pBindings = &binding;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &set_create, nullptr, &display_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(display)", result, error);

    VkPushConstantRange exposure_range{};
    exposure_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    exposure_range.size = sizeof(float);
    VkPipelineLayoutCreateInfo layout_create{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_create.setLayoutCount = 1;
    layout_create.pSetLayouts = &display_set_layout_;
    layout_create.pushConstantRangeCount = 1;
    layout_create.pPushConstantRanges = &exposure_range;
    result = vkCreatePipelineLayout(device, &layout_create, nullptr,
                                    &display_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(display)", result, error);

    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    if (!create_shader_module(device, "composite.vert.spv", vertex, error) ||
        !create_shader_module(device, "display_transform.frag.spv", fragment,
                              error)) {
        if (vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, vertex, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                           VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    display_pipeline_format_ = vulkan_->swapchain_format();
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &display_pipeline_format_;
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
    create.pColorBlendState = &color_blend;
    create.pDynamicState = &dynamic;
    create.layout = display_pipeline_layout_;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create,
                                       nullptr, &display_pipeline_);
    vkDestroyShaderModule(device, fragment, nullptr);
    vkDestroyShaderModule(device, vertex, nullptr);
    return result == VK_SUCCESS ||
           fail_vk("vkCreateGraphicsPipelines(display)", result, error);
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
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_slot_count * 13},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         frame_slot_count * 77},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 22}};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = frame_slot_count * 12;
    pool.poolSizeCount = 4;
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
    layouts.reserve(frame_slot_count * 12);
    for (size_t index = 0; index < frame_slot_count; ++index) {
        layouts.push_back(set_layouts_[0]);
        layouts.push_back(set_layouts_[1]);
        layouts.push_back(composite_set_layout_);
        layouts.push_back(display_set_layout_);
        layouts.push_back(gi_temporal_set_layout_);
        layouts.push_back(gi_temporal_set_layout_);
        for (uint32_t i = 0; i < 6; ++i)
            layouts.push_back(gi_atrous_set_layout_);
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
        frame.descriptor_sets[0] = sets[index * 12];
        frame.descriptor_sets[1] = sets[index * 12 + 1];
        frame.composite_descriptor_set = sets[index * 12 + 2];
        frame.display_descriptor_set = sets[index * 12 + 3];
        frame.gi_temporal_descriptor_sets[0] = sets[index * 12 + 4];
        frame.gi_temporal_descriptor_sets[1] = sets[index * 12 + 5];
        for (uint32_t i = 0; i < 6; ++i)
            frame.gi_atrous_descriptor_sets[i] = sets[index * 12 + 6 + i];
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
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !matter::create_buffer(
                *vulkan_, sizeof(MaterialGpuRecord),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                frame.material_upload, error) ||
            !matter::create_buffer(
                *vulkan_, sizeof(MaterialGpuRecord),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, frame.materials,
                error) ||
            !ensure_candidate_buffer(frame.rt_parts, sizeof(GpuRtPartRecord),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.rt_error_counter,
                                     sizeof(GpuRtCounters),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.rt_test_output, 20 * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.gi_atrous_markers,
                                     5 * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
            vkDestroyDescriptorPool(vulkan_->device(), next_pool, nullptr);
            return false;
        }
        update_frame_descriptors(frame);
        if (gpu_timers_supported_) {
            VkQueryPoolCreateInfo ts_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            ts_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            ts_info.queryCount = kGpuZoneCount * 2u;
            const VkResult ts_result = vkCreateQueryPool(
                vulkan_->device(), &ts_info, nullptr, &frame.ts_pool);
            if (ts_result != VK_SUCCESS) {
                // Soft-fail: disable timers rather than failing the whole init.
                gpu_timers_supported_ = false;
            }
        }
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vulkan_->wait_idle();
        vkDestroyDescriptorPool(vulkan_->device(), descriptor_pool_, nullptr);
    }
    for (auto& f : frames_) {
        if (f.ts_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(vulkan_->device(), f.ts_pool, nullptr);
            f.ts_pool = VK_NULL_HANDLE;
        }
    }
    frames_ = std::move(next_frames);
    descriptor_pool_ = next_pool;
    if (vulkan_->ray_tracing_available()) {
        if (rt_descriptor_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(vulkan_->device(), rt_descriptor_pool_,
                                    nullptr);
        const VkDescriptorPoolSize rt_sizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, frame_slot_count},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frame_slot_count * 5},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 5},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_slot_count * 4}};
        VkDescriptorPoolCreateInfo rt_pool{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        rt_pool.maxSets = frame_slot_count;
        rt_pool.poolSizeCount = 4;
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
    update_descriptor(frame.descriptor_sets[1], 5,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.materials);
}

void VkSceneRenderer::update_composite_descriptor(FrameResources& frame) {
    matter::VkImageResource* diffuse =
        gi_settings_.enabled && gi_candidate_frame_serial_ != 0
            ? (gi_filtered_valid_
                   ? &gi_atrous_[gi_filtered_index_]
                   : &gi_history_[gi_composite_history_index_].radiance)
            : &raw_diffuse_;
    last_composite_used_gi_temporal_ = diffuse != &raw_diffuse_;
    matter::VkImageResource* specular =
        gi_settings_.enabled && gi_candidate_frame_serial_ != 0
            ? (gi_filtered_valid_ ? &gi_spec_atrous_[gi_filtered_index_]
                                  : &gi_spec_history_[gi_composite_history_index_].radiance)
            : &raw_specular_;
    matter::VkImageResource* sampled[] = {&albedo_, &normal_, &orm_,
                                          &visibility_, diffuse, specular,
                                          &material_instance_,
                                          &raw_transmission_};
    const uint32_t sampled_slots[] = {0, 1, 2, 3, 4, 5, 6, 8};
    VkDescriptorImageInfo image_infos[8]{};
    VkWriteDescriptorSet writes[9]{};
    for (uint32_t i = 0; i < 8; ++i) {
        image_infos[i].sampler = composite_sampler_;
        image_infos[i].imageView = sampled[i]->view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frame.composite_descriptor_set;
        writes[i].dstBinding = sampled_slots[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }
    VkDescriptorBufferInfo material_info{frame.materials.buffer, 0,
                                         frame.materials.size};
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = frame.composite_descriptor_set;
    writes[8].dstBinding = 7;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &material_info;
    vkUpdateDescriptorSets(vulkan_->device(), 9, writes, 0, nullptr);
}

void VkSceneRenderer::update_display_descriptor(VkDescriptorSet set,
                                                VkImageView view) {
    VkDescriptorImageInfo image_info{};
    image_info.sampler = composite_sampler_;
    image_info.imageView = view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(vulkan_->device(), 1, &write, 0, nullptr);
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
        !vk_scene_detail::scene_storage_limits_supported(
            vk_limits.maxPerStageDescriptorStorageBuffers,
            vk_limits.maxDescriptorSetStorageBuffers)) {
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
    // Cache timestamp support from device properties.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vulkan_->physical_device(), &props);
        const bool has_ts = props.limits.timestampComputeAndGraphics &&
                            props.limits.timestampPeriod > 0.0f;
        gpu_timers_supported_ = has_ts;
        timestamp_period_ns_ = has_ts ? props.limits.timestampPeriod : 0.0f;
    }
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
        material_instance_.reset();
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
        if (!part.indices.empty()) {
            for (const auto& lod : cluster.lods) {
                if (lod.first_index > part.indices.size() ||
                    lod.index_count >
                        part.indices.size() - lod.first_index) {
                    error = "VkSceneCluster LOD exceeds part-local indices";
                    return -1;
                }
                if (lod.index_count % 3 != 0) {
                    error = "VkSceneCluster LOD index_count must be a multiple of 3";
                    return -1;
                }
            }
        }
    }
    // Validate that all index values are in-range for the vertex array (one pass).
    if (!part.indices.empty() && !part.vertices.empty()) {
        for (uint32_t idx : part.indices) {
            if (idx >= part.vertices.size()) {
                error = "VkScenePart index out of range for vertex array";
                return -1;
            }
        }
    }
    std::shared_ptr<matter::VkBufferResource> rt_geometry;
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
    }
    const uint32_t vertex_base =
        static_cast<uint32_t>(vertex_staging_.size());
    vertex_staging_.insert(vertex_staging_.end(), part.vertices.begin(),
                           part.vertices.end());
    const uint32_t index_base =
        static_cast<uint32_t>(index_staging_.size());
    index_staging_.insert(index_staging_.end(), part.indices.begin(),
                          part.indices.end());
    const int slot = static_cast<int>(parts_.size());
    PartRecord record{};
    record.hash = part.part_hash;
    record.cluster_start = static_cast<uint32_t>(cluster_staging_.size());
    record.cluster_count = static_cast<uint32_t>(part.clusters.size());
    record.vertex_start = vertex_base;   // kept for Task 4 vertexOffset
    record.vertex_count = static_cast<uint32_t>(part.vertices.size());
    record.index_start = index_base;
    record.index_count = static_cast<uint32_t>(part.indices.size());
    record.live = true;
    record.rt_geometry = std::move(rt_geometry);
    record.rt_cluster_lod_offsets =
        vk_scene_detail::dense_rt_lod_offsets(part);
    for (uint32_t cluster_index = 0; cluster_index < part.clusters.size();
         ++cluster_index) {
        const auto& cluster = part.clusters[cluster_index];
        for (uint32_t lod_index = 0; lod_index < cluster.lods.size();
             ++lod_index) {
            const auto& lod = cluster.lods[lod_index];
            RtLodRecord rt_lod{};
            rt_lod.cluster_index = cluster_index;
            rt_lod.lod_index = lod_index;
            rt_lod.first_index = index_base + lod.first_index;
            rt_lod.index_count = lod.index_count;
            rt_lod.primitive_count = lod.index_count / 3;
            if (!part.indices.empty() && !part.vertices.empty()) {
                for (uint32_t k = 0; k < lod.index_count; ++k) {
                    const uint32_t material =
                        part.vertices[part.indices[lod.first_index + k]]
                            .material_index;
                    if (material != UINT32_MAX)
                        rt_lod.material_ids.push_back(material);
                }
            }
            std::sort(rt_lod.material_ids.begin(),
                      rt_lod.material_ids.end());
            rt_lod.material_ids.erase(
                std::unique(rt_lod.material_ids.begin(),
                            rt_lod.material_ids.end()),
                rt_lod.material_ids.end());
            record.rt_lods.push_back(std::move(rt_lod));
        }
    }
    record.material_ids.reserve(part.vertices.size());
    for (const VkRasterVertex& vertex : part.vertices) {
        if (vertex.material_index != UINT32_MAX)
            record.material_ids.push_back(vertex.material_index);
    }
    std::sort(record.material_ids.begin(), record.material_ids.end());
    record.material_ids.erase(
        std::unique(record.material_ids.begin(), record.material_ids.end()),
        record.material_ids.end());
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
        if (!part.indices.empty()) {
            // Rebase part-local first_index to global index_staging_ offset.
            // Index VALUES are part-local and are never rewritten here.
            for (auto& lod : lods) lod.first_index += index_base;
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
        index_staging_.resize(index_base);
        std::string ignored_error;
        rebuild_command_template(ignored_error);
        return -1;
    }
    ++static_generation_;
    static_upload_dirty_ = true;
    note_command_layout_rebuild();
    return slot;
}

bool VkSceneRenderer::update_materials(
    const std::vector<MaterialGpuRecord>& records, uint64_t shading_revision,
    uint64_t geometry_revision, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (records.empty()) {
        error = "Vulkan material table must contain at least one record";
        return false;
    }
    if (shading_revision < material_shading_revision_ ||
        geometry_revision < material_geometry_revision_) {
        error = "Vulkan material revisions must be monotonic";
        return false;
    }
    const bool first_upload = material_staging_.empty();
    const bool shading_changed =
        shading_revision != material_shading_revision_;
    const bool geometry_changed =
        geometry_revision != material_geometry_revision_;
    const bool data_changed =
        records.size() != material_staging_.size() ||
        (records.size() == material_staging_.size() &&
         std::memcmp(records.data(), material_staging_.data(),
                     records.size() * sizeof(MaterialGpuRecord)) != 0);
    if (!shading_changed && !geometry_changed && !data_changed) return true;
    if (!first_upload && data_changed && !shading_changed && !geometry_changed) {
        error = "Vulkan material data changed without a new revision";
        return false;
    }

    // Recompute rather than only setting this bit. A material can change class
    // and then change back before a replacement BLAS is submitted.
    for (PartRecord& part : parts_) {
        part.rt_geometry_classification_dirty = std::any_of(
            part.rt_lods.begin(), part.rt_lods.end(),
            [&](const RtLodRecord& lod) {
                const bool desired = rt_material_ids_are_opaque(
                    records, lod.material_ids);
                const bool previous_desired = rt_material_ids_are_opaque(
                    material_staging_, lod.material_ids);
                return desired != previous_desired ||
                       (lod.built && lod.geometry_opaque != desired);
            });
    }
    material_staging_ = records;
    material_shading_revision_ = shading_revision;
    material_geometry_revision_ = geometry_revision;
    ++material_generation_;
    if (!first_upload && (shading_changed || geometry_changed))
        gi_history_reset_pending_ = true;
    return true;
}

bool VkSceneRenderer::consume_gi_history_reset() {
    const bool pending = gi_history_reset_pending_;
    gi_history_reset_pending_ = false;
    return pending;
}

bool VkSceneRenderer::rt_geometry_classification_dirty(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    return found != slot_of_.end() &&
           parts_[static_cast<size_t>(found->second)]
               .rt_geometry_classification_dirty;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
VkDeviceAddress VkSceneRenderer::test_rt_geometry_address(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return 0;
    const PartRecord& part = parts_[static_cast<size_t>(found->second)];
    return part.rt_geometry ? part.rt_geometry->address : 0;
}

bool VkSceneRenderer::record_test_surface_ray(
    const matter::VulkanFrame& frame, matter::Float3 origin,
    matter::Float3 direction, uint32_t invalid_part_slot,
    std::string& error) {
    error.clear();
    if (!vulkan_->ray_tracing_available() || rt_pipeline_ == VK_NULL_HANDLE ||
        frame.command_buffer == VK_NULL_HANDLE ||
        frame.frame_slot >= frames_.size() ||
        frame.frame_slot >= rt_descriptor_sets_.size()) {
        error = "test surface ray requires an active native RT frame";
        return false;
    }
    const float direction_length = std::sqrt(
        direction.x * direction.x + direction.y * direction.y +
        direction.z * direction.z);
    if (!(direction_length > 0.0f)) {
        error = "test surface ray direction must be non-zero";
        return false;
    }
    FrameResources& selected = frames_[frame.frame_slot];
    if (invalid_part_slot != UINT32_MAX) {
        if (invalid_part_slot >= parts_.size() ||
            selected.rt_parts.mapped == nullptr) {
            error = "invalid test part-table slot";
            return false;
        }
        auto* records =
            static_cast<GpuRtPartRecord*>(selected.rt_parts.mapped);
        records[invalid_part_slot].valid = 0;
        if (!matter::flush_buffer(
                selected.rt_parts,
                invalid_part_slot * sizeof(GpuRtPartRecord),
                sizeof(GpuRtPartRecord), error)) return false;
    }
    struct alignas(16) SurfaceTestConstants {
        float origin_tmin[4];
        float direction_tmax[4];
    } constants{};
    constants.origin_tmin[0] = origin.x;
    constants.origin_tmin[1] = origin.y;
    constants.origin_tmin[2] = origin.z;
    constants.origin_tmin[3] = 0.001f;
    constants.direction_tmax[0] = direction.x;
    constants.direction_tmax[1] = direction.y;
    constants.direction_tmax[2] = direction.z;
    constants.direction_tmax[3] = 10000.0f;
    transition_for_use(frame.command_buffer, visibility_,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    const VkClearColorValue raw_zero{{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_color_image_for_use(frame.command_buffer, raw_diffuse_, raw_zero,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    for (auto* image : {&raw_specular_, &raw_specular_aux_})
        transition_for_use(frame.command_buffer, *image,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdBindPipeline(frame.command_buffer,
                      VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline_);
    const VkDescriptorSet descriptor_set =
        rt_descriptor_sets_[frame.frame_slot];
    vkCmdBindDescriptorSets(frame.command_buffer,
                            VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            rt_pipeline_layout_, 0, 1, &descriptor_set, 0,
                            nullptr);
    vkCmdPushConstants(frame.command_buffer, rt_pipeline_layout_,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(constants),
                       &constants);
    const auto trace = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        vkGetDeviceProcAddr(vulkan_->device(), "vkCmdTraceRaysKHR"));
    if (!trace) {
        error = "vkCmdTraceRaysKHR unavailable for test surface ray";
        return false;
    }
    const VkStridedDeviceAddressRegionKHR raygen{
        rt_sbt_test_raygen_address_, rt_sbt_stride_, rt_sbt_stride_};
    const VkStridedDeviceAddressRegionKHR miss{
        rt_sbt_miss_address_, rt_sbt_stride_, rt_sbt_miss_size_};
    const VkStridedDeviceAddressRegionKHR hit{
        rt_sbt_hit_address_, rt_sbt_stride_, rt_sbt_hit_size_};
    const VkStridedDeviceAddressRegionKHR callable{};
    trace(frame.command_buffer, &raygen, &miss, &hit, &callable, 1, 1, 1);
    matter::record_image_transition(
        frame.command_buffer, visibility_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    matter::record_image_transition(
        frame.command_buffer, raw_diffuse_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    for (auto* image : {&raw_specular_, &raw_specular_aux_})
        matter::record_image_transition(
            frame.command_buffer, *image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    VkMemoryBarrier2 ray_to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    ray_to_host.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    ray_to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ray_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    ray_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &ray_to_host;
    vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    ++test_surface_trace_dispatches_;
    return true;
}

bool VkSceneRenderer::readback_test_surface_hit(
    uint32_t frame_slot, RtSurfaceHit& hit, uint32_t& invalid_count,
    std::string& error) {
    error.clear();
    hit = {};
    invalid_count = 0;
    if (frame_slot >= frames_.size()) {
        error = "test surface readback frame slot is out of range";
        return false;
    }
    FrameResources& selected = frames_[frame_slot];
    if (!matter::map_buffer(selected.rt_test_output, error) ||
        !matter::map_buffer(selected.rt_error_counter, error) ||
        !matter::invalidate_buffer(selected.rt_test_output, 0,
                                   18 * sizeof(uint32_t), error) ||
        !matter::invalidate_buffer(selected.rt_error_counter, 0,
                                   sizeof(GpuRtCounters), error)) return false;
    const auto* words =
        static_cast<const uint32_t*>(selected.rt_test_output.mapped);
    const auto as_float = [](uint32_t bits) {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    hit.flags = words[0];
    hit.valid = (hit.flags & kRtSurfaceValid) != 0;
    hit.part_slot = words[1];
    hit.primitive = words[2];
    hit.material_index = words[3];
    hit.position = {as_float(words[4]), as_float(words[5]),
                    as_float(words[6])};
    hit.hit_t = as_float(words[7]);
    hit.normal = {as_float(words[8]), as_float(words[9]),
                  as_float(words[10])};
    hit.baked_ao = as_float(words[11]);
    hit.tint = {as_float(words[12]), as_float(words[13]),
                as_float(words[14]), as_float(words[15])};
    hit.uv[0] = as_float(words[16]);
    hit.uv[1] = as_float(words[17]);
    invalid_count =
        *static_cast<const uint32_t*>(selected.rt_error_counter.mapped);
    return true;
}

bool VkSceneRenderer::test_rt_blas_built(uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return false;
    const PartRecord& part = parts_[static_cast<size_t>(found->second)];
    return std::any_of(part.rt_lods.begin(), part.rt_lods.end(),
                       [](const RtLodRecord& lod) { return lod.built; });
}

std::weak_ptr<void> VkSceneRenderer::test_rt_blas_lifetime(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return {};
    const PartRecord& part = parts_[static_cast<size_t>(found->second)];
    const auto built = std::find_if(
        part.rt_lods.begin(), part.rt_lods.end(),
        [](const RtLodRecord& lod) { return lod.blas != nullptr; });
    return built != part.rt_lods.end()
               ? std::weak_ptr<void>(built->blas->lifetime)
               : std::weak_ptr<void>{};
}

bool VkSceneRenderer::readback_rt_trace_counters(
    uint32_t frame_slot, RtTraceCounters& counters, std::string& error) {
    error.clear();
    counters = {};
    if (frame_slot >= frames_.size()) {
        error = "RT counter readback frame slot is out of range";
        return false;
    }
    FrameResources& selected = frames_[frame_slot];
    if (!matter::map_buffer(selected.rt_error_counter, error) ||
        !matter::invalidate_buffer(selected.rt_error_counter, 0,
                                   sizeof(GpuRtCounters), error))
        return false;
    const auto* gpu =
        static_cast<const GpuRtCounters*>(selected.rt_error_counter.mapped);
    counters = {gpu->invalid_part_records, gpu->any_hit_invocations,
                gpu->any_hit_layers, gpu->capped_rays};
    return true;
}

bool VkSceneRenderer::test_readback_reflection_sample_counts(
    uint32_t& base_samples, uint32_t& coat_samples, std::string& error) {
    base_samples = 0;
    coat_samples = 0;
    if (frames_.empty()) {
        error = "reflection sample counters require an initialized frame";
        return false;
    }
    FrameResources& frame = frames_[active_frame_index_];
    if (!matter::map_buffer(frame.rt_test_output, error) ||
        !matter::invalidate_buffer(frame.rt_test_output,
                                   18 * sizeof(uint32_t),
                                   2 * sizeof(uint32_t), error))
        return false;
    const auto* words =
        static_cast<const uint32_t*>(frame.rt_test_output.mapped);
    base_samples = words[18];
    coat_samples = words[19];
    return true;
}

uint64_t VkSceneRenderer::test_rt_blas_candidate_serial(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return 0;
    const auto& lods = parts_[static_cast<size_t>(found->second)].rt_lods;
    const auto candidate = std::find_if(
        lods.begin(), lods.end(),
        [](const RtLodRecord& lod) { return lod.candidate_serial != 0; });
    return candidate == lods.end() ? 0 : candidate->candidate_serial;
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

bool VkSceneRenderer::test_dispatch_gi_temporal_fixture(
    const GiTemporalGpuFixture& fixture, GiTemporalGpuResult& result,
    std::string& error) {
    result = {};
    // Test-only diagnostics may run immediately after an asynchronous smoke
    // frame. Recycle descriptor sets only after that submitted work is done.
    vulkan_->wait_idle();
    if (frames_.empty() || raw_diffuse_.image == VK_NULL_HANDLE ||
        fixture.output_pixel.x < 0 || fixture.output_pixel.y < 0 ||
        fixture.output_pixel.x >= static_cast<int>(raw_diffuse_extent_.width) ||
        fixture.output_pixel.y >= static_cast<int>(raw_diffuse_extent_.height)) {
        error = "GI temporal GPU fixture requires initialized in-bounds targets";
        return false;
    }
    matter::VkBufferResource upload;
    matter::VkBufferResource readback;
    if (!matter::create_buffer(
            *vulkan_, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, upload, error) ||
        !matter::create_buffer(
            *vulkan_, 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            readback, error) ||
        !matter::map_buffer(upload, error))
        return false;
    const uint16_t history_patch = static_cast<uint16_t>(
        std::min(kGiHistoryLengthMax, fixture.previous_history_length));
    std::memcpy(upload.mapped, &history_patch, sizeof(history_patch));
    if (!matter::flush_buffer(upload, 0, 4, error)) return false;

    struct FixtureRecord {
        VkSceneRenderer* renderer;
        const GiTemporalGpuFixture* fixture;
        VkBuffer upload;
        VkBuffer readback;
        bool ok = true;
        std::string* error;
    } record{this, &fixture, upload.buffer, readback.buffer, true, &error};
    const uint64_t saved_presented_token = gi_presented_attempt_token_;
    const uint64_t saved_candidate_serial = gi_candidate_frame_serial_;
    const uint64_t saved_candidate_token = gi_candidate_attempt_token_;
    const bool saved_candidate_reset = gi_candidate_was_reset_;
    const bool saved_composite = last_composite_used_gi_temporal_;
    const TemporalFrame saved_temporal = temporal_frame_;
    gi_presented_attempt_token_ = 1;
    temporal_frame_.reset = fixture.reset;
    temporal_frame_.attempt_token = kGiTestTemporalToken;
    const auto callback = [](VkCommandBuffer command_buffer, void* opaque) {
        auto& item = *static_cast<FixtureRecord*>(opaque);
        VkSceneRenderer& renderer = *item.renderer;
        const GiTemporalGpuFixture& f = *item.fixture;
        const auto clear_color = [&](matter::VkImageResource& image,
                                     const VkClearColorValue& value) {
            transition_for_use(command_buffer, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
            const VkImageSubresourceRange range{
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command_buffer, image.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value,
                                 1, &range);
        };
        VkClearColorValue value{};
        value.float32[0] = f.raw.x;
        value.float32[1] = f.raw.y;
        value.float32[2] = f.raw.z;
        value.float32[3] = f.raw.w;
        matter::VkImageResource& raw_signal =
            f.signal_mode == 0u ? renderer.raw_diffuse_
                                : renderer.raw_specular_;
        clear_color(raw_signal, value);
        value = {};
        value.float32[0] = f.raw_aux.x;
        value.float32[1] = f.raw_aux.y;
        clear_color(renderer.raw_specular_aux_, value);
        value = {};
        value.float32[0] = f.velocity.x;
        value.float32[1] = f.velocity.y;
        clear_color(renderer.velocity_, value);
        transition_for_use(command_buffer, renderer.depth_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkClearDepthStencilValue depth_clear{f.depth, 0};
        const VkImageSubresourceRange depth_range{
            VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(command_buffer, renderer.depth_.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &depth_clear, 1, &depth_range);
        value = {};
        value.float32[0] = f.normal.x;
        value.float32[1] = f.normal.y;
        value.float32[2] = f.normal.z;
        clear_color(renderer.normal_, value);
        value = {};
        value.uint32[0] = f.material_index;
        value.uint32[1] = f.instance_token;
        clear_color(renderer.material_instance_, value);
        const auto sampled = [&](matter::VkImageResource& image,
                                 VkImageAspectFlags aspect) {
            transition_for_use(command_buffer, image,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, aspect);
        };
        sampled(renderer.velocity_, VK_IMAGE_ASPECT_COLOR_BIT);
        sampled(renderer.depth_, VK_IMAGE_ASPECT_DEPTH_BIT);
        sampled(renderer.normal_, VK_IMAGE_ASPECT_COLOR_BIT);
        sampled(renderer.material_instance_, VK_IMAGE_ASPECT_COLOR_BIT);

        GiHistorySet* histories = f.signal_mode == 0u
            ? renderer.gi_history_ : renderer.gi_spec_history_;
        GiHistorySet& previous =
            histories[renderer.gi_presented_history_index_];
        value = {};
        value.float32[0] = f.previous_radiance.x;
        value.float32[1] = f.previous_radiance.y;
        value.float32[2] = f.previous_radiance.z;
        value.float32[3] = f.previous_radiance.w;
        clear_color(previous.radiance, value);
        value = {};
        value.float32[0] = f.previous_moments.x;
        value.float32[1] = f.previous_moments.y;
        clear_color(previous.moments, value);
        value = {};
        clear_color(previous.history_length, value);
        value.float32[0] = f.previous_depth;
        clear_color(previous.depth, value);
        value = {};
        value.float32[0] = f.previous_normal.x;
        value.float32[1] = f.previous_normal.y;
        value.float32[2] = f.previous_normal.z;
        clear_color(previous.normal, value);
        value = {};
        value.uint32[0] = f.previous_material_index;
        value.uint32[1] = f.previous_instance_token;
        clear_color(previous.identity, value);
        value = {};
        value.float32[0] = f.previous_aux.x;
        value.float32[1] = f.previous_aux.y;
        clear_color(previous.aux, value);

        transition_for_use(command_buffer, previous.history_length,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy patch{};
        patch.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        patch.imageSubresource.layerCount = 1;
        patch.imageOffset = {f.history_patch_pixel.x,
                             f.history_patch_pixel.y, 0};
        patch.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(command_buffer, item.upload,
                               previous.history_length.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &patch);
        matter::VulkanFrame fake{};
        fake.command_buffer = command_buffer;
        fake.frame_slot = renderer.active_frame_index_;
        fake.frame_slot_count =
            static_cast<uint32_t>(renderer.frames_.size());
        fake.serial = kGiTestTemporalToken;
        item.ok = renderer.record_gi_temporal_signal(
            fake, f.signal_mode, *item.error, false);
        if (!item.ok) return;
        GiHistorySet& output =
            histories[renderer.gi_candidate_history_index_];
        matter::VkImageResource* images[] = {
            &output.radiance, &output.moments, &output.history_length,
            &output.rejection, &output.aux};
        const VkDeviceSize offsets[] = {0, 8, 12, 16, 20};
        for (uint32_t index = 0; index < 5; ++index) {
            transition_for_use(command_buffer, *images[index],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offsets[index];
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = {f.output_pixel.x, f.output_pixel.y, 0};
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(command_buffer, images[index]->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   item.readback, 1, &copy);
        }
    };
    const bool submitted = matter::submit_immediate(
        *vulkan_, callback, &record, error,
        matter::ImmediateSubmitPhase::compute_dispatch,
        {upload.lifetime, readback.lifetime, raw_diffuse_.lifetime,
         raw_specular_.lifetime, raw_specular_aux_.lifetime,
         velocity_.lifetime, depth_.lifetime, normal_.lifetime,
         material_instance_.lifetime,
         gi_history_[0].radiance.lifetime, gi_history_[0].moments.lifetime,
         gi_history_[0].history_length.lifetime, gi_history_[0].depth.lifetime,
         gi_history_[0].normal.lifetime, gi_history_[0].identity.lifetime,
         gi_history_[0].rejection.lifetime, gi_history_[0].aux.lifetime,
         gi_history_[1].radiance.lifetime, gi_history_[1].moments.lifetime,
         gi_history_[1].history_length.lifetime, gi_history_[1].depth.lifetime,
         gi_history_[1].normal.lifetime, gi_history_[1].identity.lifetime,
         gi_history_[1].rejection.lifetime, gi_history_[1].aux.lifetime,
         gi_spec_history_[0].radiance.lifetime,
         gi_spec_history_[0].moments.lifetime,
         gi_spec_history_[0].history_length.lifetime,
         gi_spec_history_[0].depth.lifetime,
         gi_spec_history_[0].normal.lifetime,
         gi_spec_history_[0].identity.lifetime,
         gi_spec_history_[0].rejection.lifetime,
         gi_spec_history_[0].aux.lifetime,
         gi_spec_history_[1].radiance.lifetime,
         gi_spec_history_[1].moments.lifetime,
         gi_spec_history_[1].history_length.lifetime,
         gi_spec_history_[1].depth.lifetime,
         gi_spec_history_[1].normal.lifetime,
         gi_spec_history_[1].identity.lifetime,
         gi_spec_history_[1].rejection.lifetime,
         gi_spec_history_[1].aux.lifetime});
    gi_presented_attempt_token_ = saved_presented_token;
    gi_candidate_frame_serial_ = saved_candidate_serial;
    gi_candidate_attempt_token_ = saved_candidate_token;
    gi_candidate_was_reset_ = saved_candidate_reset;
    last_composite_used_gi_temporal_ = saved_composite;
    temporal_frame_ = saved_temporal;
    if (!submitted || !record.ok) return false;
    std::array<uint8_t, 32> bytes{};
    if (!matter::readback_buffer(*vulkan_, readback, bytes.data(), bytes.size(),
                                 0, error))
        return false;
    uint16_t radiance[4]{}, moments[2]{}, history = 0;
    std::memcpy(radiance, bytes.data(), sizeof(radiance));
    std::memcpy(moments, bytes.data() + 8, sizeof(moments));
    std::memcpy(&history, bytes.data() + 12, sizeof(history));
    std::memcpy(&result.rejection_bits, bytes.data() + 16,
                sizeof(result.rejection_bits));
    uint16_t aux[2]{};
    std::memcpy(aux, bytes.data() + 20, sizeof(aux));
    result.radiance = {half_to_float(radiance[0]),
                       half_to_float(radiance[1]),
                       half_to_float(radiance[2]),
                       half_to_float(radiance[3])};
    result.moments = {half_to_float(moments[0]),
                      half_to_float(moments[1]), 0.0f};
    result.history_length = history;
    result.aux = {half_to_float(aux[0]), half_to_float(aux[1]), 0.0f};
    return true;
}

bool VkSceneRenderer::test_dispatch_gi_atrous_fixture(
    const GiAtrousGpuFixture& fixture, GiAtrousGpuResult& result,
    std::string& error) {
    result = {};
    const size_t pixel_count =
        static_cast<size_t>(fixture.extent.width) * fixture.extent.height;
    if (fixture.extent.width < 33 || fixture.extent.height == 0 ||
        fixture.signal.size() != pixel_count ||
        fixture.moments.size() != pixel_count ||
        fixture.depth.size() != pixel_count ||
        fixture.normal.size() != pixel_count ||
        fixture.material_index.size() != pixel_count ||
        fixture.history_length.size() != pixel_count) {
        error = "GI A-trous GPU fixture requires complete inputs at least 33 pixels wide";
        return false;
    }
    vulkan_->wait_idle();
    const float saved_scale = gi_settings_.trace_scale;
    gi_settings_.trace_scale = 1.0f;
    if (!ensure_raster_targets(fixture.extent.width, fixture.extent.height,
                               error)) {
        gi_settings_.trace_scale = saved_scale;
        return false;
    }
    gi_settings_.trace_scale = saved_scale;

    const VkDeviceSize signal_offset = 0;
    const VkDeviceSize moments_offset = pixel_count * 8;
    const VkDeviceSize depth_offset = moments_offset + pixel_count * 4;
    const VkDeviceSize normal_offset = depth_offset + pixel_count * 4;
    const VkDeviceSize identity_offset = normal_offset + pixel_count * 8;
    const VkDeviceSize history_offset = identity_offset + pixel_count * 8;
    const VkDeviceSize upload_size = history_offset + pixel_count * 2;
    matter::VkBufferResource upload;
    matter::VkBufferResource readback;
    if (!matter::create_buffer(
            *vulkan_, upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, upload, error) ||
        !matter::create_buffer(
            *vulkan_, pixel_count * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            readback, error) ||
        !matter::map_buffer(upload, error))
        return false;
    auto* bytes = static_cast<uint8_t*>(upload.mapped);
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t signal[4] = {
            float_to_half(fixture.signal[i].x),
            float_to_half(fixture.signal[i].y),
            float_to_half(fixture.signal[i].z),
            float_to_half(fixture.signal[i].w)};
        const uint16_t moments[2] = {
            float_to_half(fixture.moments[i].x),
            float_to_half(fixture.moments[i].y)};
        const uint16_t normal[4] = {
            float_to_half(fixture.normal[i].x),
            float_to_half(fixture.normal[i].y),
            float_to_half(fixture.normal[i].z),
            float_to_half(fixture.normal[i].w)};
        const uint32_t identity[2] = {fixture.material_index[i], 1u};
        const uint16_t history = static_cast<uint16_t>(
            std::min(fixture.history_length[i], kGiHistoryLengthMax));
        std::memcpy(bytes + signal_offset + i * 8, signal, sizeof(signal));
        std::memcpy(bytes + moments_offset + i * 4, moments, sizeof(moments));
        std::memcpy(bytes + depth_offset + i * 4, &fixture.depth[i], 4);
        std::memcpy(bytes + normal_offset + i * 8, normal, sizeof(normal));
        std::memcpy(bytes + identity_offset + i * 8, identity,
                    sizeof(identity));
        std::memcpy(bytes + history_offset + i * 2, &history,
                    sizeof(history));
    }
    if (!matter::flush_buffer(upload, 0, upload_size, error)) return false;

    struct FixtureRecord {
        VkSceneRenderer* renderer;
        VkBuffer upload;
        VkBuffer readback;
        VkDeviceSize offsets[6];
        VkExtent2D extent;
        bool ok = true;
        std::string* error;
    } record{this, upload.buffer, readback.buffer,
             {signal_offset, moments_offset, depth_offset, normal_offset,
              identity_offset, history_offset}, fixture.extent, true, &error};
    const uint64_t saved_candidate_serial = gi_candidate_frame_serial_;
    const uint32_t saved_composite_index = gi_composite_history_index_;
    const bool saved_filtered_valid = gi_filtered_valid_;
    const uint32_t saved_filtered_index = gi_filtered_index_;
    gi_composite_history_index_ = gi_candidate_history_index_;
    gi_candidate_frame_serial_ = kGiTestAtrousToken;
    const auto callback = [](VkCommandBuffer command_buffer, void* opaque) {
        auto& item = *static_cast<FixtureRecord*>(opaque);
        VkSceneRenderer& renderer = *item.renderer;
        GiHistorySet& guide =
            renderer.gi_history_[renderer.gi_composite_history_index_];
        matter::VkImageResource* images[6] = {
            &guide.radiance, &guide.moments, &guide.depth,
            &guide.normal, &guide.identity, &guide.history_length};
        for (uint32_t index = 0; index < 6; ++index) {
            transition_for_use(command_buffer, *images[index],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = item.offsets[index];
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {item.extent.width, item.extent.height, 1};
            vkCmdCopyBufferToImage(command_buffer, item.upload,
                                   images[index]->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
        }
        matter::VulkanFrame fake{};
        fake.command_buffer = command_buffer;
        fake.frame_slot = renderer.active_frame_index_;
        fake.frame_slot_count =
            static_cast<uint32_t>(renderer.frames_.size());
        fake.serial = kGiTestAtrousToken;
        FrameResources& resources = renderer.frames_[fake.frame_slot];
        vkCmdFillBuffer(command_buffer, resources.gi_atrous_markers.buffer,
                        0, 5 * sizeof(uint32_t), 0);
        VkBufferMemoryBarrier2 clear_to_compute{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        clear_to_compute.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        clear_to_compute.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        clear_to_compute.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        clear_to_compute.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        clear_to_compute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clear_to_compute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clear_to_compute.buffer = resources.gi_atrous_markers.buffer;
        clear_to_compute.offset = 0;
        clear_to_compute.size = 5 * sizeof(uint32_t);
        VkDependencyInfo clear_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        clear_dependency.bufferMemoryBarrierCount = 1;
        clear_dependency.pBufferMemoryBarriers = &clear_to_compute;
        vkCmdPipelineBarrier2(command_buffer, &clear_dependency);
        item.ok = renderer.record_gi_atrous(fake, *item.error, false);
        if (!item.ok) return;
        matter::VkImageResource* outputs[2] = {
            &renderer.gi_atrous_[renderer.gi_filtered_index_],
            &renderer.gi_atrous_[renderer.gi_filtered_index_ ^ 1u]};
        for (uint32_t index = 0; index < 2; ++index) {
            transition_for_use(command_buffer, *outputs[index],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = static_cast<VkDeviceSize>(index) *
                                item.extent.width * item.extent.height * 8;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {item.extent.width, item.extent.height, 1};
            vkCmdCopyImageToBuffer(command_buffer, outputs[index]->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   item.readback, 1, &copy);
        }
        VkBufferMemoryBarrier2 markers_to_host{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        markers_to_host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        markers_to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        markers_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        markers_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        markers_to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        markers_to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        markers_to_host.buffer = resources.gi_atrous_markers.buffer;
        markers_to_host.offset = 0;
        markers_to_host.size = 5 * sizeof(uint32_t);
        VkDependencyInfo marker_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        marker_dependency.bufferMemoryBarrierCount = 1;
        marker_dependency.pBufferMemoryBarriers = &markers_to_host;
        vkCmdPipelineBarrier2(command_buffer, &marker_dependency);
    };
    const bool submitted = matter::submit_immediate(
        *vulkan_, callback, &record, error,
        matter::ImmediateSubmitPhase::compute_dispatch,
        {upload.lifetime, readback.lifetime,
         gi_history_[gi_composite_history_index_].radiance.lifetime,
         gi_history_[gi_composite_history_index_].moments.lifetime,
         gi_history_[gi_composite_history_index_].depth.lifetime,
         gi_history_[gi_composite_history_index_].normal.lifetime,
         gi_history_[gi_composite_history_index_].identity.lifetime,
         gi_history_[gi_composite_history_index_].history_length.lifetime,
         gi_atrous_[0].lifetime, gi_atrous_[1].lifetime,
         frames_[active_frame_index_].gi_atrous_markers.lifetime});
    gi_candidate_frame_serial_ = saved_candidate_serial;
    gi_composite_history_index_ = saved_composite_index;
    gi_filtered_valid_ = saved_filtered_valid;
    gi_filtered_index_ = saved_filtered_index;
    if (!submitted || !record.ok) return false;
    std::vector<uint16_t> packed(pixel_count * 8);
    if (!matter::readback_buffer(*vulkan_, readback, packed.data(),
                                 packed.size() * sizeof(uint16_t), 0, error))
        return false;
    result.filtered.resize(pixel_count);
    result.penultimate.resize(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
        result.filtered[i] = {
            half_to_float(packed[i * 4]),
            half_to_float(packed[i * 4 + 1]),
            half_to_float(packed[i * 4 + 2]),
            half_to_float(packed[i * 4 + 3])};
        const size_t offset = pixel_count * 4 + i * 4;
        result.penultimate[i] = {
            half_to_float(packed[offset]),
            half_to_float(packed[offset + 1]),
            half_to_float(packed[offset + 2]),
            half_to_float(packed[offset + 3])};
    }
    if (!matter::readback_buffer(
            *vulkan_, frames_[active_frame_index_].gi_atrous_markers,
            result.gpu_step_widths.data(),
            result.gpu_step_widths.size() * sizeof(uint32_t), 0, error))
        return false;
    return true;
}
#endif

bool VkSceneRenderer::record_gi_temporal(const matter::VulkanFrame& frame,
                                         std::string& error, bool retain) {
    return record_gi_temporal_signal(frame, 0u, error, retain) &&
           record_gi_temporal_signal(frame, 1u, error, retain);
}

bool VkSceneRenderer::record_gi_temporal_signal(
    const matter::VulkanFrame& frame, uint32_t signal_mode,
    std::string& error, bool retain) {
    if (frame.frame_slot >= frames_.size() ||
        gi_temporal_pipeline_ == VK_NULL_HANDLE)
        return true;
    const uint32_t previous_index = gi_presented_history_index_;
    const uint32_t candidate_index = previous_index ^ 1u;
    GiHistorySet* histories = signal_mode == 0u ? gi_history_ : gi_spec_history_;
    GiHistorySet& previous = histories[previous_index];
    GiHistorySet& candidate = histories[candidate_index];
    matter::VkImageResource& raw_signal =
        signal_mode == 0u ? raw_diffuse_ : raw_specular_;
    matter::VkImageResource& raw_aux = raw_specular_aux_;
    const auto sampled = [&](matter::VkImageResource& image) {
        transition_for_use(frame.command_buffer, image,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    };
    const auto storage = [&](matter::VkImageResource& image) {
        transition_for_use(frame.command_buffer, image, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    };
    transition_for_use(frame.command_buffer, raw_signal,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    transition_for_use(frame.command_buffer, raw_aux,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    sampled(previous.radiance);
    sampled(previous.moments);
    sampled(previous.history_length);
    sampled(previous.depth);
    sampled(previous.normal);
    sampled(previous.identity);
    sampled(previous.aux);
    storage(candidate.radiance);
    storage(candidate.moments);
    storage(candidate.history_length);
    storage(candidate.depth);
    storage(candidate.normal);
    storage(candidate.identity);
    storage(candidate.rejection);
    storage(candidate.aux);

    VkDescriptorImageInfo infos[21]{};
    const auto combined = [&](uint32_t binding,
                              const matter::VkImageResource& image) {
        infos[binding] = {composite_sampler_, image.view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    combined(0, raw_signal);
    combined(1, velocity_);
    combined(2, depth_);
    combined(3, normal_);
    combined(4, material_instance_);
    combined(5, previous.radiance);
    combined(6, previous.moments);
    combined(7, previous.history_length);
    combined(8, previous.depth);
    combined(9, previous.normal);
    combined(10, previous.identity);
    combined(18, raw_aux);
    combined(19, previous.aux);
    matter::VkImageResource* outputs[] = {
        &candidate.radiance, &candidate.moments, &candidate.history_length,
        &candidate.depth, &candidate.normal, &candidate.identity,
        &candidate.rejection};
    for (uint32_t binding = 11; binding < 18; ++binding)
        infos[binding] = {VK_NULL_HANDLE, outputs[binding - 11]->view,
                          VK_IMAGE_LAYOUT_GENERAL};
    infos[20] = {VK_NULL_HANDLE, candidate.aux.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[21]{};
    FrameResources& resources = frames_[frame.frame_slot];
    const VkDescriptorSet temporal_set =
        resources.gi_temporal_descriptor_sets[signal_mode];
    for (uint32_t binding = 0; binding < 21; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = temporal_set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            binding <= 10 || binding == 18 || binding == 19
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[binding].pImageInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(vulkan_->device(), 21, writes, 0, nullptr);
    VulkanGiTemporalConstants constants{};
    constants.temporal_extent[0] = raw_diffuse_extent_.width;
    constants.temporal_extent[1] = raw_diffuse_extent_.height;
    constants.gbuffer_extent[0] = raster_extent_.width;
    constants.gbuffer_extent[1] = raster_extent_.height;
    constants.reset = temporal_frame_.reset || gi_history_reset_pending_ ||
                      gi_presented_attempt_token_ == 0;
    gi_candidate_was_reset_ = constants.reset != 0;
    constants.attempt_token_lo =
        static_cast<uint32_t>(temporal_frame_.attempt_token);
    constants.presented_attempt_token_lo =
        static_cast<uint32_t>(gi_presented_attempt_token_);
    constants.signal_mode = signal_mode;
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      gi_temporal_pipeline_);
    vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            gi_temporal_pipeline_layout_, 0, 1,
                            &temporal_set, 0, nullptr);
    vkCmdPushConstants(frame.command_buffer, gi_temporal_pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(frame.command_buffer,
                  (raw_diffuse_extent_.width + 7u) / 8u,
                  (raw_diffuse_extent_.height + 7u) / 8u, 1);
    matter::record_image_transition(
        frame.command_buffer, candidate.radiance,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    gi_candidate_history_index_ = candidate_index;
    gi_composite_history_index_ = candidate_index;
    gi_candidate_frame_serial_ = frame.serial;
    gi_candidate_attempt_token_ = temporal_frame_.attempt_token;
    update_composite_descriptor(resources);
    std::vector<std::shared_ptr<void>> retained{
        raw_signal.lifetime, raw_aux.lifetime, velocity_.lifetime, depth_.lifetime,
        normal_.lifetime, material_instance_.lifetime};
    for (GiHistorySet* set : {&previous, &candidate}) {
        retained.push_back(set->radiance.lifetime);
        retained.push_back(set->moments.lifetime);
        retained.push_back(set->history_length.lifetime);
        retained.push_back(set->depth.lifetime);
        retained.push_back(set->normal.lifetime);
        retained.push_back(set->identity.lifetime);
        retained.push_back(set->rejection.lifetime);
        retained.push_back(set->aux.lifetime);
    }
    return !retain ||
           vulkan_->retain_for_frame(frame, std::move(retained), error);
}

bool VkSceneRenderer::record_gi_atrous(const matter::VulkanFrame& frame,
                                       std::string& error, bool retain) {
    gi_filtered_valid_ = false;
    if (!record_gi_atrous_signal(frame, 0u, error, retain) ||
        !record_gi_atrous_signal(frame, 1u, error, retain))
        return false;
    gi_filtered_valid_ = true;
    update_composite_descriptor(frames_[frame.frame_slot]);
    return true;
}

bool VkSceneRenderer::record_gi_atrous_signal(
    const matter::VulkanFrame& frame, uint32_t signal_mode,
    std::string& error, bool retain) {
    if (frame.frame_slot >= frames_.size() ||
        gi_atrous_pipeline_ == VK_NULL_HANDLE ||
        gi_candidate_frame_serial_ == 0)
        return true;
    GiHistorySet& guide =
        (signal_mode == 0u ? gi_history_ : gi_spec_history_)
            [gi_composite_history_index_];
    matter::VkImageResource* filtered =
        signal_mode == 0u ? gi_atrous_ : gi_spec_atrous_;
    const auto sampled = [&](matter::VkImageResource& image) {
        transition_for_use(frame.command_buffer, image,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    };
    sampled(guide.radiance);
    sampled(guide.moments);
    sampled(guide.depth);
    sampled(guide.normal);
    sampled(guide.identity);
    sampled(guide.history_length);
    sampled(guide.aux);
    for (uint32_t output_index = 0; output_index < 2; ++output_index) {
        auto& output = filtered[output_index];
        transition_for_use(frame.command_buffer, output,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    }

    FrameResources& resources = frames_[frame.frame_slot];
    matter::VkImageResource* inputs[3] = {
        &guide.radiance, &filtered[0], &filtered[1]};
    matter::VkImageResource* outputs[3] = {
        &filtered[0], &filtered[1], &filtered[0]};
    for (uint32_t set_index = 0; set_index < 3; ++set_index) {
        const uint32_t descriptor_index = signal_mode * 3u + set_index;
        matter::VkImageResource* sampled_images[6] = {
            inputs[set_index], &guide.moments, &guide.depth,
            &guide.normal, &guide.identity, &guide.history_length};
        VkDescriptorImageInfo infos[7]{};
        VkWriteDescriptorSet writes[9]{};
        for (uint32_t binding = 0; binding < 6; ++binding) {
            infos[binding] = {composite_sampler_, sampled_images[binding]->view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet =
                resources.gi_atrous_descriptor_sets[descriptor_index];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &infos[binding];
        }
        infos[6] = {VK_NULL_HANDLE, outputs[set_index]->view,
                    VK_IMAGE_LAYOUT_GENERAL};
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = resources.gi_atrous_descriptor_sets[descriptor_index];
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[6].pImageInfo = &infos[6];
        const VkDescriptorBufferInfo marker_info{
            resources.gi_atrous_markers.buffer, 0,
            5 * sizeof(uint32_t)};
        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = resources.gi_atrous_descriptor_sets[descriptor_index];
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[7].pBufferInfo = &marker_info;
        VkDescriptorImageInfo aux_info{composite_sampler_, guide.aux.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = resources.gi_atrous_descriptor_sets[descriptor_index];
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[8].pImageInfo = &aux_info;
        vkUpdateDescriptorSets(vulkan_->device(), 9, writes, 0, nullptr);
    }

    constexpr uint32_t steps[5] = {1, 2, 4, 8, 16};
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      gi_atrous_pipeline_);
    const uint32_t iteration_count = signal_mode == 0u ? 5u : 3u;
    for (uint32_t iteration = 0; iteration < iteration_count; ++iteration) {
        const uint32_t set_index = iteration == 0 ? 0 :
                                   (iteration & 1u ? 1u : 2u);
        if (iteration >= 2) {
            matter::VkImageResource& output = filtered[iteration & 1u];
            transition_for_use(frame.command_buffer, output,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        }
        const VkDescriptorSet set =
            resources.gi_atrous_descriptor_sets[signal_mode * 3u + set_index];
        vkCmdBindDescriptorSets(frame.command_buffer,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                gi_atrous_pipeline_layout_, 0, 1, &set, 0,
                                nullptr);
        VulkanGiAtrousConstants constants{};
        constants.extent[0] = raw_diffuse_extent_.width;
        constants.extent[1] = raw_diffuse_extent_.height;
        constants.step_width = steps[iteration];
        constants.signal_mode = signal_mode;
        constants.kernel_radius = signal_mode == 0u ? 2u : 1u;
        constants.phi_luminance = 4.0f;
        constants.phi_depth = 0.02f;
        constants.normal_power = 64.0f;
        constants.pass_index = iteration;
        vkCmdPushConstants(frame.command_buffer, gi_atrous_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                           &constants);
        vkCmdDispatch(frame.command_buffer,
                      (raw_diffuse_extent_.width + 7u) / 8u,
                      (raw_diffuse_extent_.height + 7u) / 8u, 1);
        matter::VkImageResource& written = filtered[iteration & 1u];
        matter::record_image_transition(
            frame.command_buffer, written,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                (iteration + 1u == iteration_count
                    ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0),
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    gi_filtered_index_ = (iteration_count - 1u) & 1u;
    if (!retain) return true;
    return vulkan_->retain_for_frame(
        frame,
        {guide.radiance.lifetime, guide.moments.lifetime,
         guide.depth.lifetime, guide.normal.lifetime,
         guide.identity.lifetime, guide.history_length.lifetime,
         guide.aux.lifetime, filtered[0].lifetime, filtered[1].lifetime,
         resources.gi_atrous_markers.lifetime},
        error);
}

void VkSceneRenderer::finish_ray_tracing_frame(uint64_t frame_serial,
                                               bool succeeded) {
    if (frame_serial == 0) return;
    if (gi_candidate_frame_serial_ == frame_serial) {
        if (succeeded) {
            gi_presented_history_index_ = gi_candidate_history_index_;
            gi_presented_attempt_token_ = gi_candidate_attempt_token_;
            if (gi_candidate_was_reset_)
                gi_history_reset_pending_ = false;
            if (gi_candidate_was_reset_) ++gi_history_reset_count_;
        }
        gi_candidate_frame_serial_ = 0;
        gi_candidate_attempt_token_ = 0;
    }
    for (auto& part : parts_) {
        bool finished_candidate = false;
        for (auto& lod : part.rt_lods) {
            if (lod.candidate_serial != frame_serial) continue;
            finished_candidate = true;
            if (lod.candidate) {
                if (succeeded) {
                    lod.blas = std::move(lod.candidate);
                    lod.built = true;
                    lod.geometry_opaque = lod.candidate_opaque;
                } else {
                    lod.candidate.reset();
                }
            } else {
                lod.built = succeeded;
                if (succeeded) lod.geometry_opaque = lod.candidate_opaque;
            }
            lod.candidate_serial = 0;
        }
        if (succeeded && finished_candidate) {
            part.rt_geometry_classification_dirty = std::any_of(
                part.rt_lods.begin(), part.rt_lods.end(),
                [&](const RtLodRecord& lod) {
                    return lod.built && lod.geometry_opaque !=
                        rt_material_ids_are_opaque(material_staging_,
                                                   lod.material_ids);
                });
        }
    }
}

void VkSceneRenderer::set_lighting(const VkSceneLighting& lighting) {
    const bool source_changed =
        lighting.sun_direction.x != lighting_.sun_direction.x ||
        lighting.sun_direction.y != lighting_.sun_direction.y ||
        lighting.sun_direction.z != lighting_.sun_direction.z ||
        lighting.sun_intensity != lighting_.sun_intensity ||
        lighting.sun_color.x != lighting_.sun_color.x ||
        lighting.sun_color.y != lighting_.sun_color.y ||
        lighting.sun_color.z != lighting_.sun_color.z ||
        lighting.sky_color.x != lighting_.sky_color.x ||
        lighting.sky_color.y != lighting_.sky_color.y ||
        lighting.sky_color.z != lighting_.sky_color.z ||
        lighting.emission_multiplier != lighting_.emission_multiplier;
    if (lighting_initialized_ && source_changed)
        gi_history_reset_pending_ = true;
    lighting_ = lighting;
    lighting_initialized_ = true;
}

void VkSceneRenderer::set_display_exposure(float exposure_ev) {
    display_exposure_ev_ = exposure_ev;
}

void VkSceneRenderer::release_part(uint64_t part_hash) {
    if (poisoned()) return;
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return;
    const uint32_t released_slot = static_cast<uint32_t>(found->second);
    std::vector<GpuCluster> compact_clusters;
    std::vector<std::vector<VkSceneLod>> compact_lods;
    std::vector<VkRasterVertex> compact_vertices;
    std::vector<uint32_t> compact_indices;
    compact_clusters.reserve(cluster_staging_.size() -
                             parts_[released_slot].cluster_count);
    compact_lods.reserve(compact_clusters.capacity());
    compact_vertices.reserve(vertex_staging_.size() -
                             parts_[released_slot].vertex_count);
    compact_indices.reserve(index_staging_.size() -
                            parts_[released_slot].index_count);
    for (uint32_t old_slot = 0; old_slot < parts_.size(); ++old_slot) {
        if (old_slot == released_slot || !parts_[old_slot].live) continue;
        PartRecord& part = parts_[old_slot];
        const uint32_t old_cluster_start = part.cluster_start;
        const uint32_t old_vertex_start = part.vertex_start;
        const uint32_t old_index_start = part.index_start;
        part.cluster_start = static_cast<uint32_t>(compact_clusters.size());
        // Vertex compaction: vertex_start adjusted, vertex VALUES untouched.
        part.vertex_start = static_cast<uint32_t>(compact_vertices.size());
        if (part.vertex_count != 0) {
            compact_vertices.insert(
                compact_vertices.end(),
                vertex_staging_.begin() + old_vertex_start,
                vertex_staging_.begin() + old_vertex_start +
                    part.vertex_count);
        }
        // Index compaction: index staging shifted, first_index rebased,
        // index VALUES are part-local and are NOT rewritten.
        const uint32_t index_delta = static_cast<uint32_t>(compact_indices.size());
        if (part.index_count != 0) {
            compact_indices.insert(
                compact_indices.end(),
                index_staging_.begin() + old_index_start,
                index_staging_.begin() + old_index_start + part.index_count);
        }
        part.index_start = index_delta;
        for (uint32_t i = 0; i < part.cluster_count; ++i) {
            GpuCluster cluster =
                cluster_staging_[old_cluster_start + i];
            cluster.part_slot = old_slot;
            compact_clusters.push_back(cluster);
            std::vector<VkSceneLod> lods =
                cluster_lods_[old_cluster_start + i];
            if (part.index_count != 0) {
                for (auto& lod : lods) {
                    lod.first_index = index_delta +
                                      (lod.first_index - old_index_start);
                }
            }
            compact_lods.push_back(std::move(lods));
        }
    }
    slot_of_.erase(found);
    parts_[released_slot] = {};
    std::vector<GpuInstance> kept_instances;
    std::vector<uint32_t> kept_slots;
    kept_instances.reserve(instance_staging_.size());
    kept_slots.reserve(instance_part_slots_.size());
    for (size_t i = 0; i < instance_staging_.size(); ++i) {
        const uint32_t old_slot = instance_part_slots_[i];
        if (old_slot == released_slot) continue;
        GpuInstance instance = instance_staging_[i];
        instance.cluster_start = parts_[old_slot].cluster_start;
        instance.cluster_count = parts_[old_slot].cluster_count;
        kept_instances.push_back(instance);
        kept_slots.push_back(old_slot);
    }
    cluster_staging_ = std::move(compact_clusters);
    cluster_lods_ = std::move(compact_lods);
    vertex_staging_ = std::move(compact_vertices);
    index_staging_ = std::move(compact_indices);
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
        instance.instance_token =
            stable_id != 0
                ? vulkan_history_token(stable_id)
                : static_cast<uint32_t>(source_index) + 1u;
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
                // Task 4 replaces this with indexed draw (index_count, first_index).
                command.vertex_count = lods[lod].index_count;
                command.first_vertex = lods[lod].first_index;
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

bool VkSceneRenderer::upload_scene_buffers(
    FrameResources& frame, VkCommandBuffer material_command_buffer,
    bool reset_stats, std::string& error) {
    VkDeviceSize cluster_bytes = 0;
    VkDeviceSize instance_bytes = 0;
    VkDeviceSize command_bytes = 0;
    VkDeviceSize transform_bytes = 0;
    VkDeviceSize vertex_bytes = 0;
    VkDeviceSize material_bytes = 0;
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
            "vertex buffer", error) ||
        !vk_scene_detail::checked_mul_to_device_size(
            material_staging_.size(), sizeof(MaterialGpuRecord),
            material_bytes, "material buffer", error)) {
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
        !storage_size_ok(transform_bytes, "draw-transform buffer") ||
        !storage_size_ok(material_bytes, "material buffer")) {
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
    frame.pending_material_bytes = 0;
    if (frame.material_generation != material_generation_) {
        const VkDeviceSize required =
            std::max<VkDeviceSize>(material_bytes, 1);
        VkDeviceSize material_capacity = frame.materials.size;
        VkDeviceSize upload_capacity = frame.material_upload.size;
        if (!vk_scene_detail::checked_grown_capacity(
                material_capacity, required, limits_.max_buffer_size,
                material_capacity, "material buffer", error) ||
            !vk_scene_detail::checked_grown_capacity(
                upload_capacity, required, limits_.max_buffer_size,
                upload_capacity, "material upload buffer", error)) {
            return false;
        }
        bool material_replaced = false;
        if (frame.materials.size < required) {
            matter::VkBufferResource replacement;
            if (!matter::create_buffer(
                    *vulkan_, material_capacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, replacement,
                    error)) {
                return false;
            }
            frame.materials = std::move(replacement);
            material_replaced = true;
        }
        if (frame.material_upload.size < required) {
            matter::VkBufferResource replacement;
            if (!matter::create_buffer(
                    *vulkan_, upload_capacity,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                    replacement, error)) {
                return false;
            }
            frame.material_upload = std::move(replacement);
        }
        if (!upload(frame.material_upload, material_staging_.data(),
                    material_bytes)) {
            return false;
        }
        if (material_replaced)
            update_descriptor(frame.descriptor_sets[1], 5,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              frame.materials);
        frame.pending_material_bytes = material_bytes;
        frame.material_generation = material_generation_;
        if (material_command_buffer != VK_NULL_HANDLE)
            record_material_upload(material_command_buffer, frame);
    }
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

void VkSceneRenderer::record_material_upload(VkCommandBuffer command_buffer,
                                             FrameResources& frame) {
    record_material_upload_commands(command_buffer,
                                    frame.material_upload.buffer,
                                    frame.materials.buffer,
                                    frame.pending_material_bytes);
    if (frame.pending_material_bytes != 0)
        ++frame.material_upload_record_count;
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
    constants.counts[2] = static_cast<uint32_t>(material_staging_.size());
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

void VkSceneRenderer::write_gpu_timestamp(VkCommandBuffer cmd, uint32_t zone_id,
                                          bool is_end, FrameResources& frame) {
    if (!gpu_timers_supported_ || frame.ts_pool == VK_NULL_HANDLE) return;
    write_ts(cmd, frame.ts_pool, zone_id, is_end);
    const uint8_t bit = is_end ? 2u : 1u;
    frame.ts_written[zone_id] |= bit;
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
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    test_last_rt_geometry_records_.clear();
    test_last_rt_blas_build_count_ = 0;
#endif
    // GPU timestamp readback, reset, and begin of the 'total' zone.
    // Must happen outside any render pass (vkCmdResetQueryPool requirement).
    if (gpu_timers_supported_ && selected.ts_pool != VK_NULL_HANDLE) {
        if (selected.ts_valid) {
            // Non-blocking readback of the previous frame's timestamps.
            constexpr uint32_t kQueryCount = kGpuZoneCount * 2u;
            // Two uint64_t per query: value + availability.
            uint64_t results[kQueryCount * 2]{};
            const VkResult rb = vkGetQueryPoolResults(
                vulkan_->device(), selected.ts_pool, 0, kQueryCount,
                sizeof(results), results, sizeof(uint64_t) * 2,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            // vkGetQueryPoolResults returns VK_NOT_READY when any query is unavailable;
            // the per-query availability bits below gate each sample individually.
            if (rb == VK_SUCCESS || rb == VK_NOT_READY) {
                for (uint32_t z = 0; z < kGpuZoneCount; ++z) {
                    const uint8_t written = selected.ts_written[z];
                    if ((written & 3u) != 3u) {
                        // Zone did not execute this frame — report 0 immediately.
                        gpu_smoothed_ms_[z] = 0.0f;
                        continue;
                    }
                    const uint64_t begin_val = results[z * 4 + 0];
                    const uint64_t begin_avail = results[z * 4 + 1];
                    const uint64_t end_val   = results[z * 4 + 2];
                    const uint64_t end_avail = results[z * 4 + 3];
                    if (!begin_avail || !end_avail) continue;
                    const float ms = static_cast<float>(
                        static_cast<double>(end_val - begin_val) *
                        timestamp_period_ns_ / 1e6);
                    gpu_smoothed_ms_[z] = gpu_smoothed_ms_[z] * 0.9f + ms * 0.1f;
                }
            }
        }
        // Reset all queries for this slot; must be outside a render pass.
        vkCmdResetQueryPool(frame.command_buffer, selected.ts_pool,
                            0, kGpuZoneCount * 2u);
        std::memset(selected.ts_written, 0, sizeof(selected.ts_written));
        selected.ts_valid = false;
        // Begin the 'total' zone immediately after the reset.
        write_ts(frame.command_buffer, selected.ts_pool, kGpuZoneTotal, false);
        selected.ts_written[kGpuZoneTotal] |= 1u;
    }
    if (!upload_scene_buffers(selected, frame.command_buffer, false, error) ||
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
        selected.material_upload.lifetime,
        selected.materials.lifetime,
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
    matter::Float3 camera_eye, float pixel_budget,
    VkExtent2D trace_extent, std::string& error) {
    auto clear_visibility = [&]() {
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        clear_color_image_for_use(frame.command_buffer, visibility_, one,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    auto clear_raw_diffuse = [&]() {
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        for (auto* image : {&raw_diffuse_, &raw_specular_,
                            &raw_specular_aux_, &raw_transmission_}) {
            clear_color_image_for_use(
                frame.command_buffer, *image, zero,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    };
    const bool native_trace_enabled =
        ray_tracing_settings_.enabled && vulkan_->ray_tracing_available()
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        && !test_force_rt_unavailable_
#endif
        ;
    if (!native_trace_enabled || rt_instances_.empty()) {
        clear_visibility();
        clear_raw_diffuse();
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
    std::vector<RtBuildSel> selected_geometry;
    std::vector<RtBlasPending> pending;
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        1, vulkan_->ray_tracing_properties()
               .min_acceleration_structure_scratch_offset_alignment);
    if (!build_ray_geometry(frame, camera_eye, pixel_budget,
                            get_sizes, cmd_build,
                            selected_geometry, pending, error))
        return false;
    bool instances_empty = false;
    if (!emit_ray_instances(frame, get_sizes, cmd_build, scratch_alignment,
                            selected_geometry, pending,
                            instances_empty, error))
        return false;
    if (instances_empty) {
        clear_visibility();
        return true;
    }
    if (!record_ray_trace_dispatch(frame, matrices, trace_extent,
                                   cmd_trace, error))
        return false;
    for (auto& item : pending)
        item.lod->candidate_serial = frame.serial;
    return true;
}

bool VkSceneRenderer::build_ray_geometry(
    const matter::VulkanFrame& frame,
    matter::Float3 camera_eye, float pixel_budget,
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
    std::vector<RtBuildSel>& selected_geometry,
    std::vector<RtBlasPending>& pending,
    std::string& error) {
    FrameResources& selected = frames_[frame.frame_slot];
    VkDeviceSize scratch_size = 1;
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        1, vulkan_->ray_tracing_properties()
               .min_acceleration_structure_scratch_offset_alignment);
    for (const RtInstance& source : rt_instances_) {
        const auto found = slot_of_.find(source.part_hash);
        if (found == slot_of_.end()) continue;
        PartRecord& part = parts_[static_cast<size_t>(found->second)];
        matter::Mat4f object_to_world{};
        std::memcpy(object_to_world.m, source.transform,
                    sizeof(object_to_world.m));
        for (uint32_t cluster_index = 0;
             cluster_index < part.cluster_count; ++cluster_index) {
            const uint32_t global_cluster =
                part.cluster_start + cluster_index;
            const GpuCluster& gpu_cluster = cluster_staging_[global_cluster];
            const uint32_t lod_index = vk_scene_detail::select_cluster_lod_view(
                {gpu_cluster.aabb_min[0], gpu_cluster.aabb_min[1],
                 gpu_cluster.aabb_min[2]},
                {gpu_cluster.aabb_max[0], gpu_cluster.aabb_max[1],
                 gpu_cluster.aabb_max[2]},
                gpu_cluster.radius, gpu_cluster.thresholds,
                gpu_cluster.lod_count, object_to_world, camera_eye,
                pixel_budget);
            uint32_t record_index = 0;
            if (!vk_scene_detail::dense_rt_lod_index(
                    part.rt_cluster_lod_offsets, cluster_index, lod_index,
                    record_index))
                continue;
            RtLodRecord& record = part.rt_lods[record_index];
            if (record.primitive_count != 0) {
                const bool opaque = rt_material_ids_are_opaque(
                    material_staging_, record.material_ids);
                selected_geometry.push_back(
                    {&part, &record, &source, opaque});
            }
        }
    }
    for (const RtBuildSel& selected_lod : selected_geometry) {
        PartRecord& part = *selected_lod.part;
        RtLodRecord& lod = *selected_lod.lod;
        if (!part.rt_geometry || lod.candidate_serial != 0 ||
            (lod.built && lod.geometry_opaque == selected_lod.opaque) ||
            std::any_of(pending.begin(), pending.end(),
                        [&lod](const RtBlasPending& item) {
                            return item.lod == &lod;
                        }))
            continue;
        RtBlasPending item{};
        item.part = &part;
        item.lod = &lod;
        auto& triangles = item.geometry.geometry.triangles;
        triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        // Task 5 replaces this with indexed BLAS geometry using first_index and
        // the index buffer.  For now, use vertex-offset 0 (soup fallback).
        triangles.vertexData.deviceAddress = part.rt_geometry->address;
        triangles.vertexStride = sizeof(VkRasterVertex);
        triangles.maxVertex = static_cast<uint32_t>(
            part.rt_geometry->size / sizeof(VkRasterVertex)) - 1;
        triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        item.geometry.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        item.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        item.geometry.flags = selected_lod.opaque
                                  ? VK_GEOMETRY_OPAQUE_BIT_KHR
                                  : 0;
        item.build.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        item.build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        item.build.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        item.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        item.build.geometryCount = 1;
        item.build.pGeometries = &item.geometry;
        item.range.primitiveCount = lod.primitive_count;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        get_sizes(vulkan_->device(),
                  VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                  &item.build, &item.range.primitiveCount, &sizes);
        if (lod.built) {
            auto replacement = std::make_shared<
                matter::VkAccelerationStructureResource>();
            if (!matter::create_acceleration_structure(
                    *vulkan_, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                    sizes.accelerationStructureSize, *replacement, error))
                return false;
            lod.candidate = std::move(replacement);
            item.target = lod.candidate;
        } else {
            if (!lod.blas) {
                lod.blas = std::make_shared<
                    matter::VkAccelerationStructureResource>();
                if (!matter::create_acceleration_structure(
                        *vulkan_,
                        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                        sizes.accelerationStructureSize, *lod.blas, error))
                    return false;
            }
            item.target = lod.blas;
        }
        lod.candidate_opaque = selected_lod.opaque;
        item.build.dstAccelerationStructure = item.target->handle;
        item.scratch_size =
            (sizes.buildScratchSize + scratch_alignment - 1) /
            scratch_alignment * scratch_alignment;
        pending.push_back(item);
    }
    // Builds within a batch get disjoint scratch regions so the GPU can
    // overlap them; the budget chunks first-load spikes into several batches
    // instead of growing the scratch buffer without bound.
    constexpr VkDeviceSize kBlasScratchBudget = 64ull << 20;
    std::vector<size_t> batch_ends;
    VkDeviceSize batch_offset = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (batch_offset > 0 &&
            batch_offset + pending[i].scratch_size > kBlasScratchBudget) {
            batch_ends.push_back(i);
            batch_offset = 0;
        }
        pending[i].scratch_offset = batch_offset;
        batch_offset += pending[i].scratch_size;
        scratch_size = std::max(scratch_size, batch_offset);
    }
    batch_ends.push_back(pending.size());
    if (!ensure_build_buffer(
            selected.rt_scratch, scratch_size + scratch_alignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            error)) return false;
    const VkDeviceAddress blas_scratch_address =
        (selected.rt_scratch.address + scratch_alignment - 1) /
        scratch_alignment * scratch_alignment;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> batch_builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> batch_ranges;
    size_t batch_begin = 0;
    const bool has_blas_work = !pending.empty();
    if (has_blas_work)
        write_gpu_timestamp(frame.command_buffer, kGpuZoneBlas, false, selected);
    for (const size_t batch_end : batch_ends) {
        if (batch_end == batch_begin) continue;
        batch_builds.clear();
        batch_ranges.clear();
        for (size_t i = batch_begin; i < batch_end; ++i) {
            RtBlasPending& item = pending[i];
            item.build.pGeometries = &item.geometry;
            item.build.scratchData.deviceAddress =
                blas_scratch_address + item.scratch_offset;
            batch_builds.push_back(item.build);
            batch_ranges.push_back(&item.range);
        }
        cmd_build(frame.command_buffer,
                  static_cast<uint32_t>(batch_builds.size()),
                  batch_builds.data(), batch_ranges.data());
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
        batch_begin = batch_end;
    }
    if (has_blas_work)
        write_gpu_timestamp(frame.command_buffer, kGpuZoneBlas, true, selected);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    test_last_rt_blas_build_count_ = static_cast<uint32_t>(pending.size());
#endif
    return true;
}

bool VkSceneRenderer::emit_ray_instances(
    const matter::VulkanFrame& frame,
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
    VkDeviceSize scratch_alignment,
    const std::vector<RtBuildSel>& selected_geometry,
    const std::vector<RtBlasPending>& pending,
    bool& instances_empty,
    std::string& error) {
    instances_empty = false;
    FrameResources& selected = frames_[frame.frame_slot];
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<GpuRtPartRecord> part_records;
    instances.reserve(selected_geometry.size());
    part_records.reserve(selected_geometry.size());
    for (const RtBuildSel& selected_lod : selected_geometry) {
        const PartRecord& part = *selected_lod.part;
        const RtLodRecord& lod = *selected_lod.lod;
        const RtInstance& source = *selected_lod.source;
        const auto& traced_blas = lod.candidate ? lod.candidate : lod.blas;
        if (!traced_blas) continue;
        if (part_records.size() >= kTlasCustomIndexMax) {
            error = "RT geometry table exceeds TLAS custom-index capacity";
            return false;
        }
        VkAccelerationStructureInstanceKHR instance{};
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                instance.transform.matrix[row][col] =
                    source.transform[row * 4 + col];
        instance.instanceCustomIndex =
            static_cast<uint32_t>(part_records.size());
        instance.mask = selected_lod.opaque ? 0x01 : 0x02;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = traced_blas->address;
        instances.push_back(instance);
        GpuRtPartRecord record{};
        // Task 5 replaces this with indexed vertex lookup via first_index.
        record.vertex_address = part.rt_geometry->address;
        record.vertex_stride = sizeof(VkRasterVertex);
        record.vertex_count = static_cast<uint32_t>(
            part.rt_geometry->size / sizeof(VkRasterVertex));
        record.primitive_count = lod.primitive_count;
        record.valid = 1;
        part_records.push_back(record);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        const bool built_this_frame = std::any_of(
            pending.begin(), pending.end(), [&lod](const RtBlasPending& item) {
                return item.lod == &lod;
            });
        test_last_rt_geometry_records_.push_back(
            {part.hash, lod.cluster_index, lod.lod_index,
             instance.instanceCustomIndex, lod.first_index, lod.index_count,
             record.vertex_address, traced_blas->address,
             selected_lod.opaque, built_this_frame});
#endif
    }
    if (instances.empty()) {
        instances_empty = true;
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
    write_gpu_timestamp(frame.command_buffer, kGpuZoneTlas, false, selected);
    cmd_build(frame.command_buffer, 1, &tlas_build, &tlas_range_ptr);
    VkMemoryBarrier2 as_to_ray{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    write_gpu_timestamp(frame.command_buffer, kGpuZoneTlas, true, selected);
    as_to_ray.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    as_to_ray.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    as_to_ray.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    as_to_ray.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo as_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    as_dependency.memoryBarrierCount = 1;
    as_dependency.pMemoryBarriers = &as_to_ray;
    vkCmdPipelineBarrier2(frame.command_buffer, &as_dependency);

    const VkDeviceSize part_bytes = std::max<VkDeviceSize>(
        sizeof(GpuRtPartRecord),
        part_records.size() * sizeof(GpuRtPartRecord));
    if (!ensure_buffer(selected.rt_parts, part_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) ||
        !matter::map_buffer(selected.rt_parts, error) ||
        !matter::map_buffer(selected.rt_error_counter, error)) return false;
    std::memset(selected.rt_parts.mapped, 0,
                static_cast<size_t>(part_bytes));
    if (!part_records.empty())
        std::memcpy(selected.rt_parts.mapped, part_records.data(),
                    part_records.size() * sizeof(GpuRtPartRecord));
    std::memset(selected.rt_error_counter.mapped, 0, sizeof(GpuRtCounters));
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (!matter::map_buffer(selected.rt_test_output, error)) return false;
    auto* test_words =
        static_cast<uint32_t*>(selected.rt_test_output.mapped);
    test_words[18] = 0u;
    test_words[19] = 0u;
    if (!matter::flush_buffer(selected.rt_test_output,
                              18 * sizeof(uint32_t),
                              2 * sizeof(uint32_t), error)) return false;
#endif
    if (!matter::flush_buffer(selected.rt_parts, 0, part_bytes, error) ||
        !matter::flush_buffer(selected.rt_error_counter, 0,
                              sizeof(GpuRtCounters),
                              error)) return false;
    return true;
}

bool VkSceneRenderer::record_ray_trace_dispatch(
    const matter::VulkanFrame& frame,
    const FrameMatrices& matrices,
    VkExtent2D trace_extent,
    PFN_vkCmdTraceRaysKHR cmd_trace,
    std::string& error) {
    FrameResources& selected = frames_[frame.frame_slot];
    transition_for_use(frame.command_buffer, visibility_, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    const VkClearColorValue gi_zero{{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_color_image_for_use(frame.command_buffer, raw_diffuse_, gi_zero,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    for (auto* specular_image : {&raw_specular_, &raw_specular_aux_,
                                 &raw_transmission_}) {
        clear_color_image_for_use(
            frame.command_buffer, *specular_image, gi_zero,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    VkWriteDescriptorSetAccelerationStructureKHR as_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &selected.rt_tlas.handle;
    VkDescriptorImageInfo depth_info{composite_sampler_, depth_.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo visibility_info{VK_NULL_HANDLE, visibility_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_diffuse_info{VK_NULL_HANDLE, raw_diffuse_.view,
                                           VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo albedo_info{composite_sampler_, albedo_.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo normal_info{composite_sampler_, normal_.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo orm_info{composite_sampler_, orm_.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo identity_info{composite_sampler_, material_instance_.view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo raw_specular_info{VK_NULL_HANDLE, raw_specular_.view,
                                            VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_specular_aux_info{VK_NULL_HANDLE,
                                                raw_specular_aux_.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_transmission_info{VK_NULL_HANDLE,
                                                raw_transmission_.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo part_info{selected.rt_parts.buffer, 0,
                                     selected.rt_parts.size};
    VkDescriptorBufferInfo material_info{selected.materials.buffer, 0,
                                         selected.materials.size};
    VkDescriptorBufferInfo error_info{selected.rt_error_counter.buffer, 0,
                                      selected.rt_error_counter.size};
    VkDescriptorBufferInfo test_output_info{selected.rt_test_output.buffer, 0,
                                            selected.rt_test_output.size};
    VkWriteDescriptorSet writes[15]{};
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
    const VkDescriptorBufferInfo* rt_buffers[] = {
        &part_info, &material_info, &error_info, &test_output_info};
    for (uint32_t i = 3; i < 7; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = rt_buffers[i - 3];
    }
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &raw_diffuse_info;
    VkDescriptorImageInfo* gi_inputs[] = {&albedo_info, &normal_info, &orm_info};
    for (uint32_t i = 8; i < 11; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = gi_inputs[i - 8];
    }
    VkDescriptorImageInfo* extra_infos[] = {
        &identity_info, &raw_specular_info, &raw_specular_aux_info,
        &raw_transmission_info};
    for (uint32_t i = 11; i < 15; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 11
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = extra_infos[i - 11];
    }
    vkUpdateDescriptorSets(vulkan_->device(), 15, writes, 0, nullptr);
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
    const VkStridedDeviceAddressRegionKHR raygen{rt_sbt_address_, handle_stride,
                                                  handle_stride};
    const VkStridedDeviceAddressRegionKHR miss{rt_sbt_miss_address_,
                                                handle_stride,
                                                rt_sbt_miss_size_};
    const VkStridedDeviceAddressRegionKHR hit{rt_sbt_hit_address_, handle_stride,
                                               rt_sbt_hit_size_};
    const VkStridedDeviceAddressRegionKHR callable{};
    FrameResources& rt_frame_slot = frames_[frame.frame_slot];
    write_gpu_timestamp(frame.command_buffer, kGpuZoneRt, false, rt_frame_slot);
    cmd_trace(frame.command_buffer, &raygen, &miss, &hit, &callable,
              trace_extent.width, trace_extent.height, 1);
    if (gi_settings_.enabled) {
        const VkClearColorValue dispatch_zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        clear_color_image_for_use(
            frame.command_buffer, raw_diffuse_, dispatch_zero,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        struct alignas(16) GiConstants {
            GpuMat4 clip_to_world;
            float to_sun_intensity[4];
            float sun_color_bias[4];
            float sky_color_distance[4];
            uint32_t presented_frame_index;
            float max_reflection_roughness;
            float diffuse_multiplier;
            float reflection_multiplier;
            float emission_multiplier;
            float pad0;
            float pad1;
            float pad2;
        } gi{};
        gi.clip_to_world = pack_glsl_mat4(matrices.clip_to_world);
        gi.to_sun_intensity[0] = constants.to_sun_max_distance[0];
        gi.to_sun_intensity[1] = constants.to_sun_max_distance[1];
        gi.to_sun_intensity[2] = constants.to_sun_max_distance[2];
        gi.to_sun_intensity[3] = lighting_.sun_intensity;
        gi.sun_color_bias[0] = lighting_.sun_color.x;
        gi.sun_color_bias[1] = lighting_.sun_color.y;
        gi.sun_color_bias[2] = lighting_.sun_color.z;
        gi.sun_color_bias[3] = ray_tracing_settings_.bias;
        gi.sky_color_distance[0] = lighting_.sky_color.x;
        gi.sky_color_distance[1] = lighting_.sky_color.y;
        gi.sky_color_distance[2] = lighting_.sky_color.z;
        gi.sky_color_distance[3] = ray_tracing_settings_.max_distance;
        gi.presented_frame_index =
            static_cast<uint32_t>(temporal_frame_.presented_frame_index);
        gi.max_reflection_roughness =
            std::clamp(gi_settings_.max_reflection_roughness, 0.02f, 1.0f);
        gi.diffuse_multiplier = gi_settings_.diffuse_multiplier;
        gi.reflection_multiplier = gi_settings_.reflection_multiplier;
        gi.emission_multiplier = lighting_.emission_multiplier;
        vkCmdPushConstants(frame.command_buffer, rt_pipeline_layout_,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(gi), &gi);
        const VkStridedDeviceAddressRegionKHR gi_raygen{
            rt_sbt_lighting_raygen_address_, handle_stride, handle_stride};
        cmd_trace(frame.command_buffer, &gi_raygen, &miss, &hit, &callable,
                  raw_diffuse_extent_.width, raw_diffuse_extent_.height, 1);
        ++last_rt_trace_dispatches_;
    }
    write_gpu_timestamp(frame.command_buffer, kGpuZoneRt, true, rt_frame_slot);
    if (gi_settings_.enabled) {
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDenoise, false,
                            rt_frame_slot);
        if (!record_gi_temporal(frame, error)) return false;
        if (!record_gi_atrous(frame, error)) return false;
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDenoise, true,
                            rt_frame_slot);
    }
    VkMemoryBarrier2 counters_to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    counters_to_host.srcStageMask =
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    counters_to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    counters_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    counters_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo counters_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    counters_dependency.memoryBarrierCount = 1;
    counters_dependency.pMemoryBarriers = &counters_to_host;
    vkCmdPipelineBarrier2(frame.command_buffer, &counters_dependency);
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
    matter::record_image_transition(
        frame.command_buffer, raw_diffuse_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        gi_settings_.enabled ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                             : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        gi_settings_.enabled ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                             : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    matter::VkImageResource& composite_specular =
        gi_settings_.enabled && gi_filtered_valid_
            ? gi_spec_atrous_[gi_filtered_index_] : raw_specular_;
    transition_for_use(frame.command_buffer, composite_specular,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    transition_for_use(frame.command_buffer, raw_transmission_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    std::vector<std::shared_ptr<void>> retained{visibility_.lifetime,
        raw_diffuse_.lifetime, raw_specular_.lifetime,
        raw_specular_aux_.lifetime, raw_transmission_.lifetime,
        composite_specular.lifetime,
        selected.rt_instances.lifetime, selected.rt_scratch.lifetime,
        selected.rt_tlas_scratch.lifetime,
        selected.rt_tlas.lifetime, selected.rt_parts.lifetime,
        selected.rt_error_counter.lifetime, selected.materials.lifetime,
        selected.rt_test_output.lifetime, rt_sbt_.lifetime};
    for (const auto& part : parts_) {
        if (part.rt_geometry) retained.push_back(part.rt_geometry->lifetime);
        for (const auto& lod : part.rt_lods) {
            if (lod.candidate)
                retained.push_back(lod.candidate->lifetime);
            else if (lod.blas)
                retained.push_back(lod.blas->lifetime);
        }
    }
    if (!vulkan_->retain_for_frame(frame, std::move(retained), error))
        return false;
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
    const bool native_gi_effective = gi_settings_.enabled &&
        ray_tracing_settings_.enabled && vulkan_->ray_tracing_available() &&
        !rt_instances_.empty()
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        && !test_force_rt_unavailable_
#endif
        ;
    if (!native_gi_effective) {
        // A previously filtered reflection must never remain bound when this
        // frame cannot produce native RT lighting. Keep reset pending until a
        // successful native candidate is presented again.
        gi_filtered_valid_ = false;
        gi_candidate_frame_serial_ = 0;
        gi_candidate_attempt_token_ = 0;
        gi_history_reset_pending_ = true;
    }
    update_composite_descriptor(selected);
    // prepare_frame owns the existing scene resources for this slot. Newly
    // created/replaced attachments are retained here before commands reference
    // them, preserving the frame-lifetime contract.
    std::vector<std::shared_ptr<void>> attachments{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        material_instance_.lifetime, selected.materials.lifetime,
        depth_.lifetime, hdr_.lifetime,
        visibility_.lifetime, raw_diffuse_.lifetime,
        raw_specular_.lifetime, raw_specular_aux_.lifetime,
        raw_transmission_.lifetime,
        gi_atrous_[0].lifetime, gi_atrous_[1].lifetime,
        gi_spec_atrous_[0].lifetime, gi_spec_atrous_[1].lifetime};
    for (auto* histories : {&gi_history_, &gi_spec_history_}) {
        for (auto& history : *histories) {
            attachments.push_back(history.radiance.lifetime);
            attachments.push_back(history.moments.lifetime);
            attachments.push_back(history.history_length.lifetime);
            attachments.push_back(history.depth.lifetime);
            attachments.push_back(history.normal.lifetime);
            attachments.push_back(history.identity.lifetime);
            attachments.push_back(history.rejection.lifetime);
            attachments.push_back(history.aux.lifetime);
        }
    }
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
        write_gpu_timestamp(frame.command_buffer, kGpuZoneCull, false, selected);
        const CullDispatchRecord dispatch{
            pipeline_, pipeline_layout_,
            {selected.descriptor_sets[0], selected.descriptor_sets[1]},
            group_count};
        record_cull_dispatch_commands(frame.command_buffer, dispatch);
        write_gpu_timestamp(frame.command_buffer, kGpuZoneCull, true, selected);
    }

    bool ray_trace_ok = true;
    VkSceneLighting frame_lighting = lighting_;
    frame_lighting.diffuse_rt_multiplier = gi_settings_.enabled &&
                                  ray_tracing_settings_.enabled &&
                                  vulkan_->ray_tracing_available()
                              ? 1.0f
                              : 0.0f;
    frame_lighting.debug_view =
        ray_tracing_settings_.enabled && vulkan_->ray_tracing_available() &&
                ray_tracing_settings_.debug_view
            ? 1.0f
            : 0.0f;
    RasterRecord record{&albedo_,
                        &normal_,
                        &orm_,
                        &velocity_,
                        &material_instance_,
                        &depth_,
                        &hdr_,
                        &visibility_,
                        &raw_diffuse_,
                        &raw_specular_,
                        &raw_transmission_,
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
                        camera_eye,
                        pixel_budget,
                        &error,
                        &ray_trace_ok,
                        selected.ts_pool,
                        selected.ts_written,
                        kGpuZoneGBuffer};
    record_raster(frame.command_buffer, &record);
    if (!ray_trace_ok) return false;
    raster_attachments_ready_ = true;
    selected.stats_valid = true;
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
    if (!upload_scene_buffers(selected, VK_NULL_HANDLE, true, error))
        return false;
    if (!upload_frame_constants(selected, frame, camera_eye, pixel_budget,
                                error))
        return poison(error);
    active_frame_index_ = 0;
    if (instance_staging_.empty() || max_clusters_per_instance_ == 0)
        return true;
    CullDispatchRecord dispatch{pipeline_, pipeline_layout_,
                                {selected.descriptor_sets[0],
                                 selected.descriptor_sets[1]},
                                group_count,
                                selected.material_upload.buffer,
                                selected.materials.buffer,
                                selected.pending_material_bytes,
                                &selected.material_upload_record_count};
    std::vector<std::shared_ptr<void>> dependencies{
        selected.frame_constants.lifetime, clusters_.lifetime,
        selected.instances.lifetime, selected.commands.lifetime,
        selected.draw_transforms.lifetime, selected.stats.lifetime,
        selected.material_upload.lifetime, selected.materials.lifetime};
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
    const uint32_t raw_width = std::max(
        1u, static_cast<uint32_t>(std::ceil(width * gi_settings_.trace_scale)));
    const uint32_t raw_height = std::max(
        1u, static_cast<uint32_t>(std::ceil(height * gi_settings_.trace_scale)));
    if (raster_extent_.width == width && raster_extent_.height == height &&
        raw_diffuse_extent_.width == raw_width &&
        raw_diffuse_extent_.height == raw_height &&
        albedo_.image != VK_NULL_HANDLE && normal_.image != VK_NULL_HANDLE &&
        orm_.image != VK_NULL_HANDLE && depth_.image != VK_NULL_HANDLE &&
        velocity_.image != VK_NULL_HANDLE &&
        material_instance_.image != VK_NULL_HANDLE &&
        hdr_.image != VK_NULL_HANDLE &&
        visibility_.image != VK_NULL_HANDLE &&
        raw_diffuse_.image != VK_NULL_HANDLE &&
        raw_specular_.image != VK_NULL_HANDLE &&
        raw_specular_aux_.image != VK_NULL_HANDLE &&
        raw_transmission_.image != VK_NULL_HANDLE &&
        gi_history_[0].radiance.image != VK_NULL_HANDLE &&
        gi_history_[1].radiance.image != VK_NULL_HANDLE &&
        gi_atrous_[0].image != VK_NULL_HANDLE &&
        gi_atrous_[1].image != VK_NULL_HANDLE) {
        return true;
    }
    if (raster_extent_.width != 0 || raster_extent_.height != 0) {
        vulkan_->wait_idle();
        if (!dlss_bridge_->free_dlss_resources(error)) return false;
    }
    matter::VkImageResource albedo;
    matter::VkImageResource normal;
    matter::VkImageResource orm;
    matter::VkImageResource velocity;
    matter::VkImageResource material_instance;
    matter::VkImageResource depth;
    matter::VkImageResource hdr;
    matter::VkImageResource visibility;
    matter::VkImageResource raw_diffuse;
    matter::VkImageResource raw_specular;
    matter::VkImageResource raw_specular_aux;
    matter::VkImageResource raw_transmission;
    GiHistorySet history[2];
    GiHistorySet spec_history[2];
    matter::VkImageResource atrous[2];
    matter::VkImageResource spec_atrous[2];
    const VkExtent3D extent{width, height, 1};
    const VkExtent3D raw_extent{raw_width, raw_height, 1};
    VkImageUsageFlags visibility_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vulkan_->ray_tracing_available())
        visibility_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags gbuffer_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
        !matter::create_image(*vulkan_, VK_IMAGE_TYPE_2D,
                              VK_FORMAT_R32G32_UINT, extent,
                              gbuffer_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              material_instance, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, extent,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
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
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, extent,
            visibility_usage,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            visibility, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, raw_extent,
            visibility_usage,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            raw_diffuse, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, raw_extent, visibility_usage,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            raw_specular, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, raw_extent,
            visibility_usage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, raw_specular_aux, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, raw_extent, visibility_usage,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            raw_transmission, error)) {
        return false;
    }
    const VkImageUsageFlags history_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (auto* sets : {&history, &spec_history}) {
        for (auto& set : *sets) {
        const auto make = [&](VkFormat format,
                              matter::VkImageResource& resource) {
            return matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_2D, format, raw_extent, history_usage,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, resource, error);
        };
        if (!make(VK_FORMAT_R16G16B16A16_SFLOAT, set.radiance) ||
            !make(VK_FORMAT_R16G16_SFLOAT, set.moments) ||
            !make(VK_FORMAT_R16_UINT, set.history_length) ||
            !make(VK_FORMAT_R32_SFLOAT, set.depth) ||
            !make(VK_FORMAT_R16G16B16A16_SFLOAT, set.normal) ||
            !make(VK_FORMAT_R32G32_UINT, set.identity) ||
            !make(VK_FORMAT_R32_UINT, set.rejection) ||
            !make(VK_FORMAT_R16G16_SFLOAT, set.aux))
            return false;
        }
    }
    for (auto& image : atrous) {
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_2D,
                VK_FORMAT_R16G16B16A16_SFLOAT, raw_extent, history_usage,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, error))
            return false;
    }
    for (auto& image : spec_atrous) {
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_2D,
                VK_FORMAT_R16G16B16A16_SFLOAT, raw_extent, history_usage,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, error))
            return false;
    }
    albedo_ = std::move(albedo);
    normal_ = std::move(normal);
    orm_ = std::move(orm);
    velocity_ = std::move(velocity);
    material_instance_ = std::move(material_instance);
    depth_ = std::move(depth);
    hdr_ = std::move(hdr);
    visibility_ = std::move(visibility);
    raw_diffuse_ = std::move(raw_diffuse);
    raw_specular_ = std::move(raw_specular);
    raw_specular_aux_ = std::move(raw_specular_aux);
    raw_transmission_ = std::move(raw_transmission);
    gi_history_[0] = std::move(history[0]);
    gi_history_[1] = std::move(history[1]);
    gi_spec_history_[0] = std::move(spec_history[0]);
    gi_spec_history_[1] = std::move(spec_history[1]);
    gi_atrous_[0] = std::move(atrous[0]);
    gi_atrous_[1] = std::move(atrous[1]);
    gi_spec_atrous_[0] = std::move(spec_atrous[0]);
    gi_spec_atrous_[1] = std::move(spec_atrous[1]);
    gi_filtered_index_ = 0;
    gi_filtered_valid_ = false;
    gi_presented_history_index_ = 0;
    gi_candidate_history_index_ = 1;
    gi_composite_history_index_ = 1;
    gi_candidate_frame_serial_ = 0;
    gi_candidate_attempt_token_ = 0;
    gi_presented_attempt_token_ = 0;
    gi_history_reset_pending_ = true;
    raw_diffuse_extent_ = {raw_width, raw_height};
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
                        &material_instance_,
                        &depth_,
                        &hdr_,
                        &visibility_,
                        &raw_diffuse_,
                        &raw_specular_,
                        &raw_transmission_,
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
                        {},
                        1.0f,
                        nullptr,
                        nullptr};
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        material_instance_.lifetime, depth_.lifetime, hdr_.lifetime,
        visibility_.lifetime, raw_diffuse_.lifetime,
        vertices_.lifetime, selected.commands.lifetime,
        selected.frame_constants.lifetime, selected.draw_transforms.lifetime,
        selected.materials.lifetime};
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

#ifdef MATTER_VK_TEST_FAULT_INJECTION
bool VkSceneRenderer::test_record_hdr_constant(
    const matter::VulkanFrame& frame, matter::Float3 color,
    std::string& error) {
    error.clear();
    if (!raster_attachments_ready_ || hdr_.image == VK_NULL_HANDLE ||
        frame.command_buffer == VK_NULL_HANDLE) {
        error = "HDR test target is unavailable";
        return false;
    }
    VkClearValue clear{};
    clear.color = {{color.x, color.y, color.z, 1.0f}};
    VkRenderingAttachmentInfo attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = hdr_.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue = clear;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {hdr_.extent.width, hdr_.extent.height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(frame.command_buffer, &rendering);
    vkCmdEndRendering(frame.command_buffer);
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
        frame.swapchain_image_view == VK_NULL_HANDLE ||
        frame.extent.width == 0 || frame.extent.height == 0) {
        error = "invalid acquired swapchain frame";
        return false;
    }
    if (frame.frame_slot >= frames_.size()) {
        error = "acquired frame slot has no display descriptor";
        return false;
    }
    if (display_pipeline_ == VK_NULL_HANDLE ||
        display_pipeline_layout_ == VK_NULL_HANDLE ||
        display_pipeline_format_ != frame.swapchain_format) {
        error = "display pipeline is unavailable for the acquired swapchain format";
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
        gi_history_reset_pending_ = true;
    };
    if (selected_dlss_mode_ == matter::DlssMode::Native) {
        matter::DlssEvaluationOutput ignored_output{};
        matter::DlssConstants ignored_constants{};
        matter::DlssResources ignored_resources{};
        std::string transition_error;
        FrameResources& dlss_slot_native = frames_[frame.frame_slot];
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDlss, false,
                            dlss_slot_native);
        (void)dlss_bridge_->evaluate_dlss(
            frame.command_buffer, temporal_frame_.attempt_token,
            {matter::DlssMode::Native, frame.extent, true, true},
            ignored_constants, ignored_resources, ignored_output,
            transition_error);
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDlss, true,
                            dlss_slot_native);
        consume_bridge_reset();
    }
    if (selected_dlss_mode_ != matter::DlssMode::Native &&
        dlss_bridge_->supports_dlss_mode(selected_dlss_mode_) &&
        frame.frame_slot < frames_.size()) {
        FrameResources& slot = frames_[frame.frame_slot];
        if (!ensure_dlss_output(slot, frame.extent, error)) return false;
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
            -1.0f / static_cast<float>(raster_extent_.width),
            -1.0f / static_cast<float>(raster_extent_.height)};
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
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDlss, false, slot);
        if (dlss_bridge_->evaluate_dlss(
                frame.command_buffer, temporal_frame_.attempt_token,
                {selected_dlss_mode_, frame.extent, true, true}, constants,
                resources, evaluation_output, evaluation_error)) {
            write_gpu_timestamp(frame.command_buffer, kGpuZoneDlss, true, slot);
            composite_source = &slot.dlss_output;
            slot.dlss_output.layout = evaluation_output.layout;
            composite_source_stage = evaluation_output.stage;
            composite_source_access = evaluation_output.access;
        } else {
            write_gpu_timestamp(frame.command_buffer, kGpuZoneDlss, true, slot);
            composite_source_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            composite_source_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            consume_bridge_reset();
        }
    }
    if (!vulkan_->retain_for_frame(frame, {composite_source->lifetime}, error))
        return false;
    matter::record_image_transition(
        frame.command_buffer, *composite_source,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, composite_source_stage,
        composite_source_access, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    FrameResources& frame_slot = frames_[frame.frame_slot];
    update_display_descriptor(frame_slot.display_descriptor_set,
                              composite_source->view);

    VkClearValue clear{};
    VkRenderingAttachmentInfo attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = frame.swapchain_image_view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue = clear;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = frame.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    write_gpu_timestamp(frame.command_buffer, kGpuZoneComposite, false,
                        frame_slot);
    vkCmdBeginRendering(frame.command_buffer, &rendering);
    VkViewport viewport{0.0f, 0.0f,
                        static_cast<float>(frame.extent.width),
                        static_cast<float>(frame.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, frame.extent};
    vkCmdSetViewport(frame.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      display_pipeline_);
    vkCmdBindDescriptorSets(frame.command_buffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            display_pipeline_layout_, 0, 1,
                            &frame_slot.display_descriptor_set, 0, nullptr);
    vkCmdPushConstants(frame.command_buffer, display_pipeline_layout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float),
                       &display_exposure_ev_);
    vkCmdDraw(frame.command_buffer, 3, 1, 0, 0);
    vkCmdEndRendering(frame.command_buffer);
    write_gpu_timestamp(frame.command_buffer, kGpuZoneComposite, true,
                        frame_slot);
    // End the 'total' zone and mark timestamps valid for readback next frame.
    if (gpu_timers_supported_ && frame.frame_slot < frames_.size()) {
        FrameResources& slot = frames_[frame.frame_slot];
        if (slot.ts_pool != VK_NULL_HANDLE &&
            (slot.ts_written[kGpuZoneTotal] & 1u)) {
            write_ts(frame.command_buffer, slot.ts_pool, kGpuZoneTotal, true);
            slot.ts_written[kGpuZoneTotal] |= 2u;
            slot.ts_valid = true;
        }
    }
    return true;
}

VkRasterAttachments VkSceneRenderer::raster_attachments() const {
    if (poisoned() || !raster_attachments_ready_) return {};
    return {{albedo_.image, albedo_.format},
            {normal_.image, normal_.format},
            {orm_.image, orm_.format},
            {velocity_.image, velocity_.format},
            {material_instance_.image, material_instance_.format},
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
    pixel.visibility = {1.0f, 1.0f, 1.0f};
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
    constexpr VkDeviceSize readback_size = 88;
    if (!matter::create_buffer(
            *vulkan_, readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            staging, error)) {
        return false;
    }
    matter::VkImageResource& accumulated_diffuse =
        gi_filtered_valid_ ? gi_atrous_[gi_filtered_index_] : raw_diffuse_;
    matter::VkImageResource& accumulated_specular =
        gi_filtered_valid_ ? gi_spec_atrous_[gi_filtered_index_]
                           : raw_specular_;
    RasterReadbackRecord record{{&albedo_, &normal_, &orm_, &velocity_, &depth_,
                                 &hdr_, &visibility_, &material_instance_,
                                 &raw_diffuse_,
                                 &accumulated_diffuse, &raw_specular_,
                                 &accumulated_specular},
                                {VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT},
                                staging.buffer,
                                x,
                                y,
                                std::min(raw_diffuse_extent_.width - 1,
                                         x * raw_diffuse_extent_.width /
                                             raster_extent_.width),
                                std::min(raw_diffuse_extent_.height - 1,
                                         y * raw_diffuse_extent_.height /
                                             raster_extent_.height)};
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        depth_.lifetime, hdr_.lifetime, visibility_.lifetime,
        material_instance_.lifetime, raw_diffuse_.lifetime,
        accumulated_diffuse.lifetime,
        raw_specular_.lifetime, accumulated_specular.lifetime,
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
    uint16_t visibility_half[4]{};
    std::memcpy(visibility_half, bytes.data() + 40,
                sizeof(visibility_half));
    pixel.visibility = {half_to_float(visibility_half[0]),
                        half_to_float(visibility_half[1]),
                        half_to_float(visibility_half[2])};
    std::memcpy(&pixel.material_index, bytes.data() + 48,
                sizeof(pixel.material_index));
    std::memcpy(&pixel.instance_token, bytes.data() + 52,
                sizeof(pixel.instance_token));
    uint16_t raw_half[4]{};
    std::memcpy(raw_half, bytes.data() + 56, sizeof(raw_half));
    pixel.raw_diffuse = {half_to_float(raw_half[0]), half_to_float(raw_half[1]),
                         half_to_float(raw_half[2]), half_to_float(raw_half[3])};
    uint16_t accumulated_half[4]{};
    std::memcpy(accumulated_half, bytes.data() + 64,
                sizeof(accumulated_half));
    pixel.accumulated_diffuse = {
        half_to_float(accumulated_half[0]),
        half_to_float(accumulated_half[1]),
        half_to_float(accumulated_half[2]),
        half_to_float(accumulated_half[3])};
    uint16_t raw_specular_half[4]{};
    std::memcpy(raw_specular_half, bytes.data() + 72,
                sizeof(raw_specular_half));
    pixel.raw_specular = {
        half_to_float(raw_specular_half[0]),
        half_to_float(raw_specular_half[1]),
        half_to_float(raw_specular_half[2]),
        half_to_float(raw_specular_half[3])};
    uint16_t accumulated_specular_half[4]{};
    std::memcpy(accumulated_specular_half, bytes.data() + 80,
                sizeof(accumulated_specular_half));
    pixel.accumulated_specular = {
        half_to_float(accumulated_specular_half[0]),
        half_to_float(accumulated_specular_half[1]),
        half_to_float(accumulated_specular_half[2]),
        half_to_float(accumulated_specular_half[3])};
    return true;
}

bool VkSceneRenderer::readback_materials(
    std::vector<MaterialGpuRecord>& records, std::string& error) {
    error.clear();
    records.clear();
    if (fail_if_poisoned(error)) return false;
    if (material_staging_.empty() || frames_.empty() ||
        frames_[active_frame_index_].materials.buffer == VK_NULL_HANDLE) {
        error = "Vulkan material buffer is unavailable before frame preparation";
        return false;
    }
    records.resize(material_staging_.size());
    return matter::readback_buffer(
        *vulkan_, frames_[active_frame_index_].materials, records.data(),
        records.size() * sizeof(MaterialGpuRecord), 0, error);
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
        material_instance_.reset();
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
