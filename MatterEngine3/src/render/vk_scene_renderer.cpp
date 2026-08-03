#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vk_scene_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>   // static_upload_census() backing counters
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

#include "gpu_matrix_pack.h"
#include "matrix_math.h"
#include "vk_build_profile.h"
#include "matter/vulkan_device.h"
#include "matter/vt_budgets.h"
#include "matter/world_definition.h"
#include "matter/world_session.h"
#include "shaders_gen/embedded_spirv.h"
#include "bc_encode.h"
#include "streamline_bridge.h"
#include "tileset_gtex.h"
#include "tileset_slicer.h"
#include "vk_volumetrics.h"
#include "tileset_bake_vk.h"

namespace viewer {
namespace {

// ---- per-frame build-cost kill switches ---------------------------------
// Each guards one CPU-time optimisation in the build region and defaults to
// ON. Read exactly once into a function-local static; never per frame and
// never per instance. None of them changes what is drawn -- they select
// between two implementations of the same result -- so a flip is an A/B of
// cost only. See docs/sector-bake-time-findings-2026-07-30.md.
bool env_flag_on(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0' || value[0] != '0';
}

// Flat mirror of slot_of_ for the per-instance part lookup.
bool slot_index_enabled() {
    static const bool value = env_flag_on("MATTER_VK_SLOT_INDEX");
    return value;
}

// Version-counter form of update_instances()' slot_of_/parts_ snapshot.
bool snapshot_version_enabled() {
    static const bool value = env_flag_on("MATTER_VK_SNAPSHOT_VERSION");
    return value;
}

// Fused compare-and-copy in set_temporal_frame().
bool temporal_copy_fuse_enabled() {
    static const bool value = env_flag_on("MATTER_VK_TEMPORAL_COPY");
    return value;
}

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
// 88 = the historical 72-byte layout + the VT Phase 2 warp block (warp_uv
// 8 B, warp_tangent 4 B, warp_scales 4 B) appended so every pre-existing
// word offset survives. rt_surface_common.glsl's stride guard and manual
// word-offset decode pair with this — change one, change both.
static_assert(sizeof(VkRasterVertex) == 88,
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
    VkPipeline skinned_raster_pipeline;
    VkPipelineLayout raster_layout;
    VkDescriptorSet raster_sets[2];
    VkPipeline composite_pipeline;
    VkPipelineLayout composite_layout;
    VkDescriptorSet composite_set;
    VkBuffer vertex_buffer;
    VkBuffer index_buffer;
    VkBuffer indirect_buffer;
    const DrawCommand* static_commands;
    uint32_t static_command_count;
    uint32_t index_count;
    const VkBuffer* skin_vertex_buffers;
    const VkBuffer* skin_previous_vertex_buffers;
    const uint32_t* skin_vertex_counts;
    uint32_t skin_buffer_count;
    const VkSkinRasterDraw* skin_draws;
    uint32_t skin_draw_count;
    uint32_t draw_transform_slots;
    // First slot of the skin transform tail (see skin_transform_base_).
    uint32_t skin_transform_base;
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
    VkVolumetrics* volumetrics = nullptr;
    uint32_t frame_slot = 0;
    float frame_time = 0.0f;
    uint32_t volumetrics_zone = 0;
    matter::VkAccelerationStructureResource* tlas = nullptr;
    // WP-E: owner for the VT frame hooks only. Deliberately NOT `renderer`:
    // that field also gates the ray-traced-shadow branch, which dereferences
    // `frame` -- null on the legacy immediate path -- so reusing it to reach
    // the VT hooks turns the smoke suite into a null deref.
    VkSceneRenderer* vt_hooks = nullptr;
    uint32_t vt_zone = 0;
};

void record_raster(VkCommandBuffer command_buffer, void* user_data) {
    auto& record = *static_cast<RasterRecord*>(user_data);
    const auto valid_skin_draw = [&record](const VkSkinRasterDraw& draw) {
        if (draw.output_frame_slot >= record.skin_buffer_count ||
            record.skin_vertex_buffers == nullptr ||
            record.skin_previous_vertex_buffers == nullptr ||
            record.skin_vertex_counts == nullptr ||
            record.skin_vertex_buffers[draw.output_frame_slot] == VK_NULL_HANDLE ||
            record.skin_previous_vertex_buffers[draw.output_frame_slot] == VK_NULL_HANDLE)
            return false;
        const uint32_t vertex_count =
            record.skin_vertex_counts[draw.output_frame_slot];
        return draw.index_count != 0 && draw.index_count % 3u == 0u &&
               draw.first_index <= record.index_count &&
               draw.index_count <= record.index_count - draw.first_index &&
               draw.output_vertex < vertex_count && draw.vertex_count != 0 &&
               draw.vertex_count <= vertex_count - draw.output_vertex &&
               record.skin_transform_base <= record.draw_transform_slots &&
               draw.instance_slot < record.draw_transform_slots -
                                        record.skin_transform_base &&
               draw.output_vertex <= static_cast<uint32_t>(INT32_MAX) &&
               draw.source_vertex <= static_cast<uint32_t>(INT32_MAX);
    };
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
    // Reversed-Z: far plane maps to NDC depth 0, so the "no geometry yet"
    // clear value is 0.0 (was 1.0 under standard-Z).
    depth_attachment.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = record.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 5;
    rendering.pColorAttachments = color_attachments;
    rendering.pDepthAttachment = &depth_attachment;
    // WP-E: VT pool/indirection uploads and the feedback clear are transfers,
    // so they must land before dynamic rendering begins. Timed as its own GPU
    // zone under the SAME gate as the G-buffer zone -- record.ts_pool is
    // non-null only on the production frame path, which is the only path that
    // resets the query pool.
    const bool time_vt = record.ts_pool != VK_NULL_HANDLE &&
                         record.ts_written != nullptr && record.vt_hooks;
    if (time_vt) {
        write_ts(command_buffer, record.ts_pool, record.vt_zone, false);
        record.ts_written[record.vt_zone] |= 1u;
    }
    if (record.vt_hooks) record.vt_hooks->vt_record_pre_pass(command_buffer);
    if (time_vt) {
        write_ts(command_buffer, record.ts_pool, record.vt_zone, true);
        record.ts_written[record.vt_zone] |= 2u;
    }
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
    vkCmdBindIndexBuffer(command_buffer, record.index_buffer, 0,
                         VK_INDEX_TYPE_UINT32);
    for (uint32_t i = 0; i < record.draw_range_count; ++i) {
        const PartCommandRange& range = record.draw_ranges[i];
        if (range.first_command > record.static_command_count ||
            range.command_count > record.static_command_count - range.first_command)
            continue;
        // Skin-raster instances are removed individually by cull.comp before
        // this command's instance_count is finalized, so every command in the
        // range is recorded unconditionally: that preserves bind fallbacks and
        // ordinary instances which share the same immutable mesh range.
        //
        // Because nothing is skipped per command any more, the range is issued
        // as one multi-draw rather than one call per cluster/LOD slot. It was
        // temporarily split into drawCount=1 calls when this loop still had to
        // drop individual commands replaced by compute-skinned output; that
        // filter moved into cull.comp (uses_skin_raster) and the grouping was
        // never restored, leaving kVkMaxLod draw calls per part where one does.
        // drawCount stays clamped to maxDrawIndirectCount; max(1) only keeps a
        // degenerate limit from spinning here -- the caller already refuses to
        // record when the device reports less than one.
        const uint32_t max_per_call =
            std::max(1u, record.max_draw_indirect_count);
        uint32_t remaining = range.command_count;
        uint32_t first = range.first_command;
        while (remaining != 0) {
            const uint32_t count = std::min(remaining, max_per_call);
            vkCmdDrawIndexedIndirect(command_buffer, record.indirect_buffer,
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
    // Accepted skin work is drawn from the per-frame compute output, never
    // from the immutable bind-pose arena.  Each draw carries the exact
    // visibility-selected index span plus the source/output rebase.  Every
    // field is range-checked below, so a stale mapping cannot turn into an
    // out-of-bounds vertex fetch.
    if (record.skinned_raster_pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          record.skinned_raster_pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                record.raster_layout, 0, 2,
                                record.raster_sets, 0, nullptr);
        for (uint32_t draw_index = 0; draw_index < record.skin_draw_count;
            ++draw_index) {
            const VkSkinRasterDraw& draw = record.skin_draws[draw_index];
            if (!valid_skin_draw(draw)) {
                continue;  // fail closed to the already-recorded static path
            }
            const VkDeviceSize offset = static_cast<VkDeviceSize>(
                draw.output_vertex) * sizeof(VkSkinVertex);
            // A retained pose is spatially stable for this presentation
            // frame. Bind its current slice as both streams so repeatedly
            // reusing it cannot replay the source frame's old velocity.
            const bool retained =
                draw.output_frame_slot != record.frame_slot;
            const VkBuffer skin_buffers[] = {
                record.skin_vertex_buffers[draw.output_frame_slot],
                retained
                    ? record.skin_vertex_buffers[draw.output_frame_slot]
                    : record.skin_previous_vertex_buffers[draw.output_frame_slot]};
            const VkDeviceSize skin_offsets[] = {offset, offset};
            vkCmdBindVertexBuffers(command_buffer, 0, 2, skin_buffers,
                                   skin_offsets);
            // Index VALUES in the shared buffer are PART-LOCAL (see
            // matter_engine.cpp, which rebases each mesh by its offset within
            // the part; vk_scene_renderer never rewrites them). The skin
            // buffer is already bound at output_vertex, so the draw must
            // subtract exactly this range's part-local base -- no more.
            //
            // The previous form, output_vertex - source_vertex, was wrong
            // twice over: it treated the values as renderer-global (so it
            // over-rebased by the part's arena base) and it re-applied
            // output_vertex on top of the bind offset. Both errors vanish
            // when the part sits at arena base 0 as the only submission,
            // which is exactly what every fixture arranged.
            const int64_t rebase = -static_cast<int64_t>(draw.local_vertex_base);
            if (rebase < INT32_MIN || rebase > INT32_MAX) continue;
            // The dynamic slot indexes the SKIN TAIL. Passing it raw put the
            // draw in cull.comp's bucket space -- slot 0 there is the gallery
            // world's Crate floor slab, whose scale(4, 0.1, 4) squashed the
            // creature flat.
            vkCmdDrawIndexed(command_buffer, draw.index_count, 1,
                             draw.first_index, static_cast<int32_t>(rebase),
                             record.skin_transform_base + draw.instance_slot);
        }
    }
    vkCmdEndRendering(command_buffer);
    // WP-E: copy the 1/8-res feedback target into this frame slot's readback
    // buffer; it is consumed at the next begin_frame on the same slot.
    if (record.vt_hooks) record.vt_hooks->vt_record_post_pass(command_buffer);
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

    // --- Volumetrics pass: froxel density + scatter + integrate ---
    const bool volumetrics_ready =
        record.volumetrics && record.volumetrics->active() &&
        record.tlas && record.tlas->handle != VK_NULL_HANDLE &&
        (!record.renderer || record.renderer->rt_effective_observed());
    if (volumetrics_ready) {
        if (record.ts_pool != VK_NULL_HANDLE && record.ts_written) {
            write_ts(command_buffer, record.ts_pool, record.volumetrics_zone, false);
            record.ts_written[record.volumetrics_zone] |= 1u;
        }
        std::string vol_error;
        record.volumetrics->record(
            command_buffer, record.frame_slot,
            *record.depth, record.tlas->handle, *record.matrices,
            record.frame_time, vol_error);
        if (record.ts_pool != VK_NULL_HANDLE && record.ts_written) {
            write_ts(command_buffer, record.ts_pool, record.volumetrics_zone, true);
            record.ts_written[record.volumetrics_zone] |= 2u;
        }
    } else {
        // A streaming scene can be empty while its first sectors bake, or can
        // retain an invalidated TLAS handle after a world switch. Do not sample
        // stale integrated volume data or bind that TLAS for ray queries.
        record.lighting.vol_enabled = 0.0f;
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
    matter::VkImageResource* images[15];
    VkImageAspectFlags aspects[15];
    VkBuffer destination;
    uint32_t x;
    uint32_t y;
    uint32_t raw_x;
    uint32_t raw_y;
};

void record_raster_readback(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<RasterReadbackRecord*>(user_data);
    // Each offset is aligned to its format's texel-block size (4 or 8 bytes).
    constexpr VkDeviceSize offsets[15] = {0, 8, 16, 20, 24, 32,
                                          40, 48, 56, 64, 72, 80,
                                          88, 96, 104};
    for (size_t i = 0; i < 15; ++i) {
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
    return max_per_stage >= 6 && max_per_set >= 7;
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

bool VkSceneRenderer::register_animation_skin_asset(
    uint64_t asset_key, const std::vector<VkSkinInfluence>& influences) {
    return animation_skinning_.register_asset(asset_key, influences);
}

bool VkSceneRenderer::begin_animation_skinning_frame(
    uint32_t frame_slot, uint64_t completed_fence) {
    return animation_skinning_.begin_frame(frame_slot, completed_fence);
}

bool VkSceneRenderer::submit_visible_animation_skinning(
    uint32_t frame_slot, const std::vector<VkSkinSubmission>& visible,
    const FrameMatrices& matrices, matter::Float3 camera_eye,
    float pixel_budget,
    const std::vector<VkAnimationBoundsInstance>& rejected_bounds) {
    // The bridge can reject before it has a VkSkinSubmission.  Its explicit
    // full generational scope is consumed before the empty queue is
    // published, so culling cannot retain a prior animated record.
    animation_bounds_.fail_open_instances(rejected_bounds);
    // Publish this frame's conservative current/previous animated bounds
    // before choosing skin work.  The queue planner below reads this exact
    // payload, and the later GPU cull uploads the same records.
    std::set<std::pair<uint32_t, uint32_t>> bounds_published;
    for (const VkSkinSubmission& submission : visible) {
        const auto key = std::make_pair(submission.instance_slot,
                                        submission.instance_generation);
        if (bounds_published.insert(key).second) {
            (void)animation_bounds_.update_instance(
                submission.instance_slot, submission.instance_generation,
                submission.asset_key, submission.pose,
                submission.history_valid);
        }
    }
    // Publish the work queue first: a rejected queue must not advance a
    // dynamic bound independently of the pose it claims to represent. Bounds
    // assets intentionally share the immutable skin asset key, while callers
    // without serialized bounds retain the static path unchanged.
    // Resolve the same current-frame conservative animated bounds, frustum,
    // and cluster LOD that the cull pass will consume. Presentation LOD is a
    // pose-rate policy only: it must not pick a mesh LOD or keep a stale
    // visible work item alive.  The bridge supplies candidates for every
    // baked LOD; this compacted vector contains exactly the one selected
    // candidate per current visible cluster.
    float frustum_planes[6][4]{};
    const bool have_frustum =
        extract_frustum_planes_zo(matrices.world_to_clip, frustum_planes);
    const auto unpack_matrix = [](const GpuMat4& packed) {
        matter::Mat4f result{};
        for (uint32_t row = 0; row != 4; ++row)
            for (uint32_t column = 0; column != 4; ++column)
                result.m[row * 4u + column] =
                    packed.elements[column * 4u + row];
        return result;
    };
    const auto culled = [&frustum_planes, have_frustum, &unpack_matrix](
                            const VkAnimationBoundsGpuRecord& bounds,
                            const GpuInstance& instance) {
        if (!have_frustum) return false;  // fail open on an invalid camera.
        const matter::Mat4f object_to_world = unpack_matrix(instance.object_to_world);
        for (uint32_t plane = 0; plane != 6; ++plane) {
            bool outside = true;
            for (uint32_t x = 0; x != 2; ++x)
                for (uint32_t y = 0; y != 2; ++y)
                    for (uint32_t z = 0; z != 2; ++z) {
                        const matter::Float3 world = transform_point(
                            object_to_world,
                            {x == 0 ? bounds.aabb_min[0] : bounds.aabb_max[0],
                             y == 0 ? bounds.aabb_min[1] : bounds.aabb_max[1],
                             z == 0 ? bounds.aabb_min[2] : bounds.aabb_max[2]});
                        if (frustum_planes[plane][0] * world.x +
                                frustum_planes[plane][1] * world.y +
                                frustum_planes[plane][2] * world.z +
                                frustum_planes[plane][3] >= 0.0f) {
                            outside = false;
                            break;
                        }
                    }
            if (outside) return true;
        }
        return false;
    };
    std::vector<VkSkinSubmission> compacted;
    compacted.reserve(visible.size());
    std::set<uint64_t> current_visible_instances;
    const std::vector<VkAnimationBoundsGpuRecord> current_bounds =
        animation_bounds_.gpu_records();
    // MATTER_SKIN_PROBE census: which (cluster, lod) submissions survive
    // compaction. A cluster dropped here keeps drawing its static bind pose
    // while its peers animate, so a partly-animating mesh shows up as a
    // cluster present in `visible` and absent from `compacted`.
    static const bool probe = [] {
        const char* value = std::getenv("MATTER_SKIN_PROBE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    std::string census;
    for (const VkSkinSubmission& submitted : visible) {
        VkSkinSubmission candidate = submitted;
        candidate.current_frustum_visible = false;
        if (candidate.instance_slot >= dynamic_instance_staging_.size() ||
            candidate.instance_slot >= dynamic_instance_part_slots_.size() ||
            dynamic_instance_part_slots_[candidate.instance_slot] == UINT32_MAX) {
            if (probe) census += " c" + std::to_string(candidate.cluster) + "/l" + std::to_string(candidate.lod) + "=slot";
            continue;
        }
        const GpuInstance& instance =
            dynamic_instance_staging_[candidate.instance_slot];
        if (instance.animation_instance_generation !=
                candidate.instance_generation ||
            instance.part_slot >= parts_.size()) {
            if (probe) census += " c" + std::to_string(candidate.cluster) + "/l" + std::to_string(candidate.lod) + "=gen";
            continue;
        }
        const PartRecord& part = parts_[instance.part_slot];
        if (!part.live || candidate.cluster >= part.cluster_count ||
            part.cluster_start > cluster_staging_.size() ||
            candidate.cluster >= cluster_staging_.size() - part.cluster_start) {
            if (probe) census += " c" + std::to_string(candidate.cluster) + "/l" + std::to_string(candidate.lod) + "=part";
            continue;
        }
        const GpuCluster& cluster =
            cluster_staging_[part.cluster_start + candidate.cluster];
        VkAnimationBoundsAabb planning_bounds{
            {cluster.aabb_min[0], cluster.aabb_min[1], cluster.aabb_min[2]},
            {cluster.aabb_max[0], cluster.aabb_max[1], cluster.aabb_max[2]}};
        const bool dynamic_planning_bound = resolve_animation_cluster_union(
            current_bounds, candidate.instance_slot,
            candidate.instance_generation, candidate.cluster,
            planning_bounds);
        const float planning_dx =
            planning_bounds.max[0] - planning_bounds.min[0];
        const float planning_dy =
            planning_bounds.max[1] - planning_bounds.min[1];
        const float planning_dz =
            planning_bounds.max[2] - planning_bounds.min[2];
        const float planning_radius = dynamic_planning_bound
            ? 0.5f * std::sqrt(planning_dx * planning_dx +
                               planning_dy * planning_dy +
                               planning_dz * planning_dz)
            : cluster.radius;
        const uint32_t selected_lod = vk_scene_detail::select_cluster_lod_view(
            {planning_bounds.min[0], planning_bounds.min[1],
             planning_bounds.min[2]},
            {planning_bounds.max[0], planning_bounds.max[1],
             planning_bounds.max[2]},
            planning_radius, cluster.thresholds, cluster.lod_count,
            unpack_matrix(instance.object_to_world), camera_eye, pixel_budget);
        if (candidate.lod != selected_lod) {
            if (probe)
                census += " c" + std::to_string(candidate.cluster) + "/l" +
                          std::to_string(candidate.lod) + "=lod" +
                          std::to_string(selected_lod);
            continue;
        }
        const auto bounds = std::find_if(
            current_bounds.begin(), current_bounds.end(), [&candidate](
                const VkAnimationBoundsGpuRecord& value) {
                return value.instance_slot == candidate.instance_slot &&
                       value.instance_generation ==
                           candidate.instance_generation &&
                       value.cluster_index == candidate.cluster &&
                       value.lod == candidate.lod;
            });
        // Missing dynamic bounds cannot reject an animated owner. It remains
        // in the queue and cull's conservative/static lane decides it.
        candidate.current_frustum_visible =
            bounds == current_bounds.end() || !culled(*bounds, instance);
        if (candidate.current_frustum_visible) {
            const uint64_t visibility_key =
                (uint64_t(candidate.instance_slot) << 32u) |
                candidate.instance_generation;
            candidate.history_valid = candidate.history_valid &&
                                      visible_skin_instances_.count(
                                          visibility_key) != 0;
            current_visible_instances.insert(visibility_key);
        }
        if (probe)
            census += " c" + std::to_string(candidate.cluster) + "/l" +
                      std::to_string(candidate.lod) +
                      (candidate.current_frustum_visible ? "=ok" : "=offscreen");
        compacted.push_back(std::move(candidate));
    }
    if (probe) {
        static std::string last_census;
        if (census != last_census) {
            last_census = census;
            fprintf(stderr, "[skin-census] in=%zu out=%zu%s\n",
                    visible.size(), compacted.size(), census.c_str());
        }
    }
    pending_visible_skin_instances_ = std::move(current_visible_instances);
    pending_skin_visibility_frame_slot_ = frame_slot;
    if (!animation_skinning_.submit_visible(frame_slot, compacted)) {
        // The queue has fallen back before a complete pose can be consumed.
        // Clear each matching dynamic record before publishing its
        // conservative asset bound with occlusion disabled.
        std::vector<VkAnimationBoundsInstance> queue_rejected;
        queue_rejected.reserve(visible.size());
        for (const VkSkinSubmission& submission : compacted)
            queue_rejected.push_back({submission.instance_slot,
                                      submission.instance_generation,
                                      submission.asset_key});
        animation_bounds_.fail_open_instances(queue_rejected);
        consumed_animation_skin_fallbacks_ =
            animation_skinning_.frame(frame_slot).fallbacks;
        return false;
    }
    const VkSkinFrameArenas& staged = animation_skinning_.frame(frame_slot);
    consumed_animation_skin_fallbacks_ = staged.fallbacks;
    for (const VkSkinSubmission& submission : compacted) {
        const bool accepted = std::any_of(
            staged.work_items.begin(), staged.work_items.end(),
            [&submission](const VkSkinWorkItem& work) {
                return work.instance_slot == submission.instance_slot &&
                       vk_skin_work_lod(work.flags) == submission.lod &&
                       vk_skin_work_cluster(work.flags) == submission.cluster;
            });
        if (accepted) {
            (void)animation_bounds_.update_instance(
                submission.instance_slot, submission.instance_generation,
                submission.asset_key, submission.pose,
                submission.history_valid);
            continue;
        }
        const auto fallback = std::find_if(
            staged.fallbacks.begin(), staged.fallbacks.end(),
            [&submission](const VkSkinFallback& value) {
                return value.instance_slot == submission.instance_slot &&
                       value.instance_generation ==
                           submission.instance_generation;
            });
        // LastCompletePose deliberately retains its matching last-complete
        // dynamic bound. BindPose retires dynamic deformation and routes
        // culling/raster through the conservative immutable asset path.
        if (fallback != staged.fallbacks.end() &&
            fallback->mode == VkSkinFallbackMode::BindPose) {
            animation_bounds_.fail_open_instances(
                {{submission.instance_slot, submission.instance_generation,
                  submission.asset_key}});
        }
    }
    return true;
}

bool VkSceneRenderer::finish_animation_skinning_frame(uint32_t frame_slot,
                                                       uint64_t fence) {
    if (!animation_skinning_.mark_submitted(frame_slot, fence)) return false;
    if (pending_skin_visibility_frame_slot_ == frame_slot) {
        visible_skin_instances_ = std::move(pending_visible_skin_instances_);
        pending_visible_skin_instances_.clear();
        pending_skin_visibility_frame_slot_ = UINT32_MAX;
    }
    return true;
}

bool VkSceneRenderer::skinned_rt_uses_bind_pose_blas() const noexcept {
    const auto& contract = matter::animation::skinned_rt_build_contract();
    return contract.build_once && !contract.allow_update && !contract.allow_refit;
}

bool VkSceneRenderer::register_animation_bounds_asset(
    const VkAnimationBoundsAsset& asset) {
    return animation_bounds_.register_asset(asset);
}

bool VkSceneRenderer::update_animation_bounds(uint32_t instance_slot,
                                              uint32_t instance_generation,
                                              uint64_t asset_key,
                                              const VkSkinPose& pose,
                                              bool history_valid) {
    return animation_bounds_.update_instance(instance_slot, instance_generation,
                                             asset_key, pose,
                                             history_valid);
}

bool VkSceneRenderer::unregister_animation_bounds_asset(uint64_t asset_key) {
    return animation_bounds_.unregister_asset(asset_key);
}

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
    if (volumetrics_) {
        volumetrics_->destroy();
        volumetrics_.reset();
    }
    const VkDevice device = vulkan_->device();
    rt_sbt_.reset();
    visibility_.reset();
    raw_diffuse_.reset();
    raw_specular_.reset();
    raw_specular_aux_.reset();
    raw_transmission_.reset();
    raw_transmission_aux_.reset();
    vol_dummy_3d_.reset();
    for (auto& image : gi_atrous_) image.reset();
    for (auto& image : gi_spec_atrous_) image.reset();
    for (auto& image : gi_trans_atrous_) image.reset();
    for (auto* histories : {&gi_history_, &gi_spec_history_,
                            &gi_trans_history_}) {
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
    if (vol_linear_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, vol_linear_sampler_, nullptr);
    // Phase 1 tileset Vulkan port (Task 6): tear down slot images, dummies,
    // sampler, and the params UBO (matter::VkBufferResource cleans itself up
    // via reset()).
    for (auto& slot : tileset_slots_) {
        for (auto& channel : slot.channels) destroy_tileset_image(channel);
        slot = TilesetSlotGpu{};
    }
    destroy_tileset_image(tileset_dummy_rgba8_);
    destroy_tileset_image(tileset_dummy_rg8_);
    destroy_tileset_image(tileset_dummy_r16_);
    if (tileset_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, tileset_sampler_, nullptr);
    tileset_sampler_ = VK_NULL_HANDLE;
    tileset_params_.reset();
    tileset_infra_ready_ = false;
    // WP-E: the residency runtime owns raw Vulkan handles, so it must go down
    // with the pipeline (its shutdown() is idempotent).
    if (vt_) {
        vt_->shutdown();
        vt_.reset();
    }
    vt_compositor_ = nullptr;   // borrowed; the residency layer owned it
    vt_enricher_ = nullptr;     // ditto (WP-H)
    vt_pending_invalidate_.clear();
    vt_inputs_dirty_ = true;
    vt_inputs_pushed_ = false;
    vt_init_attempted_ = false;
    vt_unavailable_ = false;
    vt_unavailable_reason_.clear();
    destroy_tileset_image(vt_dummy_feedback_);
    vt_dummy_storage_.reset();
    vt_dummies_ready_ = false;
    vt_draw_slot_table_.clear();
    vt_draw_slots_dirty_ = true;
    part_draw_override_entries_.clear();
    part_draw_override_table_.clear();
    part_draw_overrides_dirty_ = true;
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
    if (skinned_raster_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, skinned_raster_pipeline_, nullptr);
    if (skin_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device, skin_pipeline_, nullptr);
    if (skin_pipeline_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, skin_pipeline_layout_, nullptr);
    if (skin_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, skin_set_layout_, nullptr);
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
    skinned_raster_pipeline_ = VK_NULL_HANDLE;
    skin_pipeline_ = VK_NULL_HANDLE;
    skin_pipeline_layout_ = VK_NULL_HANDLE;
    skin_set_layout_ = VK_NULL_HANDLE;
    composite_set_layout_ = VK_NULL_HANDLE;
    composite_pipeline_layout_ = VK_NULL_HANDLE;
    composite_pipeline_ = VK_NULL_HANDLE;
    composite_sampler_ = VK_NULL_HANDLE;
    vol_linear_sampler_ = VK_NULL_HANDLE;
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
    // Phase 2 (tileset POM, Task 10) adds FRAGMENT_BIT: gbuffer.frag needs
    // FrameConstants.world_to_clip (project the marched world position for
    // the conservative depth write) and camera_eye_pixel_budget.xyz (view
    // ray origin for the march).
    const VkDescriptorSetLayoutBinding frame_binding =
        descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_COMPUTE_BIT |
                               VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayoutCreateInfo frame_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    frame_layout.bindingCount = 1;
    frame_layout.pBindings = &frame_binding;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &frame_layout, nullptr, &set_layouts_[0]);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(frame)", result, error);

    // Bindings 0-5: scene storage buffers (DrawTransforms at 3, Materials at
    // 5). Bindings 6-7 (Phase 1 tileset Vulkan port, Task 6): the 16-entry
    // ground tileset sampler2DArray and the TilesetParams UBO, sampled only
    // by gbuffer.frag.
    // Bindings 9-13 (WP-E, chart-space virtual texturing):
    //   9  vt_draw_slots  storage buffer, COMPUTE  (cull.comp -> DrawTransform)
    //   10 vt_pool[4]     combined image samplers, FRAGMENT
    //   11 vt_indirection storage buffer,          FRAGMENT (was an image
    //      array; the buffer indirection removed the 2048-layer format cap)
    //   12 vt_variants    storage buffer,          FRAGMENT
    //   13 vt_feedback    storage image,           FRAGMENT
    // Binding 14 (per-module draw overrides): a per-part_slot
    // {max_draw_distance, lod_bias} table, COMPUTE-only (cull.comp). Always
    // bound; a one-entry neutral table in the default state.
    std::array<VkDescriptorSetLayoutBinding, 15> scene_bindings{};
    for (uint32_t i = 0; i < 6; ++i)
        scene_bindings[i] =
            descriptor_binding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               vk_scene_detail::scene_binding_stage_flags(i));
    scene_bindings[6] = descriptor_binding(
        6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_FRAGMENT_BIT);
    // kMaxTilesetSlots slots * kTilesetChannelCount channels (Phase 2's
    // horizon-map lighting grew the per-slot channel count 4 -> 6; WP-B of
    // the chart-VT work grew the slot count 4 -> 8). 8 * 6 = 48, and
    // shaders_vk/tileset_common.glsl declares tilesetTex[48] to match.
    scene_bindings[6].descriptorCount =
        tileset::kMaxTilesetSlots * kTilesetChannelCount;
    scene_bindings[7] = descriptor_binding(
        7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    // C3 dynamic cluster AABBs. Static cluster metadata stays at binding 0;
    // cull.comp selects this optional per-frame override by instance slot.
    scene_bindings[8] = descriptor_binding(
        8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
    scene_bindings[9] = descriptor_binding(
        9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
    scene_bindings[10] = descriptor_binding(
        10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_FRAGMENT_BIT);
    scene_bindings[10].descriptorCount = vt::kVtChannelCount;
    scene_bindings[11] = descriptor_binding(
        11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    scene_bindings[12] = descriptor_binding(
        12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    scene_bindings[13] = descriptor_binding(
        13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
    scene_bindings[14] = descriptor_binding(
        14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
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

    // C2 owns a separate set because the skin buffers are intentionally not
    // visible to ordinary static cull/raster dispatches. Bindings are kept in
    // shader order: source, influence, current/previous palettes, work,
    // current/previous output.
    std::array<VkDescriptorSetLayoutBinding, 7> skin_bindings{};
    for (uint32_t binding = 0; binding != skin_bindings.size(); ++binding)
        skin_bindings[binding] = descriptor_binding(
            binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_SHADER_STAGE_COMPUTE_BIT);
    VkDescriptorSetLayoutCreateInfo skin_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    skin_layout_info.bindingCount = static_cast<uint32_t>(skin_bindings.size());
    skin_layout_info.pBindings = skin_bindings.data();
    result = vkCreateDescriptorSetLayout(device, &skin_layout_info, nullptr,
                                         &skin_set_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateDescriptorSetLayout(animation skin)", result, error);
    const VkDescriptorSetLayout skin_sets[] = {set_layouts_[0], set_layouts_[1],
                                               skin_set_layout_};
    const VkPushConstantRange skin_push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                        sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo skin_pipeline_layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    skin_pipeline_layout_info.setLayoutCount = 3;
    skin_pipeline_layout_info.pSetLayouts = skin_sets;
    skin_pipeline_layout_info.pushConstantRangeCount = 1;
    skin_pipeline_layout_info.pPushConstantRanges = &skin_push;
    result = vkCreatePipelineLayout(device, &skin_pipeline_layout_info, nullptr,
                                    &skin_pipeline_layout_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreatePipelineLayout(animation skin)", result, error);
    const matter::EmbeddedSpirvView skin_spirv =
        matter::find_spirv("animation_skin.comp.spv");
    if (!skin_spirv.words || skin_spirv.word_count == 0) {
        error = "embedded SPIR-V not found: animation_skin.comp.spv";
        return false;
    }
    VkShaderModule skin_shader = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo skin_shader_info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    skin_shader_info.codeSize = skin_spirv.word_count * sizeof(uint32_t);
    skin_shader_info.pCode = skin_spirv.words;
    result = vkCreateShaderModule(device, &skin_shader_info, nullptr, &skin_shader);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateShaderModule(animation skin)", result, error);
    VkComputePipelineCreateInfo skin_create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    skin_create.layout = skin_pipeline_layout_;
    skin_create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    skin_create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    skin_create.stage.module = skin_shader;
    skin_create.stage.pName = "main";
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &skin_create,
                                      nullptr, &skin_pipeline_);
    vkDestroyShaderModule(device, skin_shader, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateComputePipelines(animation skin)", result, error);

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
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    // The skin compute fixture does not exercise RT. Keeping it disabled for
    // that test avoids creating ray-query modules on a device deliberately
    // configured without the optional ray-query feature.
    if (test_force_rt_unavailable_) return true;
#endif
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
    VkDescriptorSetLayoutBinding bindings[] = {
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
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        // Phase 1 tileset Vulkan port (Task 6): mirrors raster set 1's
        // bindings 6/7. Stage flags must cover every stage whose SPIR-V can
        // statically reference the bindings: tileset_common.glsl is pulled in
        // by rt_surface_common.glsl, which is included by rt_lighting.rgen /
        // rt_surface_test.rgen (raygen), rt_surface.rchit (closest hit),
        // rt_visibility.rahit (any hit), and rt_radiance.rmiss (miss). glslang
        // keeps declared-but-uncalled helper functions (rt_tileset_sample) in
        // the module unless optimized, and SPIR-V 1.4+ lists all globals in
        // the entry-point interface, so be conservative and cover all four
        // stages — extra stage bits on a set layout are harmless.
        descriptor_binding(15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        // WP-G (chart VT in the RT path): 17/18/19 mirror raster set 1's VT
        // bindings 10/11/12 (pool array, indirection buffer, variant table).
        // Same all-four-stages reasoning as 15/16 above: vt_common.glsl is
        // pulled in by rt_surface_common.glsl, so every stage that includes it
        // lists these globals in its entry-point interface whether or not it
        // samples. Binding 13 (feedback) is NOT mirrored — rays never request
        // pages. 18 became a STORAGE_BUFFER with the buffer indirection (it
        // was the R16G16_UINT image array whose 2048-layer format cap forced
        // the redesign).
        descriptor_binding(17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        descriptor_binding(19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                               VK_SHADER_STAGE_MISS_BIT_KHR),
        // RT PBR Phase 1: transmission denoiser aux lane, the storage-image
        // sibling of binding 13 (raw_specular_aux).
        descriptor_binding(20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR)};
    bindings[15].descriptorCount =
        tileset::kMaxTilesetSlots * kTilesetChannelCount;
    bindings[17].descriptorCount = vt::kVtChannelCount;
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount =
        static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
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
                           "rt_surface.rchit.spv",
                           "rt_surface.rahit.spv"};
    const VkShaderStageFlagBits stages_bits[] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR};
    VkShaderModule modules[9]{};
    VkPipelineShaderStageCreateInfo stages[9]{};
    for (uint32_t i = 0; i < 9; ++i) {
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
    // RT PBR Phase 1: alpha-tested occluders in the refraction walk
    // (constant_id 1 in rt_lighting.rgen). Default ON: measured on the
    // rt-transmission smoke fixture the two-mask walk costs well under the
    // spec's ~5% RT-budget retreat threshold (see the fixture's gpu-zone
    // print), and correctness -- foliage no longer silhouettes refraction --
    // wins at that price. MATTER_RT_WALK_ALPHA_TEST=0 restores the legacy
    // single OpaqueEXT trace for A/B measurement.
    uint32_t walk_alpha_test = 1u;
    if (const char* walk_env = std::getenv("MATTER_RT_WALK_ALPHA_TEST"))
        walk_alpha_test = std::strtoul(walk_env, nullptr, 10) != 0 ? 1u : 0u;
    const uint32_t lighting_constants[2] = {count_lobe_samples,
                                            walk_alpha_test};
    const VkSpecializationMapEntry lighting_entries[2] = {
        {0, 0, sizeof(uint32_t)},
        {1, sizeof(uint32_t), sizeof(uint32_t)}};
    const VkSpecializationInfo lighting_specialization{
        2, lighting_entries, sizeof(lighting_constants), lighting_constants};
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
    // RT PBR Phase 1: the surface hit group gains an alpha-test any-hit.
    // Only rays traced WITHOUT gl_RayFlagsOpaqueEXT ever run it (today:
    // the refraction walk's non-opaque re-trace).
    groups[6].anyHitShader = 8;
    VkRayTracingPipelineCreateInfoKHR create{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    create.stageCount = 9;
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

    // GBuffer pipeline: 5-color MRT + depth write.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable  = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    // Reversed-Z: nearer geometry has a larger NDC depth, so passing requires
    // depth >= existing (was <= under standard-Z).
    depth_stencil.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;
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
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
        vkDestroyShaderModule(device, raster_fragment, nullptr);
        vkDestroyShaderModule(device, raster_vertex, nullptr);
        return fail_vk("vkCreateGraphicsPipelines(raster)", result, error);
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
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                       &raster_create, nullptr,
                                       &skinned_raster_pipeline_);
    vkDestroyShaderModule(device, skinned_raster_vertex, nullptr);
    vkDestroyShaderModule(device, raster_fragment, nullptr);
    vkDestroyShaderModule(device, raster_vertex, nullptr);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateGraphicsPipelines(skinned raster)", result,
                       error);

    std::array<VkDescriptorSetLayoutBinding, 11> sampled_bindings{};
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
    // binding 9: vol_integrated_texture (sampler3D), binding 10: depth_texture
    sampled_bindings[9] = descriptor_binding(
        9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_FRAGMENT_BIT);
    sampled_bindings[10] = descriptor_binding(
        10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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
    // Fullscreen pass: never cull. This previously shared the gbuffer's
    // rasterization state, which was harmless only while that state was
    // CULL_MODE_NONE — with scene backface culling enabled it would cull the
    // composite triangle itself and black out the frame.
    VkPipelineRasterizationStateCreateInfo fullscreen_rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    fullscreen_rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    fullscreen_rasterization.cullMode = VK_CULL_MODE_NONE;
    fullscreen_rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    fullscreen_rasterization.lineWidth = 1.0f;
    composite_create.pRasterizationState = &fullscreen_rasterization;
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
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateSampler(composite)", result, error);

    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    result = vkCreateSampler(device, &sampler, nullptr, &vol_linear_sampler_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateSampler(vol_linear)", result, error);

    // 1x1x1 placeholder 3D texture for the volumetric integrated binding
    // (composite.frag binding 9).  Replaced by the real froxel texture once
    // VkVolumetrics is wired up; until then vol_enabled stays 0.0 so the
    // shader never samples it.
    if (!matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT,
            {1, 1, 1},
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            vol_dummy_3d_, error)) {
        return false;
    }
    if (!matter::transition_image(
            *vulkan_, vol_dummy_3d_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, error)) {
        return false;
    }
    return true;
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
    // { exposure_ev, passthrough, srgb_output } -- see DisplaySettings in
    // display_transform.frag and kDisplayPush below.
    exposure_range.size = sizeof(float) * 3;
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

bool VkSceneRenderer::ensure_index_buffer(VkDeviceSize required_size,
                                          std::string& error,
                                          bool* replaced) {
    if (replaced) *replaced = false;
    required_size = std::max<VkDeviceSize>(required_size, 1);
    if (indices_.size >= required_size) return true;
    VkDeviceSize capacity = 0;
    if (!vk_scene_detail::checked_grown_capacity(
            indices_.size, required_size, limits_.max_buffer_size, capacity,
            "index buffer", error)) {
        return false;
    }
    matter::VkBufferResource replacement;
    if (!matter::create_buffer(
            *vulkan_, capacity,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            replacement, error)) {
        return false;
    }
    indices_ = std::move(replacement);
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
    // Phase 1 tileset Vulkan port (Task 6): raster set 1 gained binding 6
    // (kMaxTilesetSlots*kTilesetChannelCount combined-image-sampler
    // descriptors -- 48 = 8 slots x 6 channels) and binding 7 (1 uniform
    // buffer, TilesetParams) per frame slot — added to the
    // +kMaxTilesetSlots*kTilesetChannelCount / +1 below.
    // WP-E adds, per frame slot: 3 storage buffers (vt_draw_slots at 9, the
    // vt_indirection buffer at 11, vt_variants at 12), kVtChannelCount
    // combined image samplers (the pool array at 10), and 1 storage image
    // (feedback at 13). The indirection moved from a sampled image array to a
    // storage buffer with the buffer-indirection redesign — hence 27 storage
    // buffers, not 26, and no "+1" sampler.
    // RT PBR Phase 1 adds, per frame slot: one gi_temporal set (13 combined
    // samplers, 8 storage images) and three gi_atrous sets (7 combined
    // samplers, 1 storage image, 1 storage buffer each) for the transmission
    // signal chain -- the counts below already fold those in.
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame_slot_count * 2},
        // 27 for the scene/VT buffers above, +1 for the per-module
        // draw-override table at binding 14.
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_slot_count * 28},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         frame_slot_count *
             (113 + tileset::kMaxTilesetSlots * kTilesetChannelCount +
              vt::kVtChannelCount)},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 34}};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = frame_slot_count * 17;
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
    layouts.reserve(frame_slot_count * 17);
    for (size_t index = 0; index < frame_slot_count; ++index) {
        layouts.push_back(set_layouts_[0]);
        layouts.push_back(set_layouts_[1]);
        layouts.push_back(skin_set_layout_);
        layouts.push_back(composite_set_layout_);
        layouts.push_back(display_set_layout_);
        for (uint32_t i = 0; i < 3; ++i)
            layouts.push_back(gi_temporal_set_layout_);
        for (uint32_t i = 0; i < 9; ++i)
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
        frame.descriptor_sets[0] = sets[index * 17];
        frame.descriptor_sets[1] = sets[index * 17 + 1];
        frame.skin_descriptor_set = sets[index * 17 + 2];
        frame.composite_descriptor_set = sets[index * 17 + 3];
        frame.display_descriptor_set = sets[index * 17 + 4];
        for (uint32_t i = 0; i < 3; ++i)
            frame.gi_temporal_descriptor_sets[i] = sets[index * 17 + 5 + i];
        for (uint32_t i = 0; i < 9; ++i)
            frame.gi_atrous_descriptor_sets[i] = sets[index * 17 + 8 + i];
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
            !ensure_candidate_buffer(frame.animation_bounds,
                                     sizeof(VkAnimationBoundsGpuRecord),
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
            // 0-17 surface query, 18/19 reflection-lobe counters,
            // 20-28 WP-G VT/ray-cone readback (rt_surface_test.rgen).
            !ensure_candidate_buffer(frame.rt_test_output, 32 * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.gi_atrous_markers,
                                     5 * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_influences,
                                     sizeof(VkSkinInfluence),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_sources,
                                     sizeof(VkSkinSourceVertex),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_palette_current,
                                     sizeof(VkSkinJoint),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_palette_previous,
                                     sizeof(VkSkinJoint),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_work, sizeof(VkSkinWorkItem),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_current_output,
                                     sizeof(VkSkinVertex),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.skin_previous_output,
                                     sizeof(VkSkinVertex),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !ensure_candidate_buffer(frame.vt_draw_slots, sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !ensure_candidate_buffer(
                frame.part_draw_overrides,
                sizeof(matter::PartDrawOverrideGpu),
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
        // Phase 1 tileset Vulkan port (Task 6): RT set 0 gained binding 15
        // (kMaxTilesetSlots*kTilesetChannelCount combined-image-sampler
        // descriptors -- 48 = 8 slots x 6 channels) and binding 16 (1
        // uniform buffer, TilesetParams) per frame slot.
        // WP-G: plus binding 17 (kVtChannelCount = 4 VT pool samplers)
        // combined-image-samplers, and 18/19 storage buffers (the VT
        // indirection buffer and the variant table — 18 was an indirection
        // sampler before the buffer-indirection redesign).
        const VkDescriptorPoolSize rt_sizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, frame_slot_count},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             frame_slot_count *
                 (5 + tileset::kMaxTilesetSlots * kTilesetChannelCount +
                  vt::kVtChannelCount)},
            // 6 storage images: visibility, raw diffuse, raw specular +
            // aux, raw transmission + aux (RT PBR Phase 1).
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 6},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_slot_count * 6},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame_slot_count}};
        VkDescriptorPoolCreateInfo rt_pool{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        rt_pool.maxSets = frame_slot_count;
        rt_pool.poolSizeCount =
            static_cast<uint32_t>(sizeof(rt_sizes) / sizeof(rt_sizes[0]));
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
    update_descriptor(frame.descriptor_sets[1], 8,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.animation_bounds);
    // Compute source is a separately packed vec4/std430 copy of the immutable
    // raster arena; VkRasterVertex's 72-byte vertex-input ABI cannot be bound
    // as the shader's 80-byte source storage record.
    update_descriptor(frame.skin_descriptor_set, 0,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_sources);
    update_descriptor(frame.skin_descriptor_set, 1,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_influences);
    update_descriptor(frame.skin_descriptor_set, 2,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_palette_current);
    update_descriptor(frame.skin_descriptor_set, 3,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_palette_previous);
    update_descriptor(frame.skin_descriptor_set, 4,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_work);
    update_descriptor(frame.skin_descriptor_set, 5,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_current_output);
    update_descriptor(frame.skin_descriptor_set, 6,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.skin_previous_output);
    update_descriptor(frame.descriptor_sets[1], 9,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.vt_draw_slots);
    update_descriptor(frame.descriptor_sets[1], 14,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      frame.part_draw_overrides);
    write_tileset_descriptors_for_frame(frame.descriptor_sets[1]);
    write_vt_descriptors_for_frame(frame);
}

void VkSceneRenderer::probe_skin_raster_draws(
    const std::vector<VkSkinRasterDraw>& draws) const {
    static const bool enabled = [] {
        const char* value = std::getenv("MATTER_SKIN_PROBE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (!enabled) return;
    // raster.vert fetches skinned vertex (index - local_vertex_base) from the
    // draw's own output window. An index outside [local_vertex_base,
    // +vertex_count) reads memory the compute pass never wrote this frame --
    // the arenas are never cleared, so it decodes as plausible stale geometry
    // rather than an obvious crash. Report it loudly instead.
    for (const VkSkinRasterDraw& draw : draws) {
        if (draw.first_index > index_staging_.size() ||
            draw.index_count > index_staging_.size() - draw.first_index)
            continue;
        uint32_t outside = 0;
        uint32_t lowest = UINT32_MAX;
        uint32_t highest = 0;
        for (uint32_t offset = 0; offset != draw.index_count; ++offset) {
            const uint32_t vertex = index_staging_[draw.first_index + offset];
            lowest = std::min(lowest, vertex);
            highest = std::max(highest, vertex);
            if (vertex < draw.local_vertex_base ||
                vertex - draw.local_vertex_base >= draw.vertex_count)
                ++outside;
        }
        if (outside == 0) continue;
        fprintf(stderr,
                "[skin-probe] instance=%u gen=%u cluster=%u lod=%u "
                "outside=%u/%u indices=[%u,%u] window=[%u,%u)\n",
                draw.instance_slot, draw.instance_generation, draw.cluster,
                draw.lod, outside, draw.index_count, lowest, highest,
                draw.local_vertex_base,
                draw.local_vertex_base + draw.vertex_count);
    }
}

bool VkSceneRenderer::record_animation_skinning(
    const matter::VulkanFrame& frame, FrameResources& resources,
    std::string& error) {
    resources.skin_raster_ready = false;
    resources.ready_skin_raster_draws.clear();
    const VkSkinFrameArenas& staged = animation_skinning_.frame(frame.frame_slot);
    const auto publish_ready_draws =
        [this, &resources, &staged, &frame](bool current_source_ready) {
            VkSkinRasterValidationView validation{};
            validation.current_frame_slot = frame.frame_slot;
            validation.current_source_ready = current_source_ready;
            validation.index_count =
                static_cast<uint32_t>(index_staging_.size());
            validation.draw_transform_slots = draw_transform_slots_;
            validation.output_vertex_counts.resize(frames_.size(), 0);
            validation.output_buffers_ready.resize(frames_.size(), 0);
            for (uint32_t slot = 0; slot < frames_.size(); ++slot) {
                validation.output_vertex_counts[slot] =
                    animation_skinning_.frame(slot).current_output_vertices;
                validation.output_buffers_ready[slot] =
                    frames_[slot].skin_current_output.buffer != VK_NULL_HANDLE &&
                    frames_[slot].skin_previous_output.buffer != VK_NULL_HANDLE;
            }
            resources.ready_skin_raster_draws =
                filter_ready_animation_skin_raster_draws(
                    staged.raster_draws, validation);
            resources.skin_raster_ready =
                !resources.ready_skin_raster_draws.empty();
            probe_skin_raster_draws(resources.ready_skin_raster_draws);
        };
    const auto downgrade_gpu_skin =
        [this, &resources, &staged, &frame, &error, &publish_ready_draws](
            VkSkinGpuFailureReason reason) {
            std::vector<VkAnimationBoundsInstance> affected;
            affected.reserve(staged.work_items.size());
            for (size_t index = 0; index != staged.work_items.size(); ++index) {
                const VkSkinWorkItem& work = staged.work_items[index];
                const uint32_t generation =
                    index < staged.work_instance_generations.size()
                        ? staged.work_instance_generations[index] : 0;
                const auto draw = std::find_if(
                    staged.raster_draws.begin(), staged.raster_draws.end(),
                    [&work, generation, &frame](const VkSkinRasterDraw& value) {
                        return value.instance_slot == work.instance_slot &&
                               value.instance_generation == generation &&
                               value.output_frame_slot == frame.frame_slot &&
                               value.lod == vk_skin_work_lod(work.flags) &&
                               value.cluster ==
                                   vk_skin_work_cluster(work.flags);
                    });
                affected.push_back({
                    work.instance_slot,
                    generation,
                    draw != staged.raster_draws.end() ? draw->asset_key : 0});
            }
            if (!animation_skinning_.reject_gpu_frame(frame.frame_slot, reason))
                return false;
            // Never let a failed current pose retain a smaller dynamic AABB.
            // Replace it with the asset-wide conservative bound. A retained
            // explicit draw can then exclude exactly its own static peer,
            // while bind fallbacks remain in the indirect lane.
            animation_bounds_.fail_open_instances(affected);
            for (const auto& key : affected) {
                pending_visible_skin_instances_.erase(
                    (uint64_t(key.instance_slot) << 32u) |
                    key.instance_generation);
            }
            consumed_animation_skin_fallbacks_ =
                animation_skinning_.frame(frame.frame_slot).fallbacks;
            error.clear();
            publish_ready_draws(false);
            return true;
        };
    if (staged.work_items.empty()) {
        // Retained fallback draws reference an older sealed frame resource and
        // need no current compute dispatch.
        publish_ready_draws(false);
        return true;
    }
    const auto bytes = [](size_t count, size_t stride) -> VkDeviceSize {
        return static_cast<VkDeviceSize>(count) * static_cast<VkDeviceSize>(stride);
    };
    std::vector<VkSkinSourceVertex> sources(vertex_staging_.size());
    for (size_t index = 0; index != vertex_staging_.size(); ++index) {
        const VkRasterVertex& input = vertex_staging_[index];
        VkSkinSourceVertex& output = sources[index];
        output.position[0] = input.position.x; output.position[1] = input.position.y;
        output.position[2] = input.position.z;
        output.normal[0] = input.normal.x; output.normal[1] = input.normal.y;
        output.normal[2] = input.normal.z;
        output.tint[0] = input.tint.x; output.tint[1] = input.tint.y;
        output.tint[2] = input.tint.z; output.tint[3] = input.tint.w;
        output.surface[0] = input.surface.x; output.surface[1] = input.surface.y;
        output.surface[2] = input.surface.z; output.surface[3] = input.surface.w;
        output.material_index = input.material_index;
    }
    // C1 validates immutable influence slices.  The renderer additionally
    // owns the mutable raster arena, so validate it here before recording any
    // dispatch.  On a stale/missing source range we deliberately retain the
    // static (or last-good) raster path instead of issuing undefined SSBO
    // reads or exposing a half-populated compute output.
    for (const VkSkinWorkItem& work : staged.work_items) {
        if (work.source_vertex > sources.size() ||
            work.vertex_count > sources.size() - work.source_vertex) {
            return downgrade_gpu_skin(VkSkinGpuFailureReason::Upload);
        }
    }
    bool descriptors_changed = false;
    uint32_t skin_allocation = 0;
    const auto ensure = [&](matter::VkBufferResource& buffer, VkDeviceSize size,
                            VkBufferUsageFlags usage) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (skin_allocation == test_fail_after_skin_allocations_) {
            error = "forced animation skin allocation failure";
            return false;
        }
#endif
        ++skin_allocation;
        bool replaced = false;
        if (!ensure_buffer(buffer, std::max<VkDeviceSize>(size, 1), usage,
                           error, &replaced)) return false;
        descriptors_changed = descriptors_changed || replaced;
        return true;
    };
    if (!ensure(resources.skin_sources, bytes(sources.size(), sizeof(VkSkinSourceVertex)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensure(resources.skin_influences, bytes(animation_skinning_.influences().size(), sizeof(VkSkinInfluence)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensure(resources.skin_palette_current, bytes(staged.palette_current.size(), sizeof(VkSkinJoint)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensure(resources.skin_palette_previous, bytes(staged.palette_previous.size(), sizeof(VkSkinJoint)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensure(resources.skin_work, bytes(staged.work_items.size(), sizeof(VkSkinWorkItem)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensure(resources.skin_current_output, bytes(staged.current_output_vertices, sizeof(VkSkinVertex)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !ensure(resources.skin_previous_output, bytes(staged.previous_output_vertices, sizeof(VkSkinVertex)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return downgrade_gpu_skin(VkSkinGpuFailureReason::Allocation);
    // Every successful replacement is live even if a following upload
    // downgrades this skin queue. Publish all seven bindings before the first
    // fallible upload so a same-slot, same-size retry cannot reuse descriptors
    // that still reference the retired allocations.
    if (descriptors_changed) update_frame_descriptors(resources);
    uint32_t skin_upload = 0;
    const auto upload = [&](matter::VkBufferResource& buffer, const void* data,
                            VkDeviceSize size) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (size != 0 && skin_upload == test_fail_after_skin_uploads_) {
            error = "forced animation skin upload failure";
            return false;
        }
#endif
        if (size != 0) ++skin_upload;
        return size == 0 || matter::upload_buffer(*vulkan_, buffer, data, size, 0, error);
    };
    if (!upload(resources.skin_sources, sources.data(), bytes(sources.size(), sizeof(VkSkinSourceVertex))) ||
        !upload(resources.skin_influences, animation_skinning_.influences().data(), bytes(animation_skinning_.influences().size(), sizeof(VkSkinInfluence))) ||
        !upload(resources.skin_palette_current, staged.palette_current.data(), bytes(staged.palette_current.size(), sizeof(VkSkinJoint))) ||
        !upload(resources.skin_palette_previous, staged.palette_previous.data(), bytes(staged.palette_previous.size(), sizeof(VkSkinJoint))) ||
        !upload(resources.skin_work, staged.work_items.data(), bytes(staged.work_items.size(), sizeof(VkSkinWorkItem))))
        return downgrade_gpu_skin(VkSkinGpuFailureReason::Upload);
    uint32_t max_vertices = 0;
    for (const VkSkinWorkItem& work : staged.work_items)
        max_vertices = std::max(max_vertices, work.vertex_count);
    const uint32_t groups_x = (max_vertices + 63u) / 64u;
    if (groups_x == 0 || groups_x > limits_.max_dispatch_group_count_x) {
        error = "animation skin dispatch exceeds device workgroup limit";
        return downgrade_gpu_skin(VkSkinGpuFailureReason::Allocation);
    }
    const VkDescriptorSet sets[] = {resources.descriptor_sets[0],
                                    resources.descriptor_sets[1],
                                    resources.skin_descriptor_set};
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      skin_pipeline_);
    vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            skin_pipeline_layout_, 0, 3, sets, 0, nullptr);
    const uint32_t work_count = static_cast<uint32_t>(staged.work_items.size());
    vkCmdPushConstants(frame.command_buffer, skin_pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(work_count),
                       &work_count);
    vkCmdDispatch(frame.command_buffer, groups_x, work_count, 1);
    VkBufferMemoryBarrier2 barriers[2]{};
    for (VkBufferMemoryBarrier2& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
    }
    barriers[0].buffer = resources.skin_current_output.buffer;
    barriers[1].buffer = resources.skin_previous_output.buffer;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 2;
    dependency.pBufferMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    publish_ready_draws(true);
    return true;
}

// --- Phase 1 tileset Vulkan port (Task 6) ----------------------------------

bool VkSceneRenderer::create_tileset_image(VkFormat format, uint32_t edge_px,
                                           uint32_t mip_levels,
                                           uint32_t array_layers,
                                           TilesetImage& out,
                                           std::string& error) {
    out = TilesetImage{};
    if (edge_px == 0 || mip_levels == 0 || array_layers == 0) {
        error = "create_tileset_image requires nonzero extent/mips/layers";
        return false;
    }
    const VkDevice device = vulkan_->device();
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {edge_px, edge_px, 1};
    create.mipLevels = mip_levels;
    create.arrayLayers = array_layers;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkResult result = vkCreateImage(device, &create, nullptr, &image);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateImage(tileset)", result, error);
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    uint32_t memory_type = 0;
    VkMemoryPropertyFlags selected = 0;
    if (!matter::find_memory_type(vulkan_->physical_device(),
                                  requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  memory_type, selected, error)) {
        vkDestroyImage(device, image, nullptr);
        return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &allocate, nullptr, &memory);
    if (result != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        return fail_vk("vkAllocateMemory(tileset)", result, error);
    }
    result = vkBindImageMemory(device, image, memory, 0);
    if (result != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return fail_vk("vkBindImageMemory(tileset)", result, error);
    }
    VkImageViewCreateInfo view_create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_create.image = image;
    view_create.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_create.format = format;
    view_create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels,
                                    0, array_layers};
    VkImageView view = VK_NULL_HANDLE;
    result = vkCreateImageView(device, &view_create, nullptr, &view);
    if (result != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return fail_vk("vkCreateImageView(tileset)", result, error);
    }
    out.image = image;
    out.view = view;
    out.memory = memory;
    return true;
}

void VkSceneRenderer::destroy_tileset_image(TilesetImage& image) {
    if (!vulkan_) {
        image = TilesetImage{};
        return;
    }
    const VkDevice device = vulkan_->device();
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(device, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) vkDestroyImage(device, image.image, nullptr);
    if (image.memory != VK_NULL_HANDLE) vkFreeMemory(device, image.memory, nullptr);
    image = TilesetImage{};
}

namespace {
// Non-capturing so it converts to matter::ImmediateRecordFn (a plain function
// pointer); context travels via user_data like every other submit_immediate
// caller in this file (see record_raster/record_cull_dispatch above).
struct TilesetDummyInitRecord {
    VkImage images[3]{};
    bool rt_available = false;
};

void record_tileset_dummy_init(VkCommandBuffer cmd, void* user_data) {
    const auto& rec = *static_cast<TilesetDummyInitRecord*>(user_data);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 16};
    const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkPipelineStageFlags2 dst_stage =
        vk_scene_detail::ray_depth_destination_stages(rec.rt_available);
    for (VkImage image : rec.images) {
        VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_dst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_dst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = image;
        to_dst.subresourceRange = range;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_dst;
        vkCmdPipelineBarrier2(cmd, &dep);

        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zero, 1, &range);

        VkImageMemoryBarrier2 to_read = to_dst;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = dst_stage;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }
}
}  // namespace

bool VkSceneRenderer::ensure_tileset_infra(std::string& error) {
    if (tileset_infra_ready_) return true;
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(vulkan_->physical_device(), &features);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkan_->physical_device(), &properties);

    // Trilinear, repeat (cell UV is always in [0,1] — harmless), full LOD
    // range. anisotropyEnable is gated on the device feature actually being
    // enabled on this logical device (see vk_context.cpp's samplerAnisotropy
    // enable, which mirrors the same physical-device query) — requesting
    // anisotropy without the feature enabled is a validation error.
    VkSamplerCreateInfo sampler_create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_create.magFilter = VK_FILTER_LINEAR;
    sampler_create.minFilter = VK_FILTER_LINEAR;
    sampler_create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_create.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_create.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_create.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_create.anisotropyEnable =
        features.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    sampler_create.maxAnisotropy =
        features.samplerAnisotropy
            ? std::min(8.0f, properties.limits.maxSamplerAnisotropy)
            : 1.0f;
    sampler_create.minLod = 0.0f;
    sampler_create.maxLod = VK_LOD_CLAMP_NONE;
    VkResult result = vkCreateSampler(vulkan_->device(), &sampler_create,
                                      nullptr, &tileset_sampler_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateSampler(tileset)", result, error);

    // 1x1x16-layer dummy per format family (albedo/ORM/horizon_a/horizon_b
    // all share R8G8B8A8_UNORM -- Phase 2's horizon channels need no fourth
    // dummy). Every one of the 16 array entries is "statically used" by the
    // nonuniformEXT-indexed descriptor array regardless of which materials
    // are drawn this frame, so all three dummies must be valid,
    // SHADER_READ_ONLY_OPTIMAL images before the first frame — not just
    // before the first load_tileset_slot() call.
    if (!create_tileset_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 16,
                              tileset_dummy_rgba8_, error) ||
        !create_tileset_image(VK_FORMAT_R8G8_UNORM, 1, 1, 16,
                              tileset_dummy_rg8_, error) ||
        !create_tileset_image(VK_FORMAT_R16_UNORM, 1, 1, 16,
                              tileset_dummy_r16_, error)) {
        return false;
    }
    TilesetDummyInitRecord dummy_record{
        {tileset_dummy_rgba8_.image, tileset_dummy_rg8_.image,
         tileset_dummy_r16_.image},
        vulkan_->ray_tracing_available()};
    if (!matter::submit_immediate(*vulkan_, record_tileset_dummy_init,
                                  &dummy_record, error,
                                  matter::ImmediateSubmitPhase::image_transition)) {
        return false;
    }

    if (!matter::create_buffer(*vulkan_, sizeof(TilesetParamsGpu),
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               tileset_params_, error) ||
        !matter::map_buffer(tileset_params_, error)) {
        return false;
    }
    tileset_infra_ready_ = true;
    write_tileset_params_buffer();
    return true;
}

VkImageView VkSceneRenderer::tileset_channel_view(int slot, int channel) const {
    if (slot >= 0 && slot < tileset::kMaxTilesetSlots) {
        const TilesetSlotGpu& s = tileset_slots_[slot];
        if (s.loaded && s.channels[channel].view != VK_NULL_HANDLE)
            return s.channels[channel].view;
    }
    switch (channel) {
        case kTilesetChannelNormal: return tileset_dummy_rg8_.view;
        case kTilesetChannelHeight: return tileset_dummy_r16_.view;
        default: return tileset_dummy_rgba8_.view;  // albedo, orm
    }
}

void VkSceneRenderer::write_tileset_params_buffer() {
    if (!tileset_infra_ready_ || tileset_params_.mapped == nullptr) return;
    TilesetParamsGpu params{};
    for (int slot = 0; slot < tileset::kMaxTilesetSlots; ++slot) {
        const TilesetSlotGpu& s = tileset_slots_[slot];
        params.slot_tile_size_m[slot] = s.tile_size_m;
        params.slot_texels_per_meter[slot] = s.texels_per_meter;
        params.slot_height_min[slot] = s.height_min;
        params.slot_height_max[slot] = s.height_max;
        params.slot_mean_albedo[slot][0] = s.mean_albedo[0];
        params.slot_mean_albedo[slot][1] = s.mean_albedo[1];
        params.slot_mean_albedo[slot][2] = s.mean_albedo[2];
        // 0 = not loaded, 1 = loaded (no horizon data), 2 = loaded with
        // horizon data. See TilesetParamsGpu's file comment.
        params.slot_mean_albedo[slot][3] =
            !s.loaded ? 0.0f : (s.has_horizon ? 2.0f : 1.0f);
        // Phase 0: whole-atlas ORM mean, the denominator of the near band's
        // mean-preserving occlusion/roughness ratio. .w is unused; an
        // unloaded slot leaves zeros, and gbuffer.frag floors the divisor.
        params.slot_mean_orm[slot][0] = s.mean_orm[0];
        params.slot_mean_orm[slot][1] = s.mean_orm[1];
        params.slot_mean_orm[slot][2] = s.mean_orm[2];
    }
    // Ground POM UI knobs (matter::TilesetPomSettings, see
    // set_tileset_pom_settings). "enabled == false" uploads pom_steps == 0,
    // which gbuffer.frag's `full_steps > 0` check turns into a full skip of
    // the march/self-shadow branch -- the flat Wang tile sample still
    // applies, so this is a soft disable, not a black hole.
    params.pom_steps = tileset_pom_settings_.enabled
                            ? static_cast<float>(tileset_pom_settings_.steps)
                            : 0.0f;
    params.pom_refine_steps = 4.0f;
    params.pom_max_distance_m = tileset_pom_settings_.max_distance_m;
    params.pom_fade_band_m = tileset_pom_settings_.fade_band_m;
    params.pom_max_relief_m = tileset_pom_settings_.relief_cap_m;
    params.pom_max_march_m = tileset_pom_settings_.max_march_m;
    params.pom_datum_bias_ao_shadow[0] = tileset_pom_settings_.datum_bias_m;
    params.pom_datum_bias_ao_shadow[1] = tileset_pom_settings_.ao_strength;
    params.pom_datum_bias_ao_shadow[2] =
        tileset_pom_settings_.horizon_ambient_strength;
    params.pom_datum_bias_ao_shadow[3] = tileset_pom_settings_.horizon_strength;
    // Phase 0 near band (matter::VtNearBandSettings). Deliberately NOT
    // derived from the POM distance pair above any more -- see the struct's
    // comment for why the two were decoupled.
    params.vt_near_band[0] = vt_near_band_settings_.near_band_m;
    params.vt_near_band[1] = vt_near_band_settings_.near_fade_m;
    // Warp field (VT Phase 2): vt_near.z is the warp-march enable. Default
    // on; MATTER_VT_WARP=0 reverts the march addressing to the shipped
    // world-XZ form (diagnostic escape hatch, spec §9 — not an end state).
    {
        static const bool warp_enabled = [] {
            const char* env = std::getenv("MATTER_VT_WARP");
            return !(env && std::strcmp(env, "0") == 0);
        }();
        params.vt_near_band[2] = warp_enabled ? 1.0f : 0.0f;
    }
    // vt_near.w: the horizon diagnostic mode (TilesetPomSettings::
    // horizon_debug, viewer "Ground POM" UI). 0 = off, which is what every
    // shipped frame uploads; gbuffer.frag's overlay branch is a
    // uniform-conditional and does nothing at 0. Carried here rather than in
    // a new vec4 because vt_near.w was the last unused component and
    // TilesetParamsGpu's size is static_asserted at 464 bytes.
    params.vt_near_band[3] =
        static_cast<float>(tileset_pom_settings_.horizon_debug);
    // Task 11: direction-to-sun, same convention as the RT shadow push
    // constants (record_ray_trace_dispatch): normalize(-sun_direction),
    // since VkSceneLighting::sun_direction points FROM the sun toward the
    // scene. Falls back to straight-up when the light vector degenerates.
    {
        const float x = -lighting_.sun_direction.x;
        const float y = -lighting_.sun_direction.y;
        const float z = -lighting_.sun_direction.z;
        const float length = std::sqrt(x * x + y * y + z * z);
        params.sun_dir_intensity[0] = length > 0.0f ? x / length : 0.0f;
        params.sun_dir_intensity[1] = length > 0.0f ? y / length : 1.0f;
        params.sun_dir_intensity[2] = length > 0.0f ? z / length : 0.0f;
        params.sun_dir_intensity[3] = lighting_.sun_intensity;
    }
    std::memcpy(tileset_params_.mapped, &params, sizeof(params));
    std::string flush_error;
    matter::flush_buffer(tileset_params_, 0, sizeof(params), flush_error);
}

void VkSceneRenderer::write_tileset_descriptors_for_frame(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE || !tileset_infra_ready_) return;
    VkDescriptorImageInfo
        image_infos[tileset::kMaxTilesetSlots * kTilesetChannelCount]{};
    for (int slot = 0; slot < tileset::kMaxTilesetSlots; ++slot) {
        for (int channel = 0; channel < kTilesetChannelCount; ++channel) {
            VkDescriptorImageInfo& info =
                image_infos[slot * kTilesetChannelCount + channel];
            info.sampler = tileset_sampler_;
            info.imageView = tileset_channel_view(slot, channel);
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    VkDescriptorBufferInfo params_info{tileset_params_.buffer, 0,
                                       sizeof(TilesetParamsGpu)};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 6;
    writes[0].descriptorCount =
        tileset::kMaxTilesetSlots * kTilesetChannelCount;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = image_infos;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 7;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &params_info;
    vkUpdateDescriptorSets(vulkan_->device(), 2, writes, 0, nullptr);
}

// --- WP-E: chart-space virtual texturing ------------------------------------

namespace {
struct VtDummyInitRecord {
    VkImage feedback = VK_NULL_HANDLE;
};

// Clears the dummy feedback image and parks it in the layout the descriptor
// writes promise: GENERAL for the storage-image feedback dummy. (The old
// sampled indirection dummy is gone — the indirection is a storage buffer
// now, and the shared vt_dummy_storage_ buffer stands in for it.)
void record_vt_dummy_init(VkCommandBuffer cmd, void* user_data) {
    const auto& rec = *static_cast<VtDummyInitRecord*>(user_data);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue zero{};
    const struct { VkImage image; VkImageLayout final_layout; } targets[1] = {
        {rec.feedback, VK_IMAGE_LAYOUT_GENERAL}};
    for (const auto& target : targets) {
        VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_dst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_dst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = target.image;
        to_dst.subresourceRange = range;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_dst;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdClearColorImage(cmd, target.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        VkImageMemoryBarrier2 to_use = to_dst;
        to_use.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_use.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_use.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        to_use.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_use.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_use.newLayout = target.final_layout;
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &to_use;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }
}
}  // namespace

bool VkSceneRenderer::ensure_vt_dummies(std::string& error) {
    if (vt_dummies_ready_) return true;
    // Storage-image dummy for the feedback binding needs STORAGE usage, which
    // create_tileset_image does not request, so it is created inline here.
    // The indirection and variant-table bindings are storage buffers and both
    // dummy through vt_dummy_storage_ below.
    {
        const VkDevice device = vulkan_->device();
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = VK_FORMAT_R16G16B16A16_UINT;
        create.extent = {1, 1, 1};
        create.mipLevels = 1;
        create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &create, nullptr,
                          &vt_dummy_feedback_.image) != VK_SUCCESS) {
            error = "vkCreateImage(vt feedback dummy) failed";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, vt_dummy_feedback_.image,
                                     &requirements);
        uint32_t type = 0;
        VkMemoryPropertyFlags selected = 0;
        if (!matter::find_memory_type(vulkan_->physical_device(),
                                      requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                      type, selected, error)) {
            return false;
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &allocate, nullptr,
                             &vt_dummy_feedback_.memory) != VK_SUCCESS ||
            vkBindImageMemory(device, vt_dummy_feedback_.image,
                              vt_dummy_feedback_.memory, 0) != VK_SUCCESS) {
            error = "vt feedback dummy memory allocation failed";
            return false;
        }
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = vt_dummy_feedback_.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R16G16B16A16_UINT;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &view, nullptr,
                              &vt_dummy_feedback_.view) != VK_SUCCESS) {
            error = "vkCreateImageView(vt feedback dummy) failed";
            return false;
        }
    }
    VtDummyInitRecord record{vt_dummy_feedback_.image};
    if (!matter::submit_immediate(*vulkan_, record_vt_dummy_init, &record,
                                  error,
                                  matter::ImmediateSubmitPhase::image_transition)) {
        return false;
    }
    if (!matter::create_buffer(*vulkan_, 64,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                               vt_dummy_storage_, error)) {
        return false;
    }
    vt_dummies_ready_ = true;
    return true;
}

bool VkSceneRenderer::ensure_vt_runtime(std::string& error) {
    if (vt_ && vt_->available()) return true;
    if (vt_unavailable_) {
        error = vt_unavailable_reason_;
        return false;
    }
    // Safety valve: one env var takes every part back to the legacy path for
    // the whole session, without a rebuild.
    if (!vt_init_attempted_) {
        const char* disable = std::getenv("MATTER_VT_DISABLE");
        if (disable != nullptr && disable[0] != '\0' && disable[0] != '0') {
            vt_init_attempted_ = true;
            vt_unavailable_ = true;
            vt_unavailable_reason_ = "MATTER_VT_DISABLE is set";
            error = vt_unavailable_reason_;
            std::fprintf(stderr,
                         "[vk] chart-space VT disabled by MATTER_VT_DISABLE\n");
            std::fflush(stderr);
            return false;
        }
    }
    vt_init_attempted_ = true;
    auto runtime = std::make_unique<vt::VtResidency>();
    // Install WP-D's tier-1 compositor as the page filler BEFORE init(), which
    // only falls back to the WP-E stub when no filler is set. The residency
    // layer stays filler-agnostic (contract C2); the renderer owns the choice
    // because it is the only party that can feed the compositor its tileset
    // and material inputs.
    //
    // A compositor that fails to create is NOT fatal: the stub still produces
    // flat, correct-topology pages, so the world renders (dull, not broken).
    {
        std::string compositor_error;
        std::unique_ptr<vt::VtCompositor> compositor = vt::VtCompositor::create(
            vulkan_->device(), vulkan_->physical_device(), VK_NULL_HANDLE,
            compositor_error);
        if (compositor) {
            vt_compositor_ = compositor.get();
            runtime->set_filler(std::move(compositor));
            vt_inputs_dirty_ = true;
        } else {
            vt_compositor_ = nullptr;
            std::fprintf(stderr,
                         "[vk] VT tier-1 compositor unavailable, falling back "
                         "to the flat stub filler: %s\n",
                         compositor_error.c_str());
            std::fflush(stderr);
        }
    }
    // WP-H: tier-2 hemisphere AO enrichment. Requires hardware ray tracing and
    // is purely additive -- with no enricher the residency layer never queues a
    // page and everything stays tier-1, which is correct (just flatter in the
    // crevices). Installed BEFORE init() so the startup stats line reports the
    // real tier-2 configuration. MATTER_VT_ENRICH_PER_FRAME=0 keeps the
    // enricher loaded but drains nothing.
    if (!vulkan_->ray_tracing_available()) {
        std::fprintf(stderr,
                     "[vk] VT tier-2 enrichment off (no hardware ray "
                     "tracing): %s\n",
                     vulkan_->ray_tracing_unavailable_reason().c_str());
        std::fflush(stderr);
    } else {
        std::string enrich_error;
        std::unique_ptr<vt::VtEnricher> enricher =
            vt::VtEnricher::create(*vulkan_, VK_NULL_HANDLE, enrich_error);
        if (enricher) {
            vt_enricher_ = enricher.get();
            runtime->set_enricher(std::move(enricher));
        } else {
            vt_enricher_ = nullptr;
            std::fprintf(stderr,
                         "[vk] VT tier-2 enrichment unavailable (tier-1 pages "
                         "retained): %s\n",
                         enrich_error.c_str());
            std::fflush(stderr);
        }
    }
    if (!runtime->init(*vulkan_, error)) {
        vt_compositor_ = nullptr;
        vt_enricher_ = nullptr;
        // Fail closed for the whole session: every part stays chartless and
        // the legacy path keeps rendering exactly as before.
        vt_unavailable_ = true;
        vt_unavailable_reason_ = error;
        std::fprintf(stderr,
                     "[vk] chart-space VT unavailable (legacy path retained): "
                     "%s\n",
                     error.c_str());
        std::fflush(stderr);
        return false;
    }
    vt_ = std::move(runtime);
    const vt::VtResidency::Stats& started = vt_->stats();
    std::fprintf(stderr,
                 "[vk] chart-space VT online: %u page pool (%.0f MiB), "
                 "%u variant slots (MATTER_VT_MAX_VARIANTS, soft bound), "
                 "%.0f MiB indirection arena (MATTER_VT_INDIRECTION_MB, "
                 "exact-sized tables), %.0f MiB mesh budget "
                 "(MATTER_VT_MESH_BUDGET_MB), %u fills/frame, filler=%s, "
                 "tier2=%s (%u rays/texel, %u pages/frame)\n",
                 started.pool_capacity,
                 static_cast<double>(started.pool_bytes) / (1024.0 * 1024.0),
                 started.max_variants,
                 static_cast<double>(started.indirection_capacity_bytes) /
                     (1024.0 * 1024.0),
                 static_cast<double>(started.mesh_budget_bytes) /
                     (1024.0 * 1024.0),
                 vt_->max_fills_per_frame(),
                 vt_compositor_ ? "tier-1 compositor" : "flat stub",
                 vt_enricher_ ? "hemisphere AO" : "off",
                 started.enrich_samples, vt_->max_enrich_per_frame());
    std::fflush(stderr);
    push_vt_compositor_inputs();
    return true;
}

void VkSceneRenderer::push_vt_compositor_inputs() {
    if (!vt_compositor_ || !vt_inputs_dirty_) return;
    // Both setters require the device to be idle with respect to this
    // compositor's prior fills (vt_compositor.h). Tileset loads and material
    // table changes are world-load / live-edit events, not per-frame ones, so
    // paying a wait_idle here is cheap and unambiguously correct.
    vulkan_->wait_idle();

    vt::VtTilesetSlotViews slots[tileset::kMaxTilesetSlots]{};
    for (int slot = 0; slot < tileset::kMaxTilesetSlots; ++slot) {
        const TilesetSlotGpu& source = tileset_slots_[slot];
        if (!source.loaded) continue;   // null views -> compositor's neutral dummy
        slots[slot].albedo = source.channels[kTilesetChannelAlbedo].view;
        slots[slot].normal = source.channels[kTilesetChannelNormal].view;
        slots[slot].orm = source.channels[kTilesetChannelOrm].view;
        slots[slot].height = source.channels[kTilesetChannelHeight].view;
        slots[slot].tile_size_m =
            source.tile_size_m > 0.0f ? source.tile_size_m : 1.0f;
        slots[slot].texels_per_meter =
            source.texels_per_meter > 0.0f ? source.texels_per_meter : 1024.0f;
    }
    std::string tileset_error;
    if (!vt_compositor_->set_tilesets(slots, tileset::kMaxTilesetSlots,
                                      tileset_error)) {
        std::fprintf(stderr, "[vk] VT compositor tileset bind failed: %s\n",
                     tileset_error.c_str());
        std::fflush(stderr);
    }

    // materialId -> detail slot + scalar fallbacks, decoded from the same
    // MaterialGpuRecord table the G-buffer shades with, so the compositor and
    // the legacy path can never disagree about which tileset a material uses.
    // flags_misc[1] carries MaterialPackDetailMacroSlots(detail, macro);
    // its low byte is detailSlot + 1 (0 = none) -- the encoding
    // tileset_common.glsl's tileset_detail_slot() decodes.
    const size_t material_count =
        std::min<size_t>(material_staging_.size(), vt::VtCompositor::kMaxMaterials);
    std::vector<vt::VtCompositorMaterial> materials(material_count);
    for (size_t i = 0; i < material_count; ++i) {
        const MaterialGpuRecord& source = material_staging_[i];
        vt::VtCompositorMaterial& target = materials[i];
        target.albedo[0] = source.base_roughness[0];
        target.albedo[1] = source.base_roughness[1];
        target.albedo[2] = source.base_roughness[2];
        target.albedo[3] = 1.0f;
        target.orm[0] = 1.0f;                              // occlusion
        target.orm[1] = source.base_roughness[3];          // roughness
        target.orm[2] = source.metal_opacity_spec_coat[0]; // metallic
        const int packed_detail =
            static_cast<int>(source.flags_misc[1] & 0xFFu) - 1;
        target.detail_slot =
            (packed_detail >= 0 && packed_detail < tileset::kMaxTilesetSlots)
                ? packed_detail
                : -1;
    }
    vt_compositor_->set_materials(materials.data(),
                                  static_cast<uint32_t>(materials.size()));
    vt_inputs_dirty_ = false;

    // Rebinding only fixes FUTURE fills. Every page already in the pool was
    // baked from the tilesets/materials that just changed, and the pinned
    // per-variant tails never expire on their own -- so without this the world
    // keeps showing pre-edit pages indefinitely.
    //
    // ORDERING: the wait_idle above retired every fill recorded against the old
    // inputs, and invalidate_all_content only QUEUES the re-fills; they are
    // drained by VtResidency::record_frame during this frame's recording, i.e.
    // strictly after the set_tilesets/set_materials above. A re-queued fill can
    // therefore only ever run against the newly bound inputs.
    //
    // Skipped on the first push (the one inside ensure_vt_runtime): nothing is
    // resident yet, so it could only duplicate registration-time tail fills.
    if (vt_inputs_pushed_ && vt_ && vt_->available())
        vt_->invalidate_all_content();
    vt_inputs_pushed_ = true;
}

void VkSceneRenderer::drain_vt_invalidations(uint64_t serial) {
    if ((!vt_compositor_ && !vt_enricher_) || vt_pending_invalidate_.empty())
        return;
    size_t keep = 0;
    for (size_t i = 0; i < vt_pending_invalidate_.size(); ++i) {
        if (serial >= vt_pending_invalidate_[i].second) {
            if (vt_compositor_)
                vt_compositor_->invalidate_part(vt_pending_invalidate_[i].first);
            // WP-H: the enricher caches the same geometry PLUS the variant's
            // acceleration structure; both are keyed on the variant hash and
            // both go stale for exactly the same reasons.
            if (vt_enricher_)
                vt_enricher_->invalidate_part(vt_pending_invalidate_[i].first);
            continue;
        }
        vt_pending_invalidate_[keep++] = vt_pending_invalidate_[i];
    }
    vt_pending_invalidate_.resize(keep);
}

void VkSceneRenderer::register_vt_part(int part_slot, const VkScenePart& part) {
    if (part_slot < 0 || static_cast<size_t>(part_slot) >= parts_.size()) return;
    PartRecord& record = parts_[part_slot];
    record.vt_slots.assign(kVkMaxLod, vt::kVtNoSlot);
    // Demand-driven path: the part declares which rungs COULD carry a VT
    // variant and ships no payload. Nothing registers here — the per-frame
    // demand pass surfaces (part, rung) requests when a rung is actually
    // selected on screen, and the engine answers with register_vt_rung().
    // Deliberately no ensure_vt_runtime() either: a world whose deferred
    // parts never get close enough to want a variant never starts the
    // runtime, exactly like a chartless world.
    if (part.vt_deferred_rung_mask != 0) {
        record.vt_rung_mask = part.vt_deferred_rung_mask;
        record.vt_last_wanted.fill(0);
        record.vt_last_requested.fill(0);
        ++vt_deferred_parts_;
        return;
    }
    bool any_charts = false;
    for (const chart_atlas::ChartAtlasRung& rung : part.lod_charts) {
        if (!rung.charts.empty()) { any_charts = true; break; }
    }
    if (!any_charts) return;   // legacy path, no runtime start
    std::string error;
    if (!ensure_vt_runtime(error)) return;

    // The packed material table travels with the part (see
    // VkScenePart::chart_material_table); register_variant() copies it, so it
    // may die with the caller's VkScenePart.
    const uint32_t material_stride = part.chart_material_stride;
    const uint32_t material_count =
        material_stride != 0
            ? static_cast<uint32_t>(part.chart_material_table.size() /
                                    material_stride)
            : 0u;

    const size_t rungs = std::min<size_t>(part.lod_charts.size(), kVkMaxLod);
    for (size_t rung = 0; rung < rungs; ++rung) {
        const chart_atlas::ChartAtlasRung& atlas = part.lod_charts[rung];
        if (atlas.charts.empty()) continue;
        vt::VtPartContext context;
        context.variant_hash = part.part_hash;
        context.rung = static_cast<uint32_t>(rung);
        context.rung_count = static_cast<uint32_t>(part.lod_charts.size());
        if (rung < part.lod_chart_meshes.size()) {
            const VkScenePartChartMesh& mesh = part.lod_chart_meshes[rung];
            context.positions = mesh.positions.empty() ? nullptr
                                                       : mesh.positions.data();
            context.normals = mesh.normals.empty() ? nullptr
                                                   : mesh.normals.data();
            context.surface_uvs =
                mesh.surface_uvs.empty() ? nullptr : mesh.surface_uvs.data();
            context.material_ids =
                mesh.material_ids.empty() ? nullptr : mesh.material_ids.data();
            context.vertex_count = mesh.vertex_count;
            context.indices = mesh.indices.empty() ? nullptr
                                                   : mesh.indices.data();
            context.triangle_count =
                static_cast<uint32_t>(mesh.indices.size() / 3u);
            context.dominant_material = mesh.dominant_material;
        }
        context.material_table = part.chart_material_table.empty()
                                     ? nullptr
                                     : part.chart_material_table.data();
        context.material_count = material_count;
        context.material_stride = material_stride;
        // WP-F: hand over the surfaces()-tape classification when the caller
        // computed one for this rung. Size mismatches fail closed to the
        // TriEx materialId path (the residency layer validates again).
        if (!part.surface_materials.empty() &&
            rung < part.lod_chart_meshes.size()) {
            const VkScenePartChartMesh& mesh = part.lod_chart_meshes[rung];
            const size_t expected =
                static_cast<size_t>(mesh.vertex_count) *
                part.surface_materials.size();
            if (!mesh.surface_weights.empty() &&
                mesh.surface_weights.size() == expected) {
                context.surface_weights = mesh.surface_weights.data();
                context.surface_materials = part.surface_materials.data();
                context.surface_material_count =
                    static_cast<uint32_t>(part.surface_materials.size());
                context.surface_tape_hash = part.surface_tape_hash;
                // P2: the mode-3 payload (tape text, anchoring, transform,
                // per-vertex field lanes). Empty text keeps the rung mode 2.
                if (!part.surface_tape_text.empty()) {
                    context.surface_tape_text =
                        part.surface_tape_text.c_str();
                    context.surface_world_anchored =
                        part.surface_world_anchored;
                    std::memcpy(context.surface_local_to_world,
                                part.surface_local_to_world,
                                sizeof(context.surface_local_to_world));
                    if (!mesh.surface_lanes.empty() &&
                        mesh.surface_lane_count > 0 &&
                        mesh.surface_lanes.size() ==
                            static_cast<size_t>(mesh.vertex_count) *
                                mesh.surface_lane_count) {
                        context.surface_lanes = mesh.surface_lanes.data();
                        context.surface_lane_count = mesh.surface_lane_count;
                    }
                }
            }
        }
        const uint32_t slot = vt_->register_variant(
            part.part_hash, static_cast<uint32_t>(rung), atlas, context);
        record.vt_slots[rung] = slot;
        if (slot != vt::kVtNoSlot) vt_draw_slots_dirty_ = true;
    }
}

// --- Demand-driven VT variant registration ----------------------------------

void VkSceneRenderer::take_vt_rung_requests(std::vector<VtRungRequest>& out) {
    out.clear();
    out.swap(vt_rung_requests_);
}

void VkSceneRenderer::evict_vt_rung(PartRecord& record, uint32_t rung) {
    if (rung >= record.vt_slots.size() ||
        record.vt_slots[rung] == vt::kVtNoSlot)
        return;
    if (vt_) vt_->release_variant(record.hash, rung);
    record.vt_slots[rung] = vt::kVtNoSlot;
    // Same deferred filler-cache retirement as release_part: a fill recorded
    // this frame may still read the compositor/enricher buffers cached for
    // this part. Re-registration after the drop rebuilds them (content-
    // addressed, so only tape edits actually change what gets rebuilt).
    if (vt_compositor_ || vt_enricher_)
        vt_pending_invalidate_.emplace_back(record.hash,
                                            vt_invalidate_retire_serial());
    vt_draw_slots_dirty_ = true;
}

bool VkSceneRenderer::register_vt_rung(uint64_t part_hash, uint32_t rung,
                                       const chart_atlas::ChartAtlasRung& atlas,
                                       const vt::VtPartContext& context) {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return false;   // part unloaded since request
    PartRecord& record = parts_[found->second];
    if (rung >= kVkMaxLod || ((record.vt_rung_mask >> rung) & 1u) == 0u)
        return false;
    if (record.vt_slots.size() < kVkMaxLod)
        record.vt_slots.assign(kVkMaxLod, vt::kVtNoSlot);
    if (record.vt_slots[rung] != vt::kVtNoSlot) return true;   // already live
    std::string error;
    if (!ensure_vt_runtime(error)) return false;

    // Working-set admission: when the layer pool or the CPU mesh budget is
    // full, reclaim least-recently-wanted demand-managed variants until the
    // registration fits. Eager registrations (vt_rung_mask == 0) and anything
    // wanted THIS demand frame are never victims — if only those remain, the
    // working set genuinely exceeds the budget, the residency layer counts
    // the rejection, and the warning banner tells the author which knob to
    // raise. The request itself simply retries while the rung stays wanted.
    const size_t wanted_bytes = vt::vt_variant_mesh_bytes(atlas, context);
    for (;;) {
        const vt::VtResidency::Stats& s = vt_->stats();
        if (vt::vt_registration_verdict(s.variants, s.max_variants,
                                        s.mesh_bytes, s.mesh_budget_bytes,
                                        wanted_bytes) ==
            vt::VtRejectReason::Accept)
            break;
        PartRecord* victim_record = nullptr;
        uint32_t victim_rung = 0;
        uint64_t victim_stamp = UINT64_MAX;
        for (PartRecord& candidate : parts_) {
            if (!candidate.live || candidate.vt_rung_mask == 0u) continue;
            const size_t rung_count =
                std::min<size_t>(candidate.vt_slots.size(), kVkMaxLod);
            for (uint32_t r = 0; r < rung_count; ++r) {
                if (candidate.vt_slots[r] == vt::kVtNoSlot) continue;
                const uint64_t stamp = candidate.vt_last_wanted[r];
                if (stamp >= vt_demand_frame_) continue;   // wanted this frame
                if (stamp < victim_stamp) {
                    victim_stamp = stamp;
                    victim_record = &candidate;
                    victim_rung = r;
                }
            }
        }
        if (!victim_record) break;   // nothing evictable; let the gate reject
        evict_vt_rung(*victim_record, victim_rung);
    }

    const uint32_t slot = vt_->register_variant(part_hash, rung, atlas, context);
    record.vt_slots[rung] = slot;
    if (slot != vt::kVtNoSlot) vt_draw_slots_dirty_ = true;
    return slot != vt::kVtNoSlot;
}

void VkSceneRenderer::update_vt_demand(matter::Float3 camera_eye,
                                       float pixel_budget) {
    if (vt_deferred_parts_ == 0) return;
    // Re-read every pass rather than latched on the first one: both knobs live
    // in matter::VtResidencyBudgets now, and the demand pass is exactly the
    // per-frame consumer that makes them live-editable from Tunables. The
    // clamps are the schema's own ranges, repeated here because the engine also
    // runs with no registry bound at all.
    matter::ensure_vt_residency_env_applied();
    {
        const matter::VtResidencyBudgets& b = matter::vt_residency_budgets();
        const auto clamp_u32 = [](uint32_t v, uint32_t lo, uint32_t hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        vt_linger_frames_ = clamp_u32(b.linger_frames, 2u, 100000u);
        vt_max_requests_ = clamp_u32(b.requests_per_frame, 1u, 256u);
    }
    ++vt_demand_frame_;

    // Stamp wanted rungs: the exact CPU mirror of cull.comp's LOD selection
    // (same clusters, thresholds and projected-size formula) over the static
    // instance set. instance_staging_ holds exactly that set here — dynamic
    // tails are merged later, in prepare_frame, and dynamic (animated)
    // instances are not deferred-VT parts. A near-threshold float divergence
    // from the GPU costs one frame of classified-legacy fallback for one
    // cluster, nothing more.
    vt_rung_requests_.clear();
    for (const GpuInstance& instance : instance_staging_) {
        if (instance.part_slot >= parts_.size()) continue;
        PartRecord& record = parts_[instance.part_slot];
        if (record.vt_rung_mask == 0u || !record.live) continue;
        const float* m = instance.object_to_world.elements;   // column-major
        const float scale =
            (std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) +
             std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]) +
             std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10])) /
            3.0f;
        const uint32_t cluster_end = instance.cluster_start +
                                     instance.cluster_count;
        for (uint32_t c = instance.cluster_start;
             c < cluster_end && c < cluster_staging_.size(); ++c) {
            const GpuCluster& cluster = cluster_staging_[c];
            const float lx = (cluster.aabb_min[0] + cluster.aabb_max[0]) * 0.5f;
            const float ly = (cluster.aabb_min[1] + cluster.aabb_max[1]) * 0.5f;
            const float lz = (cluster.aabb_min[2] + cluster.aabb_max[2]) * 0.5f;
            const float wx = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
            const float wy = m[1] * lx + m[5] * ly + m[9] * lz + m[13];
            const float wz = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
            const float dx = wx - camera_eye.x;
            const float dy = wy - camera_eye.y;
            const float dz = wz - camera_eye.z;
            const float distance_to_eye =
                std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.01f);
            const float projected_size =
                cluster.radius * scale / distance_to_eye * pixel_budget;
            uint32_t lod = cluster.lod_count != 0 ? cluster.lod_count - 1u : 0u;
            for (uint32_t i = 0; i < cluster.lod_count; ++i) {
                if (projected_size >= cluster.thresholds[i]) {
                    lod = i;
                    break;
                }
            }
            if (c >= cluster_lods_.size()) continue;
            const std::vector<VkSceneLod>& lods = cluster_lods_[c];
            if (lod >= lods.size()) continue;
            const uint32_t rung = lods[lod].chart_rung;
            if (rung >= kVkMaxLod ||
                ((record.vt_rung_mask >> rung) & 1u) == 0u)
                continue;
            record.vt_last_wanted[rung] = vt_demand_frame_;
            const bool registered =
                rung < record.vt_slots.size() &&
                record.vt_slots[rung] != vt::kVtNoSlot;
            if (!registered &&
                record.vt_last_requested[rung] != vt_demand_frame_) {
                record.vt_last_requested[rung] = vt_demand_frame_;
                vt_rung_requests_.push_back(
                    {record.hash, rung, projected_size});
            } else if (!registered) {
                // Already queued this frame by another cluster/instance —
                // keep the highest priority for the sort below.
                for (VtRungRequest& request : vt_rung_requests_) {
                    if (request.part_hash == record.hash &&
                        request.rung == rung) {
                        request.priority =
                            std::max(request.priority, projected_size);
                        break;
                    }
                }
            }
        }
    }
    std::sort(vt_rung_requests_.begin(), vt_rung_requests_.end(),
              [](const VtRungRequest& a, const VtRungRequest& b) {
                  return a.priority > b.priority;
              });
    if (vt_rung_requests_.size() > vt_max_requests_)
        vt_rung_requests_.resize(vt_max_requests_);

    // Linger release: reclaim variants no instance has wanted for a while.
    // Proactive (not just under admission pressure) so the pool tracks the
    // camera instead of pinning the high-water-mark working set forever.
    if (!vt_ || !vt_->available()) return;
    for (PartRecord& record : parts_) {
        if (!record.live || record.vt_rung_mask == 0u) continue;
        const size_t rung_count =
            std::min<size_t>(record.vt_slots.size(), kVkMaxLod);
        for (uint32_t r = 0; r < rung_count; ++r) {
            if (record.vt_slots[r] == vt::kVtNoSlot) continue;
            if (vt_demand_frame_ - record.vt_last_wanted[r] >
                vt_linger_frames_)
                evict_vt_rung(record, r);
        }
    }
}

// --- WP-F: surfaces()-tape live update --------------------------------------
// A tape edit changes what the compositor bakes into every page of every
// tape-classified variant, without touching geometry or tileset bindings. The
// bracket mirrors push_vt_compositor_inputs' discipline: wait the device idle
// once (tape edits are live-edit events, not per-frame ones), swap the CPU
// weight columns in the residency layer's owned copies, drop the compositor's
// cached GPU triangle streams (they embed the old weights), then declare all
// resident page content stale so the queued re-fills bake from the new tape.

void VkSceneRenderer::begin_vt_surface_update() {
    vt_surface_update_open_ = false;
    vt_surface_updates_applied_ = 0;
    if (!vt_ || !vt_->available()) return;
    // No fill referencing the old weight arrays (or the mesh caches about to
    // be invalidated) may still be unretired while we swap them.
    vulkan_->wait_idle();
    vt_surface_update_open_ = true;
}

bool VkSceneRenderer::update_vt_part_surface(
    uint64_t part_hash, const std::vector<std::vector<uint8_t>>& rung_weights,
    const std::vector<uint32_t>& materials, uint64_t tape_hash,
    const char* tape_text,
    const std::vector<std::vector<uint16_t>>* rung_lanes,
    uint32_t lane_count) {
    if (!vt_surface_update_open_ || !vt_ || !vt_->available()) return false;
    bool updated = false;
    // Same rung sweep bound as VtResidency::release_variant.
    for (uint32_t rung = 0; rung < 32u; ++rung) {
        if (vt_->slot_for(part_hash, rung) == vt::kVtNoSlot) continue;
        const std::vector<uint8_t>* weights =
            rung < rung_weights.size() ? &rung_weights[rung] : nullptr;
        const bool strip =
            materials.empty() || weights == nullptr || weights->empty();
        // P2: the mode-3 payload for this rung (tape text is per part; lanes
        // are per rung and optional — a missing/empty entry means the tape
        // reads no field inputs, and lane_count 0 travels with it).
        const std::vector<uint16_t>* lanes =
            (rung_lanes != nullptr && rung < rung_lanes->size())
                ? &(*rung_lanes)[rung]
                : nullptr;
        const bool have_lanes =
            !strip && lanes != nullptr && !lanes->empty() && lane_count > 0;
        const bool ok = vt_->update_variant_surface(
            part_hash, rung, strip ? nullptr : weights->data(),
            strip ? 0 : weights->size(), strip ? nullptr : materials.data(),
            strip ? 0u : static_cast<uint32_t>(materials.size()), tape_hash,
            strip ? nullptr : tape_text,
            have_lanes ? lanes->data() : nullptr,
            have_lanes ? lane_count : 0u);
        updated = updated || ok;
    }
    if (updated) {
        // The compositor's cached per-(variant, rung) triangle buffers embed
        // the per-vertex weights; rebuild them from the swapped context on the
        // next fill. Safe immediately: begin_vt_surface_update wait_idled.
        if (vt_compositor_) vt_compositor_->invalidate_part(part_hash);
        // WP-H: the enricher caches the same streams (its chart resolve reads
        // them), so it must rebuild too. Its acceleration structure is over
        // positions/indices only and a tape edit does not move geometry -- but
        // dropping the whole entry keeps one invalidation rule instead of two.
        if (vt_enricher_) vt_enricher_->invalidate_part(part_hash);
        ++vt_surface_updates_applied_;
    }
    return updated;
}

void VkSceneRenderer::end_vt_surface_update() {
    if (!vt_surface_update_open_) return;
    vt_surface_update_open_ = false;
    if (vt_surface_updates_applied_ == 0) return;
    vt_surface_updates_applied_ = 0;
    // Same reasoning as push_vt_compositor_inputs: swapping inputs only fixes
    // FUTURE fills; every resident page (pinned tails included) was baked from
    // the old tape. invalidate_all_content only QUEUES re-fills — they drain
    // in the next record_frame, strictly after the updates above.
    if (vt_ && vt_->available()) vt_->invalidate_all_content();
}

uint32_t VkSceneRenderer::vt_slot_for_lod(const PartRecord& record,
                                          uint32_t global_cluster,
                                          uint32_t lod_index) const {
    // The ONE (cluster, lod) -> vt slot mapping. Both consumers go through it:
    // rebuild_vt_draw_slots() (raster, via cull.comp's vt_draw_slots table)
    // and emit_ray_instances() (WP-G, via GpuRtPartRecord::vt_slot). A ray hit
    // and a raster fragment on the same rung MUST address the same indirection
    // layer, so these two must never drift apart — hence the shared helper
    // rather than a second transcription of the rule.
    //
    // A cluster's ladder position is not its rung index: VkSceneLod::chart_rung
    // names the rung the step actually draws.
    if (record.vt_slots.empty()) return vt::kVtNoSlot;
    if (global_cluster >= cluster_lods_.size()) return vt::kVtNoSlot;
    const std::vector<VkSceneLod>& lods = cluster_lods_[global_cluster];
    if (lod_index >= lods.size()) return vt::kVtNoSlot;
    const uint32_t rung = lods[lod_index].chart_rung;
    if (rung >= record.vt_slots.size()) return vt::kVtNoSlot;
    const uint32_t slot = record.vt_slots[rung];
    if (slot == vt::kVtNoSlot) return vt::kVtNoSlot;
    // TAIL GATE (streaming black-flash fix): a freshly registered variant's
    // pinned tail is MAPPED immediately but FILLED through the bounded fill
    // queue — potentially frames later under a streaming burst. Until the
    // residency layer says the tail's content is guaranteed written for any
    // draw recorded now, the draw must keep vt_slot 0 and render through the
    // legacy classified-but-flat path (a brief flat window, never a black
    // one). vt_begin_frame republishes this table the frame a tail becomes
    // ready (consume_activation_dirty), so the gate lifts within a frame of
    // the fill landing.
    if (!vt_ || !vt_->slot_active(slot)) return vt::kVtNoSlot;
    return slot;
}

void VkSceneRenderer::rebuild_vt_draw_slots() {
    // Indexed exactly like command_template_: cluster_index * kVkMaxLod + lod,
    // which is the `bucket` cull.comp already computes.
    vk_perf::reserve_geometric(vt_draw_slot_table_,
                               cluster_staging_.size() * kVkMaxLod);
    vt_draw_slot_table_.assign(cluster_staging_.size() * kVkMaxLod,
                               vt::kVtNoSlot);
    for (size_t cluster = 0; cluster < cluster_lods_.size(); ++cluster) {
        const GpuCluster& staged = cluster_staging_[cluster];
        const uint32_t part_slot = staged.part_slot;
        if (part_slot >= parts_.size()) continue;
        const PartRecord& record = parts_[part_slot];
        // staged.lod_count == cluster_lods_[cluster].size(); see
        // rebuild_command_template. Avoids a per-cluster heap indirection in a
        // loop that runs over every cluster in the world.
        const size_t lod_count =
            std::min<size_t>(staged.lod_count, kVkMaxLod);
        for (size_t lod = 0; lod < lod_count; ++lod) {
            vt_draw_slot_table_[cluster * kVkMaxLod + lod] =
                vt_slot_for_lod(record, static_cast<uint32_t>(cluster),
                                static_cast<uint32_t>(lod));
        }
    }
    vt_draw_slots_dirty_ = false;
}

void VkSceneRenderer::set_part_draw_overrides(
    const std::vector<matter::PartDrawOverrideEntry>& entries) {
    if (entries.size() == part_draw_override_entries_.size() &&
        std::equal(entries.begin(), entries.end(),
                   part_draw_override_entries_.begin(),
                   [](const matter::PartDrawOverrideEntry& a,
                      const matter::PartDrawOverrideEntry& b) {
                       return a.part_hash == b.part_hash &&
                              a.value.max_draw_distance ==
                                  b.value.max_draw_distance &&
                              a.value.lod_bias == b.value.lod_bias;
                   }))
        return;
    part_draw_override_entries_ = entries;
    part_draw_overrides_dirty_ = true;
}

void VkSceneRenderer::rebuild_part_draw_overrides() {
    part_draw_overrides_dirty_ = false;
    if (part_draw_override_entries_.empty()) {
        // Default state: one neutral entry. cull.comp reads slot 0, finds
        // {0, 1}, takes neither branch, and every other slot fails the
        // length() bound -- byte-identical selection to the pre-override
        // shader, with no per-part memory traffic.
        part_draw_override_table_.assign(1, matter::PartDrawOverrideGpu{});
        return;
    }
    part_draw_override_table_.assign(parts_.size(),
                                     matter::PartDrawOverrideGpu{});
    for (size_t slot = 0; slot < parts_.size(); ++slot) {
        const PartRecord& record = parts_[slot];
        if (!record.live || record.hash == 0) continue;
        auto it = std::lower_bound(
            part_draw_override_entries_.begin(),
            part_draw_override_entries_.end(), record.hash,
            [](const matter::PartDrawOverrideEntry& e, uint64_t h) {
                return e.part_hash < h;
            });
        if (it == part_draw_override_entries_.end() ||
            it->part_hash != record.hash)
            continue;
        part_draw_override_table_[slot] = it->value;
    }
    // A world with zero registered parts still needs a bindable buffer.
    if (part_draw_override_table_.empty())
        part_draw_override_table_.assign(1, matter::PartDrawOverrideGpu{});
}

void VkSceneRenderer::write_vt_descriptors_for_frame(FrameResources& frame) {
    if (frame.descriptor_sets[1] == VK_NULL_HANDLE || !vt_dummies_ready_)
        return;
    const bool live = vt_ && vt_->available();
    VkDescriptorImageInfo pool_infos[vt::kVtChannelCount]{};
    for (uint32_t c = 0; c < vt::kVtChannelCount; ++c) {
        pool_infos[c].sampler = live ? vt_->pool_sampler() : tileset_sampler_;
        pool_infos[c].imageView =
            live ? vt_->pool_view(c) : tileset_dummy_rgba8_.view;
        pool_infos[c].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // The indirection is a storage buffer since the buffer-indirection
    // redesign; the dummy storage buffer stands in for it (and for the
    // variant table) until the runtime is live.
    VkDescriptorBufferInfo indirection_info{
        live ? vt_->indirection_buffer() : vt_dummy_storage_.buffer, 0,
        live ? vt_->indirection_buffer_size() : VkDeviceSize{64}};
    VkDescriptorBufferInfo variants_info{
        live ? vt_->variant_buffer() : vt_dummy_storage_.buffer, 0,
        live ? vt_->variant_buffer_size() : VkDeviceSize{64}};
    const bool feedback_live = live && vt_->feedback_view() != VK_NULL_HANDLE;
    VkDescriptorImageInfo feedback_info{
        VK_NULL_HANDLE,
        feedback_live ? vt_->feedback_view() : vt_dummy_feedback_.view,
        VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[4]{};
    for (VkWriteDescriptorSet& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frame.descriptor_sets[1];
        write.descriptorCount = 1;
    }
    writes[0].dstBinding = 10;
    writes[0].descriptorCount = vt::kVtChannelCount;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = pool_infos;
    writes[1].dstBinding = 11;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &indirection_info;
    writes[2].dstBinding = 12;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &variants_info;
    writes[3].dstBinding = 13;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = &feedback_info;
    vkUpdateDescriptorSets(vulkan_->device(), 4, writes, 0, nullptr);
}

void VkSceneRenderer::vt_begin_frame(FrameResources& frame,
                                     uint32_t frame_slot) {
    if (!vt_ || !vt_->available()) return;
    ++vt_frame_serial_;
    // Both of these must run OUTSIDE any active command-buffer recording and
    // while no fill can still be unretired -- this hook is the one place per
    // frame that is true.
    push_vt_compositor_inputs();
    drain_vt_invalidations(vt_frame_serial_);
    vt_->begin_frame(vt_frame_serial_, frame_slot);
    // Tail-gate activations: variants whose tail fill landed last frame may
    // now enter the VT path, so the (cluster, lod) -> vt_slot table must be
    // republished (it is otherwise only rebuilt on registration/release).
    if (vt_->consume_activation_dirty()) vt_draw_slots_dirty_ = true;
    std::string error;
    if (raster_extent_.width != 0 && raster_extent_.height != 0 &&
        !vt_->ensure_feedback(raster_extent_.width, raster_extent_.height,
                              error)) {
        // Feedback is an optimization: without it nothing new becomes
        // resident, but every variant still samples its pinned tail.
        std::fprintf(stderr, "[vk] VT feedback target unavailable: %s\n",
                     error.c_str());
        std::fflush(stderr);
    }
    write_vt_descriptors_for_frame(frame);
}

void VkSceneRenderer::vt_record_pre_pass(VkCommandBuffer command_buffer) {
    if (!vt_ || !vt_->available()) return;
    std::string error;
    if (!vt_->record_frame(command_buffer, error)) {
        std::fprintf(stderr, "[vk] VT frame record failed: %s\n",
                     error.c_str());
        std::fflush(stderr);
    }
    vt_->record_feedback_clear(command_buffer);
}

void VkSceneRenderer::vt_record_post_pass(VkCommandBuffer command_buffer) {
    if (!vt_ || !vt_->available()) return;
    vt_->record_feedback_readback(command_buffer);
}

namespace {
// 6 = VkSceneRenderer::kTilesetChannelCount (private enum; this is a free
// function in an anonymous namespace, so the literal is repeated here rather
// than referenced -- matches the pre-existing style, which used a literal 4
// before the horizon channels were added).
struct TilesetUploadRecord {
    VkBuffer staging = VK_NULL_HANDLE;
    VkImage images[6]{};
    uint32_t mip_counts[6]{};
    std::vector<VkBufferImageCopy> regions[6];
    bool rt_available = false;
};

void record_tileset_upload(VkCommandBuffer cmd, void* user_data) {
    auto& rec = *static_cast<TilesetUploadRecord*>(user_data);
    const VkPipelineStageFlags2 dst_stage =
        vk_scene_detail::ray_depth_destination_stages(rec.rt_available);
    for (int c = 0; c < 6; ++c) {
        if (rec.images[c] == VK_NULL_HANDLE || rec.regions[c].empty()) continue;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                            rec.mip_counts[c], 0, 16};
        VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_dst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_dst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = rec.images[c];
        to_dst.subresourceRange = range;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_dst;
        vkCmdPipelineBarrier2(cmd, &dep);

        vkCmdCopyBufferToImage(cmd, rec.staging, rec.images[c],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(rec.regions[c].size()),
                               rec.regions[c].data());

        VkImageMemoryBarrier2 to_read = to_dst;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = dst_stage;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }
}
}  // namespace

bool VkSceneRenderer::load_tileset_slot(int slot, const std::string& gtex_path,
                                        std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (slot < 0 || slot >= tileset::kMaxTilesetSlots) {
        error = "load_tileset_slot: slot " + std::to_string(slot) +
                " out of range [0," +
                std::to_string(tileset::kMaxTilesetSlots) + ")";
        return false;
    }
    if (gtex_path.empty()) {
        error = "load_tileset_slot: empty gtex_path";
        return false;
    }
    // Lazy-init, matching every other GPU entry point (render_frame,
    // upload_scene, ...): init() is NOT called at construction. The deferred
    // tileset phase runs before the first frame is ever rendered, so this call
    // always lost the race and the very first ground atlas of a session failed
    // with "not initialized" — the .gtex baked fine and then was never
    // uploaded, leaving the ground untextured. init() is idempotent
    // (returns true early when already initialized) and itself calls
    // ensure_tileset_infra(), so this covers both flags. Safe here: the
    // provider marshals the slot load onto the app/Vulkan thread via gpu_run.
    if (!initialized_ && !init(error)) return false;
    if (!tileset_infra_ready_ && !ensure_tileset_infra(error)) return false;

    tileset::GTexHeader header;
    std::vector<uint8_t> albedo_rgb8, normal_rg8, orm_rgb8;
    std::vector<uint16_t> height_r16;
    // Phase 2 (horizon-map lighting), .gtex v2: two additional RGBA8
    // out-params carrying CHAN_HORIZON_A/B (8 packed azimuth directions,
    // quarter albedo resolution). Call site matches the 8-argument
    // tileset::load_gtex overload landed in tileset_gtex.h (horizon_a/b
    // appended after height_r16_out, before err) -- cross-checked against
    // tileset_gtex_tests.cpp's call sites. v1 .gtex files leave both
    // vectors empty (that overload's documented v1 behavior), which this
    // function treats as "no horizon data" below, not an error.
    std::vector<uint8_t> horizon_a, horizon_b;
    if (!tileset::load_gtex(gtex_path, header, albedo_rgb8, normal_rg8,
                            orm_rgb8, height_r16, horizon_a, horizon_b,
                            error)) {
        return false;
    }

    // .gtex atlases are always square with a 4x4 tile grid (tileset_gtex.h);
    // load_gtex doesn't return pixel dimensions directly, so derive + cross-
    // validate from the decoded buffer sizes (fail-closed on any mismatch —
    // never trust a corrupt/foreign .gtex enough to compute a bogus stride).
    if (height_r16.empty() || albedo_rgb8.size() % 3 != 0) {
        error = "load_tileset_slot: empty or malformed .gtex channel data: " +
                gtex_path;
        return false;
    }
    const size_t pixel_count = height_r16.size();
    const int atlas_dim = static_cast<int>(
        std::lround(std::sqrt(static_cast<double>(pixel_count))));
    if (atlas_dim <= 0 || static_cast<size_t>(atlas_dim) * atlas_dim != pixel_count ||
        atlas_dim % 4 != 0 ||
        albedo_rgb8.size() != pixel_count * 3 ||
        normal_rg8.size() != pixel_count * 2 ||
        orm_rgb8.size() != pixel_count * 3) {
        error = "load_tileset_slot: atlas dimension/channel-size mismatch: " +
                gtex_path;
        return false;
    }
    const int tile_px = atlas_dim / 4;

    // Horizon channels (v2 only): quarter albedo resolution, RGBA8 (4
    // bytes/pixel), same square/4x4-grid contract as the core channels but
    // at their own (smaller) atlas_dim/tile_px. Empty vectors (v1 file, or a
    // malformed v2 file missing one of the pair) fail CLOSED to "no horizon
    // data" -- has_horizon stays false and the slot loads normally with the
    // horizon channels bound to the shared dummy, never a hard error, since
    // horizon-map lighting is an optional enhancement layer.
    //
    // header.horizon_w_px/h_px (tileset_gtex.h, v2) give the atlas
    // dimensions directly rather than requiring another sqrt-derivation
    // from the decoded vector size, but are still cross-validated against
    // the actual horizon_a/horizon_b byte counts below -- never trust a
    // corrupt/foreign header field enough to compute a bogus stride.
    const bool has_horizon = !horizon_a.empty() && !horizon_b.empty();
    int horizon_atlas_dim = 0;
    int horizon_tile_px = 0;
    if (has_horizon) {
        horizon_atlas_dim = header.horizon_w_px;
        if (horizon_a.size() != horizon_b.size() ||
            horizon_atlas_dim <= 0 || header.horizon_h_px != horizon_atlas_dim ||
            horizon_atlas_dim % 4 != 0 ||
            horizon_a.size() !=
                static_cast<size_t>(horizon_atlas_dim) *
                    static_cast<size_t>(horizon_atlas_dim) * 4) {
            error = "load_tileset_slot: horizon atlas dimension mismatch: " +
                    gtex_path;
            return false;
        }
        horizon_tile_px = horizon_atlas_dim / 4;
    }

    tileset::SlicedChannel sliced_albedo, sliced_normal, sliced_orm, sliced_height;
    tileset::SlicedChannel sliced_horizon_a, sliced_horizon_b;
    if (!tileset::slice_channel(albedo_rgb8.data(), atlas_dim, atlas_dim, 3,
                                /*expand_rgb_to_rgba=*/true,
                                /*filter_as_u16=*/false, sliced_albedo, error) ||
        !tileset::slice_channel(normal_rg8.data(), atlas_dim, atlas_dim, 2,
                                /*expand_rgb_to_rgba=*/false,
                                /*filter_as_u16=*/false, sliced_normal, error) ||
        !tileset::slice_channel(orm_rgb8.data(), atlas_dim, atlas_dim, 3,
                                /*expand_rgb_to_rgba=*/true,
                                /*filter_as_u16=*/false, sliced_orm, error) ||
        !tileset::slice_channel(reinterpret_cast<const uint8_t*>(height_r16.data()),
                                atlas_dim, atlas_dim, 2,
                                /*expand_rgb_to_rgba=*/false,
                                /*filter_as_u16=*/true, sliced_height, error)) {
        return false;
    }
    if (has_horizon &&
        (!tileset::slice_channel(horizon_a.data(), horizon_atlas_dim,
                                 horizon_atlas_dim, 4,
                                 /*expand_rgb_to_rgba=*/false,
                                 /*filter_as_u16=*/false, sliced_horizon_a,
                                 error) ||
         !tileset::slice_channel(horizon_b.data(), horizon_atlas_dim,
                                 horizon_atlas_dim, 4,
                                 /*expand_rgb_to_rgba=*/false,
                                 /*filter_as_u16=*/false, sliced_horizon_b,
                                 error))) {
        return false;
    }
    tileset::SlicedChannel* slices[4] = {&sliced_albedo, &sliced_normal,
                                        &sliced_orm, &sliced_height};
    for (tileset::SlicedChannel* s : slices) {
        if (s->tile_px != tile_px || s->mip_count <= 0 ||
            static_cast<int>(s->layers.size()) != 16) {
            error = "load_tileset_slot: slicer output shape mismatch: " +
                    gtex_path;
            return false;
        }
    }
    tileset::SlicedChannel* horizon_slices[2] = {&sliced_horizon_a,
                                                 &sliced_horizon_b};
    if (has_horizon) {
        for (tileset::SlicedChannel* s : horizon_slices) {
            if (s->tile_px != horizon_tile_px || s->mip_count <= 0 ||
                static_cast<int>(s->layers.size()) != 16) {
                error = "load_tileset_slot: horizon slicer output shape "
                        "mismatch: " + gtex_path;
                return false;
            }
        }
    }
    // --- Block-compress the four core channels ----------------------------
    //
    // Mip FIRST (above, on the CPU, per layer), compress SECOND. The order
    // matters: mipping compressed data would require a decode/re-encode per
    // level and would lose the property the Wang tiling depends on.
    //
    // The seam invariant survives compression exactly. Color-matched tile
    // edges are byte-identical over a >= 4-texel strip at mip 0
    // (tileset_slicer.h), tile_px is a multiple of 4, and BC blocks are 4x4
    // and encoded independently of one another — so the boundary blocks of
    // two matching layers get identical inputs and, because the encoder is
    // deterministic, identical outputs. Asserted directly in
    // tests/bc_encode_tests.cpp (test_edge_strip_survives_compression); see
    // bc_encode.h's header for the full argument.
    //
    // Per-channel choice (VRAM per texel, mip-0):
    //   albedo  RGBA8 4 B -> BC7  1   B   (RGB + constant alpha)
    //   normal  RG8   2 B -> BC5  1   B   (two independent unorm channels)
    //   ORM     RGBA8 4 B -> BC7  1   B
    //   height  R16   2 B -> BC4  0.5 B   (requantized to R8 first; the unorm
    //                                      mapping onto [height_min,height_max]
    //                                      is preserved, so no shader change)
    // Horizon A/B stay uncompressed RGBA8: they are quarter-resolution and
    // already ~1/16 the cost of a core channel, and BC7 on four packed,
    // unrelated elevation scalars would trade real accuracy for nothing.
    //
    // Device support: BC is gated by the textureCompressionBC feature, which
    // every desktop GPU this engine targets has. Probe it anyway and fail
    // CLOSED with a legible message rather than letting vkCreateImage return
    // a format error deep inside create_tileset_image (the caller's contract
    // is "slot load may fail; that slot's ground stays untextured").
    {
        const VkFormat probe[3] = {VK_FORMAT_BC7_UNORM_BLOCK,
                                   VK_FORMAT_BC5_UNORM_BLOCK,
                                   VK_FORMAT_BC4_UNORM_BLOCK};
        const char* probe_name[3] = {"BC7_UNORM_BLOCK", "BC5_UNORM_BLOCK",
                                     "BC4_UNORM_BLOCK"};
        for (int i = 0; i < 3; ++i) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(vulkan_->physical_device(),
                                                probe[i], &props);
            const VkFormatFeatureFlags needed =
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if ((props.optimalTilingFeatures & needed) != needed) {
                error = std::string("load_tileset_slot: device does not "
                                    "support sampling ") + probe_name[i] +
                        " (textureCompressionBC missing?): " + gtex_path;
                return false;
            }
        }
    }

    static const tileset::BcFormat kChannelBc[4] = {
        tileset::BcFormat::kBc7,  // kTilesetChannelAlbedo
        tileset::BcFormat::kBc5,  // kTilesetChannelNormal
        tileset::BcFormat::kBc7,  // kTilesetChannelOrm
        tileset::BcFormat::kBc4,  // kTilesetChannelHeight
    };
    tileset::CompressedChannel compressed[4];
    size_t uncompressed_core_bytes = 0;
    for (int c = 0; c < 4; ++c) {
        uncompressed_core_bytes += tileset::sliced_channel_bytes(*slices[c]);
        if (!tileset::compress_sliced_channel(
                *slices[c], kChannelBc[c],
                /*src_is_r16le=*/c == kTilesetChannelHeight, compressed[c],
                error)) {
            error = "load_tileset_slot: " + error + ": " + gtex_path;
            return false;
        }
        // Release the uncompressed slice as soon as its compressed form
        // exists. At a 4096 atlas an RGBA8 core channel is ~90 MiB, so
        // holding all four alive alongside their compressed forms would peak
        // ~350 MiB for nothing. Nothing below reads *slices[c] again — the
        // upload loop uses compressed[c] and mean_rgb() reads the raw
        // albedo_rgb8 source, not the slice.
        *slices[c] = tileset::SlicedChannel{};
    }

    // Index order matches TilesetChannel (albedo/normal/orm/height/
    // horizon_a/horizon_b). The four core channels are block-compressed; the
    // horizon pair stays RGBA8 (see the comment above).
    const VkFormat formats[kTilesetChannelCount] = {
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};

    TilesetImage new_images[kTilesetChannelCount];
    bool created_ok = true;
    for (int c = 0; c < 4 && created_ok; ++c) {
        created_ok = create_tileset_image(
            formats[c], static_cast<uint32_t>(tile_px),
            static_cast<uint32_t>(compressed[c].mip_count), 16, new_images[c],
            error);
    }
    if (created_ok && has_horizon) {
        for (int c = 0; c < 2 && created_ok; ++c) {
            created_ok = create_tileset_image(
                formats[kTilesetChannelHorizonA + c],
                static_cast<uint32_t>(horizon_tile_px),
                static_cast<uint32_t>(horizon_slices[c]->mip_count), 16,
                new_images[kTilesetChannelHorizonA + c], error);
        }
    }
    if (!created_ok) {
        for (auto& image : new_images) destroy_tileset_image(image);
        return false;
    }

    // Build the single shared staging blob (all channels/layers/mips) and the
    // per-image copy regions in one pass, so the offsets the regions record
    // and the bytes actually written can never drift apart.
    TilesetUploadRecord upload_record;
    upload_record.rt_available = vulkan_->ray_tracing_available();
    std::vector<uint8_t> staging_bytes;

    // layers[layer][mip] -> staging bytes + one VkBufferImageCopy per
    // (layer, mip). expected_bytes(dim) returns the exact size that mip level
    // must have; a mismatch is a hard fail (never upload a short/long level).
    auto append_channel = [&](int channel, int base_dim, int mip_count,
                              const auto& layers, auto&& expected_bytes,
                              const char* label) -> bool {
        upload_record.mip_counts[channel] = static_cast<uint32_t>(mip_count);
        auto& regions = upload_record.regions[channel];
        regions.reserve(static_cast<size_t>(mip_count) * 16);
        for (int layer = 0; layer < 16; ++layer) {
            int dim = base_dim;
            for (int mip = 0; mip < mip_count; ++mip) {
                const std::vector<uint8_t>& data =
                    layers[static_cast<size_t>(layer)][static_cast<size_t>(mip)];
                if (data.size() != expected_bytes(dim)) {
                    error = std::string("load_tileset_slot: ") + label +
                            " mip byte-size mismatch (layer " +
                            std::to_string(layer) + " mip " +
                            std::to_string(mip) + "): " + gtex_path;
                    return false;
                }
                // vkCmdCopyBufferToImage requires bufferOffset to be a
                // multiple of 4 AND, for a block-compressed image, of the
                // texel-block size (16 B for BC7/BC5, 8 B for BC4). Aligning
                // every level to 16 satisfies all three at once and costs at
                // most 15 padding bytes per level. (The old code packed
                // tightly, which was fine only because every uncompressed
                // level was already a multiple of 4.)
                const size_t offset = (staging_bytes.size() + 15u) & ~size_t{15u};
                staging_bytes.resize(offset + data.size());
                std::memcpy(staging_bytes.data() + offset, data.data(),
                            data.size());

                VkBufferImageCopy region{};
                region.bufferOffset = static_cast<VkDeviceSize>(offset);
                // bufferRowLength/bufferImageHeight stay 0 = "tightly packed
                // per imageExtent", which for a compressed format means
                // ceil(dim/4) blocks per row — exactly what bc_encode emits.
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = static_cast<uint32_t>(mip);
                region.imageSubresource.baseArrayLayer =
                    static_cast<uint32_t>(layer);
                region.imageSubresource.layerCount = 1;
                // imageExtent is in TEXELS, not blocks, and is legal below the
                // block size precisely because it equals the mip's own extent
                // (the spec's "imageExtent + imageOffset == subresource
                // dimensions" escape from the multiple-of-block-size rule).
                region.imageExtent = {static_cast<uint32_t>(dim),
                                      static_cast<uint32_t>(dim), 1};
                regions.push_back(region);
                dim = std::max(1, dim / 2);
            }
        }
        return true;
    };

    bool appended_ok = true;
    for (int c = 0; c < 4 && appended_ok; ++c) {
        upload_record.images[c] = new_images[c].image;
        const tileset::BcFormat format = kChannelBc[c];
        appended_ok = append_channel(
            c, tile_px, compressed[c].mip_count, compressed[c].layers,
            [format](int dim) {
                return tileset::bc_encoded_size(format, dim, dim);
            },
            tileset::bc_format_name(format));
    }
    if (appended_ok && has_horizon) {
        for (int hc = 0; hc < 2 && appended_ok; ++hc) {
            const int c = kTilesetChannelHorizonA + hc;
            const tileset::SlicedChannel* s = horizon_slices[hc];
            const int bpp = s->bytes_per_pixel;
            upload_record.images[c] = new_images[c].image;
            appended_ok = append_channel(
                c, horizon_tile_px, s->mip_count, s->layers,
                [bpp](int dim) {
                    return static_cast<size_t>(dim) * static_cast<size_t>(dim) *
                           static_cast<size_t>(bpp);
                },
                "horizon");
        }
    }
    if (!appended_ok) {
        for (auto& image : new_images) destroy_tileset_image(image);
        return false;
    }
    if (staging_bytes.empty()) {
        error = "load_tileset_slot: sliced atlas produced zero bytes: " + gtex_path;
        for (auto& image : new_images) destroy_tileset_image(image);
        return false;
    }

    matter::VkBufferResource staging;
    if (!matter::create_buffer(*vulkan_,
                               static_cast<VkDeviceSize>(staging_bytes.size()),
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging,
                               error) ||
        !matter::map_buffer(staging, error)) {
        for (auto& image : new_images) destroy_tileset_image(image);
        return false;
    }
    std::memcpy(staging.mapped, staging_bytes.data(), staging_bytes.size());
    upload_record.staging = staging.buffer;
    if (!matter::flush_buffer(staging, 0, staging_bytes.size(), error)) {
        for (auto& image : new_images) destroy_tileset_image(image);
        return false;
    }
    if (!matter::submit_immediate(*vulkan_, record_tileset_upload,
                                  &upload_record, error,
                                  matter::ImmediateSubmitPhase::staging_upload,
                                  {staging.lifetime})) {
        for (auto& image : new_images) destroy_tileset_image(image);
        return false;
    }

    // Everything succeeded: swap the new images into place. Only now do we
    // touch renderer state / destroy the previous occupant, so an unloaded or
    // still-valid prior slot is never left half-updated on failure.
    //
    // wait_idle is unconditional: the descriptor rewrite below touches EVERY
    // frame slot's raster set 1, and sets referenced by still-pending command
    // buffers must not be updated (no UPDATE_AFTER_BIND on these layouts).
    // Loads are rare (world connect / rebake), so the stall is acceptable.
    vulkan_->wait_idle();
    if (tileset_slots_[slot].loaded) {
        for (auto& channel : tileset_slots_[slot].channels)
            destroy_tileset_image(channel);
    }
    TilesetSlotGpu& target = tileset_slots_[slot];
    target = TilesetSlotGpu{};
    for (int c = 0; c < 4; ++c) target.channels[c] = new_images[c];
    if (has_horizon) {
        target.channels[kTilesetChannelHorizonA] =
            new_images[kTilesetChannelHorizonA];
        target.channels[kTilesetChannelHorizonB] =
            new_images[kTilesetChannelHorizonB];
    }
    target.has_horizon = has_horizon;
    target.tile_size_m = header.tile_size_m;
    target.texels_per_meter = static_cast<float>(header.texels_per_meter);
    target.height_min = header.height_min;
    target.height_max = header.height_max;
    tileset::mean_rgb(albedo_rgb8.data(), atlas_dim, atlas_dim, 3,
                      target.mean_albedo);
    // Phase 0: the same mean over the ORM atlas. gbuffer.frag's near band
    // divides the live detail's occlusion/roughness by these so the VT page
    // keeps its own level and the detail only contributes its deviation --
    // the ORM counterpart of the albedo ratio that shipped with WP-E.
    tileset::mean_rgb(orm_rgb8.data(), atlas_dim, atlas_dim, 3,
                      target.mean_orm);
    target.loaded = true;

    write_tileset_params_buffer();
    for (auto& frame : frames_)
        write_tileset_descriptors_for_frame(frame.descriptor_sets[1]);
    // WP-D/E: the tier-1 compositor samples these same slot images at page
    // bake time; rebind them on the next vt_begin_frame.
    vt_inputs_dirty_ = true;

    // One line, per slot load: what this slot now costs in VRAM and what it
    // would have cost uncompressed. The "before" figure is the exact byte
    // count of the mipped, uncompressed slices this function just compressed,
    // not an estimate. Horizon channels are reported separately because they
    // are deliberately NOT compressed and so appear in both totals unchanged.
    {
        size_t compressed_core_bytes = 0;
        for (int c = 0; c < 4; ++c)
            compressed_core_bytes += tileset::compressed_channel_bytes(compressed[c]);
        size_t horizon_bytes = 0;
        if (has_horizon) {
            for (tileset::SlicedChannel* s : horizon_slices)
                horizon_bytes += tileset::sliced_channel_bytes(*s);
        }
        const double mib = 1.0 / (1024.0 * 1024.0);
        printf("[tileset] slot %d '%s': %dpx x16 layers x%d mips -- core "
               "%.1f MiB BC (was %.1f MiB raw, %.2fx smaller)%s\n",
               slot, gtex_path.c_str(), tile_px, compressed[0].mip_count,
               (double)compressed_core_bytes * mib,
               (double)uncompressed_core_bytes * mib,
               compressed_core_bytes
                   ? (double)uncompressed_core_bytes / (double)compressed_core_bytes
                   : 0.0,
               has_horizon
                   ? (" + horizon " +
                      std::to_string((int)((double)horizon_bytes * mib + 0.5)) +
                      " MiB uncompressed").c_str()
                   : "");
        fflush(stdout);
    }
    return true;
}

void VkSceneRenderer::unload_tileset_slot(int slot) {
    if (slot < 0 || slot >= tileset::kMaxTilesetSlots) return;
    if (!tileset_slots_[slot].loaded) return;
    if (vulkan_) vulkan_->wait_idle();
    for (auto& channel : tileset_slots_[slot].channels)
        destroy_tileset_image(channel);
    tileset_slots_[slot] = TilesetSlotGpu{};
    write_tileset_params_buffer();
    for (auto& frame : frames_)
        write_tileset_descriptors_for_frame(frame.descriptor_sets[1]);
    // WP-D/E: the compositor must stop sampling views this call just
    // destroyed. wait_idle above already retired every in-flight fill, so the
    // rebind on the next vt_begin_frame is safe -- but the views are dangling
    // until then, so push it immediately instead of deferring.
    vt_inputs_dirty_ = true;
    push_vt_compositor_inputs();
}

bool VkSceneRenderer::bake_tileset(const tileset::SettledTorus& settled,
                                   uint64_t script_source_hash,
                                   const std::string& gtex_path,
                                   const tileset::BakeInputs& inputs,
                                   bool force_rebake, bool dump_png,
                                   std::string& error) {
    if (!vulkan_) {
        error = "VkSceneRenderer::bake_tileset: no Vulkan device";
        return false;
    }
    // Q1: thin passthrough to the free function that owns the bake logic.
    return tileset::bake_tileset_vk(*vulkan_, settled, script_source_hash,
                                    gtex_path, inputs, force_rebake, dump_png,
                                    error);
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
    // RT PBR Phase 1: transmission composes from its denoised chain exactly
    // like specular. Smooth pixels (aux roughness < 0.02) pass through both
    // denoiser stages bit-exactly, so existing glass keeps rendering
    // byte-identically; coverage rides the alpha channel end to end.
    matter::VkImageResource* transmission =
        gi_settings_.enabled && gi_candidate_frame_serial_ != 0
            ? (gi_filtered_valid_ ? &gi_trans_atrous_[gi_filtered_index_]
                                  : &gi_trans_history_[gi_composite_history_index_].radiance)
            : &raw_transmission_;
    const bool vol_active = volumetrics_ && volumetrics_->active();
    matter::VkImageResource* sampled[] = {&albedo_, &normal_, &orm_,
                                          &visibility_, diffuse, specular,
                                          &material_instance_,
                                          transmission,
                                          vol_active ? &volumetrics_->vol_integrated()
                                                     : &vol_dummy_3d_,
                                          &depth_};
    const uint32_t sampled_slots[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10};
    VkDescriptorImageInfo image_infos[10]{};
    VkWriteDescriptorSet writes[11]{};
    for (uint32_t i = 0; i < 10; ++i) {
        image_infos[i].sampler = (i == 8) ? vol_linear_sampler_
                                          : composite_sampler_;
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
    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = frame.composite_descriptor_set;
    writes[10].dstBinding = 7;
    writes[10].descriptorCount = 1;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[10].pBufferInfo = &material_info;
    vkUpdateDescriptorSets(vulkan_->device(), 11, writes, 0, nullptr);
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

void VkSceneRenderer::set_test_animation_skin_failure(
    uint32_t fail_after_allocations, uint32_t fail_after_uploads) {
    if (poisoned()) return;
    test_fail_after_skin_allocations_ = fail_after_allocations;
    test_fail_after_skin_uploads_ = fail_after_uploads;
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
    // Phase 1 tileset Vulkan port (Task 6): sampler + dummy images + params
    // UBO must exist before the first ensure_frame_resources() call below,
    // since update_frame_descriptors() always writes raster set 1 bindings
    // 6/7 (dummies until a world loads a real tileset slot).
    if (!ensure_tileset_infra(error)) {
        destroy_pipeline();
        return false;
    }
    // WP-E: same story for scene set 1 bindings 10-13 — update_frame_descriptors
    // always writes them, and the residency runtime only starts once a
    // chart-bearing part registers, so the dummies must exist first.
    if (!ensure_vt_dummies(error)) {
        destroy_pipeline();
        return false;
    }
    bool initialize_volumetrics = true;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    // The direct C2 skin fixture deliberately avoids optional ray-query
    // modules as well as RT pipelines; it exercises only the skin compute
    // ABI and must be valid on a non-ray-query logical device.
    initialize_volumetrics =
        !test_force_rt_unavailable_ && !test_skip_volumetrics_;
#endif
    if (initialize_volumetrics && !volumetrics_) {
        auto vol = std::make_unique<VkVolumetrics>();
        std::string vol_error;
        if (vol->init(*vulkan_, vol_error)) {
            volumetrics_ = std::move(vol);
        } else {
            // Volumetrics are optional, but a failed init must not be silent:
            // every downstream symptom (empty froxel volume, dead debug
            // views) is otherwise indistinguishable from "disabled".
            std::fprintf(stderr,
                         "[vk] volumetrics init FAILED (volumetrics disabled): %s\n",
                         vol_error.c_str());
            std::fflush(stderr);
        }
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
        ensure_vertex_buffer(sizeof(VkRasterVertex), error) &&
        ensure_index_buffer(sizeof(uint32_t), error);
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
    // Deferred-fill admission (see command_template_dirty_): a registration
    // adds clusters but no instances, so of rebuild_command_template's
    // failure modes only the command-buffer limits can newly trip — and those
    // are O(1) against the would-be cluster total. The per-part bucket and
    // transform-slot overflow terms depend on instance counts, which this
    // call leaves untouched, so the last successful rebuild still vouches for
    // them and the deferred fill cannot fail for a reason this registration
    // introduced.
    {
        VkDeviceSize admitted_command_bytes = 0;
        if (!vk_scene_detail::checked_mul_to_device_size(
                combined_clusters * static_cast<size_t>(kVkMaxLod),
                sizeof(DrawCommand), admitted_command_bytes,
                "draw-command buffer", error)) {
            return -1;
        }
        const VkDeviceSize storage_limit =
            std::min(limits_.max_storage_buffer_range, limits_.max_buffer_size);
        if (storage_limit != 0 && admitted_command_bytes > storage_limit) {
            error = "draw-command buffer exceeds Vulkan storage descriptor limit";
            return -1;
        }
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
    std::shared_ptr<matter::VkBufferResource> rt_index;
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
    if (vulkan_->ray_tracing_available() && !part.indices.empty()) {
        rt_index = std::make_shared<matter::VkBufferResource>();
        const VkDeviceSize index_bytes =
            static_cast<VkDeviceSize>(part.indices.size()) * sizeof(uint32_t);
        if (!matter::create_buffer(
                *vulkan_, index_bytes,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, *rt_index, error) ||
            !matter::map_buffer(*rt_index, error)) {
            return -1;
        }
        std::memcpy(rt_index->mapped, part.indices.data(),
                    static_cast<size_t>(index_bytes));
        if (!matter::flush_buffer(*rt_index, 0, index_bytes, error)) return -1;
    }
    // Place the part's geometry: reuse a settled freed range when one fits
    // (steady-state streaming: an evicted sector's range carries the next
    // one), otherwise extend the tail as before.
    settle_free_ranges();
    const uint32_t vertex_base =
        allocate_vertex_range(static_cast<uint32_t>(part.vertices.size()));
    if (!part.vertices.empty())
        std::copy(part.vertices.begin(), part.vertices.end(),
                  vertex_staging_.begin() + vertex_base);
    const uint32_t index_base =
        allocate_index_range(static_cast<uint32_t>(part.indices.size()));
    if (!part.indices.empty())
        std::copy(part.indices.begin(), part.indices.end(),
                  index_staging_.begin() + index_base);
    const uint32_t cluster_base =
        allocate_cluster_range(static_cast<uint32_t>(part.clusters.size()));
    const int slot = static_cast<int>(parts_.size());
    PartRecord record{};
    record.hash = part.part_hash;
    record.cluster_start = cluster_base;
    record.cluster_count = static_cast<uint32_t>(part.clusters.size());
    record.vertex_start = vertex_base;   // kept for Task 4 vertexOffset
    record.vertex_count = static_cast<uint32_t>(part.vertices.size());
    record.index_start = index_base;
    record.index_count = static_cast<uint32_t>(part.indices.size());
    record.live = true;
    record.rt_geometry = std::move(rt_geometry);
    record.rt_index = std::move(rt_index);
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
            // Store part-local first_index; compaction does not touch rt_lods,
            // so the part-local frame keeps consumers correct after release_part.
            rt_lod.first_index = lod.first_index;
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
        cluster_staging_[cluster_base + i] = cluster;
        std::vector<VkSceneLod> lods = source.lods;
        if (!part.indices.empty()) {
            // Rebase part-local first_index to global index_staging_ offset.
            // Index VALUES are part-local and are never rewritten here.
            for (auto& lod : lods) lod.first_index += index_base;
        }
        cluster_lods_[cluster_base + i] = std::move(lods);
    }
    parts_.push_back(record);
    slot_of_[part.part_hash] = slot;
    // Retires the flat lookup mirror and update_instances()' input snapshot in
    // one integer (see slot_of_version_).
    ++slot_of_version_;
    // WP-E: chart-bearing rungs get a VT registration (and start the residency
    // runtime on first use). A part with no charts leaves vt_slots all zero,
    // which is exactly the legacy path.
    register_vt_part(slot, part);
    vt_draw_slots_dirty_ = true;
    // A new slot lengthens the per-slot override table (and may itself carry
    // an override); no-op work when nothing is overridden.
    part_draw_overrides_dirty_ = true;
    // Record the written ranges (interior when a freed range was reused, tail
    // otherwise) for the ranged upload path.
    if (record.cluster_count != 0)
        dirty_cluster_ranges_.push_back({cluster_base, record.cluster_count});
    if (record.vertex_count != 0)
        dirty_vertex_ranges_.push_back({vertex_base, record.vertex_count});
    if (record.index_count != 0)
        dirty_index_ranges_.push_back({index_base, record.index_count});
    // The O(clusters x LODs) template fill is deferred: the admission check
    // above already proved the fill cannot fail on this registration's
    // account, so a frame that registers several streamed parts pays for one
    // rebuild (in update_instances' layout path or flush_command_template)
    // instead of one per part.
    command_template_dirty_ = true;
    ++static_generation_;
    // Ranged write: an interior (reused) range is safe to write in place
    // because its old bytes went unreferenced for a full in-flight window
    // before the allocator handed it out; a tail range is safe because no
    // recorded frame reads past its old size.
    mark_static_append();
    return slot;
}

void VkSceneRenderer::refresh_part_slot_index() const {
    // Power-of-two, at least 2x occupancy, so linear probing stays short.
    size_t capacity = 16;
    while (capacity < slot_of_.size() * 2) capacity *= 2;
    part_slot_keys_.assign(capacity, 0);
    part_slot_entries_.assign(capacity, -1);
    part_slot_mask_ = static_cast<uint32_t>(capacity - 1);
    for (const auto& entry : slot_of_) {
        // Fibonacci mix: part hashes are already well distributed, but the low
        // bits are what the mask keeps and one multiply is free next to a
        // cache miss.
        uint32_t slot = static_cast<uint32_t>(
                            (entry.first * 0x9E3779B97F4A7C15ull) >> 32) &
                        part_slot_mask_;
        while (part_slot_entries_[slot] >= 0) slot = (slot + 1) & part_slot_mask_;
        part_slot_keys_[slot] = entry.first;
        part_slot_entries_[slot] = static_cast<int32_t>(entry.second);
    }
    part_slot_index_version_ = slot_of_version_;
}

int VkSceneRenderer::part_slot_lookup(uint64_t part_hash) const {
    if (!slot_index_enabled()) {
        const auto found = slot_of_.find(part_hash);
        return found == slot_of_.end() ? -1 : found->second;
    }
    if (part_slot_index_version_ != slot_of_version_ ||
        part_slot_entries_.empty())
        refresh_part_slot_index();
    uint32_t slot =
        static_cast<uint32_t>((part_hash * 0x9E3779B97F4A7C15ull) >> 32) &
        part_slot_mask_;
    while (true) {
        const int32_t entry = part_slot_entries_[slot];
        if (entry < 0) return -1;
        if (part_slot_keys_[slot] == part_hash) return entry;
        slot = (slot + 1) & part_slot_mask_;
    }
}

bool VkSceneRenderer::part_raster_range(
    uint64_t part_hash, uint32_t& vertex_start, uint32_t& vertex_count,
    uint32_t& index_start, uint32_t& index_count) const noexcept {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end() || found->second < 0 ||
        static_cast<size_t>(found->second) >= parts_.size()) return false;
    const PartRecord& part = parts_[static_cast<size_t>(found->second)];
    if (!part.live || part.hash != part_hash) return false;
    vertex_start = part.vertex_start;
    vertex_count = part.vertex_count;
    index_start = part.index_start;
    index_count = part.index_count;
    return true;
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
    // WP-D/E: the compositor bakes albedo/ORM and the detail-slot binding into
    // pages, so a material table change has to reach it too. Deferred to the
    // next vt_begin_frame (the setter wants the device idle w.r.t. fills).
    vt_inputs_dirty_ = true;
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
    return record_test_surface_ray(frame, origin, direction, invalid_part_slot,
                                   0.0f, 0.0f, error);
}

bool VkSceneRenderer::record_test_surface_ray(
    const matter::VulkanFrame& frame, matter::Float3 origin,
    matter::Float3 direction, uint32_t invalid_part_slot, float cone_width,
    float cone_spread, std::string& error) {
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
        float cone[4];   // WP-G: (width at origin, spread angle, 0, 0)
    } constants{};
    constants.cone[0] = cone_width;
    constants.cone[1] = cone_spread;
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
    // raw_transmission_ belongs in this list: the descriptor set bound below is
    // the same one record_ray_trace_dispatch uses, and it declares binding 14
    // (raw_transmission_image) as a GENERAL storage image. The post-trace
    // transitions leave that image in SHADER_READ_ONLY_OPTIMAL, so omitting it
    // here traced with a layout the descriptor contradicted. Same argument for
    // raw_transmission_aux_ (binding 20, RT PBR Phase 1).
    for (auto* image : {&raw_specular_, &raw_specular_aux_, &raw_transmission_,
                        &raw_transmission_aux_})
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
                                   29 * sizeof(uint32_t), error) ||
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
    // WP-G: words 18/19 are rt_lighting.rgen's reflection-lobe counters.
    hit.vt_slot = words[20];
    hit.vt_applied = words[21] != 0u;
    hit.vt_albedo = {as_float(words[22]), as_float(words[23]),
                     as_float(words[24])};
    hit.vt_desired_mip = as_float(words[25]);
    hit.vt_mapped_mip = as_float(words[26]);
    hit.cone_width = as_float(words[27]);
    hit.uv_density = as_float(words[28]);
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

bool VkSceneRenderer::test_dispatch_animation_skin_fixture(
    const VkAnimationSkinGpuFixture& fixture,
    VkAnimationSkinGpuResult& result, std::string& error) {
    result = {};
    error.clear();
    if (!initialized_ || skin_pipeline_ == VK_NULL_HANDLE ||
        skin_pipeline_layout_ == VK_NULL_HANDLE ||
        skin_set_layout_ == VK_NULL_HANDLE || fixture.source.empty() ||
        fixture.influences.empty() || fixture.current_palette.empty() ||
        fixture.previous_palette.empty() || fixture.work.empty() ||
        fixture.current_palette.size() != fixture.previous_palette.size()) {
        error = "animation skin GPU fixture requires initialized nonempty streams";
        return false;
    }

    uint64_t current_count = 0;
    uint64_t previous_count = 0;
    uint32_t max_vertices = 0;
    for (const VkSkinWorkItem& work : fixture.work) {
        const uint32_t palette_count =
            work.flags >> kVkSkinPaletteCountShift;
        if (work.vertex_count == 0 || palette_count == 0 ||
            work.source_vertex > fixture.source.size() ||
            work.vertex_count > fixture.source.size() - work.source_vertex ||
            work.influence > fixture.influences.size() ||
            work.vertex_count > fixture.influences.size() - work.influence ||
            work.palette > fixture.current_palette.size() ||
            palette_count > fixture.current_palette.size() - work.palette) {
            error = "animation skin GPU fixture has an invalid work range";
            return false;
        }
        current_count = std::max<uint64_t>(
            current_count,
            static_cast<uint64_t>(work.output_current) + work.vertex_count);
        previous_count = std::max<uint64_t>(
            previous_count,
            static_cast<uint64_t>(work.output_previous) + work.vertex_count);
        max_vertices = std::max(max_vertices, work.vertex_count);
    }
    const auto& skin_budget = animation_skinning_.budget_config();
    if (fixture.work.size() > skin_budget.max_skin_work_items ||
        current_count == 0 || previous_count == 0 ||
        current_count > skin_budget.max_skinned_vertices ||
        previous_count > skin_budget.max_skinned_vertices) {
        error = "animation skin GPU fixture exceeds C2 queue bounds";
        return false;
    }
    const uint32_t groups_x = (max_vertices + 63u) / 64u;
    if (groups_x == 0 || groups_x > limits_.max_dispatch_group_count_x) {
        error = "animation skin GPU fixture dispatch exceeds device limit";
        return false;
    }

    const auto bytes = [](size_t count, size_t stride) -> VkDeviceSize {
        return static_cast<VkDeviceSize>(count) * static_cast<VkDeviceSize>(stride);
    };
    const VkBufferUsageFlags input_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferUsageFlags output_usage = input_usage;
    matter::VkBufferResource source;
    matter::VkBufferResource influences;
    matter::VkBufferResource current_palette;
    matter::VkBufferResource previous_palette;
    matter::VkBufferResource work;
    matter::VkBufferResource current_output;
    matter::VkBufferResource previous_output;
    const auto create = [&](matter::VkBufferResource& buffer,
                            VkDeviceSize size, VkBufferUsageFlags usage) {
        return matter::create_buffer(
            *vulkan_, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            buffer, error);
    };
    if (!create(source, bytes(fixture.source.size(), sizeof(VkSkinSourceVertex)),
                input_usage) ||
        !create(influences, bytes(fixture.influences.size(), sizeof(VkSkinInfluence)),
                input_usage) ||
        !create(current_palette,
                bytes(fixture.current_palette.size(), sizeof(VkSkinJoint)),
                input_usage) ||
        !create(previous_palette,
                bytes(fixture.previous_palette.size(), sizeof(VkSkinJoint)),
                input_usage) ||
        !create(work, bytes(fixture.work.size(), sizeof(VkSkinWorkItem)),
                input_usage) ||
        !create(current_output,
                bytes(static_cast<size_t>(current_count), sizeof(VkSkinVertex)),
                output_usage) ||
        !create(previous_output,
                bytes(static_cast<size_t>(previous_count), sizeof(VkSkinVertex)),
                output_usage)) return false;
    result.current.resize(static_cast<size_t>(current_count));
    result.previous.resize(static_cast<size_t>(previous_count));
    const auto upload = [&](matter::VkBufferResource& buffer, const void* data,
                            VkDeviceSize size) {
        return matter::upload_buffer(*vulkan_, buffer, data,
                                     static_cast<size_t>(size), 0, error);
    };
    if (!upload(source, fixture.source.data(), source.size) ||
        !upload(influences, fixture.influences.data(), influences.size) ||
        !upload(current_palette, fixture.current_palette.data(),
                current_palette.size) ||
        !upload(previous_palette, fixture.previous_palette.data(),
                previous_palette.size) ||
        !upload(work, fixture.work.data(), work.size) ||
        !upload(current_output, result.current.data(), current_output.size) ||
        !upload(previous_output, result.previous.data(), previous_output.size))
        return false;

    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(vulkan_->device(), &pool_info, nullptr, &pool) !=
        VK_SUCCESS) {
        error = "vkCreateDescriptorPool(animation skin fixture) failed";
        return false;
    }
    const auto destroy_pool = [&]() {
        if (pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(vulkan_->device(), pool, nullptr);
    };
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &skin_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vulkan_->device(), &allocate, &set) != VK_SUCCESS) {
        destroy_pool();
        error = "vkAllocateDescriptorSets(animation skin fixture) failed";
        return false;
    }
    const std::array<matter::VkBufferResource*, 7> buffers{
        &source, &influences, &current_palette, &previous_palette, &work,
        &current_output, &previous_output};
    std::array<VkDescriptorBufferInfo, 7> infos{};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t index = 0; index != buffers.size(); ++index) {
        infos[index] = {buffers[index]->buffer, 0, buffers[index]->size};
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = set;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &infos[index];
    }
    vkUpdateDescriptorSets(vulkan_->device(), static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    struct Record {
        VkSceneRenderer* renderer;
        VkDescriptorSet set;
        VkBuffer current;
        VkBuffer previous;
        uint32_t groups_x;
        uint32_t work_count;
    } record{this, set, current_output.buffer, previous_output.buffer,
             groups_x, static_cast<uint32_t>(fixture.work.size())};
    const auto callback = [](VkCommandBuffer command_buffer, void* opaque) {
        const Record& item = *static_cast<const Record*>(opaque);
        VkSceneRenderer& renderer = *item.renderer;
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          renderer.skin_pipeline_);
        // animation_skin.comp statically uses only set 2. Binding it at its
        // production set index proves this fixture exercises the renderer's
        // real ABI instead of a look-alike standalone compute layout.
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                renderer.skin_pipeline_layout_, 2, 1,
                                &item.set, 0, nullptr);
        vkCmdPushConstants(command_buffer, renderer.skin_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(item.work_count), &item.work_count);
        vkCmdDispatch(command_buffer, item.groups_x, item.work_count, 1);
        VkBufferMemoryBarrier2 barriers[2]{};
        for (VkBufferMemoryBarrier2& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            // Keep the production raster-consumer dependency and add the host
            // consumer used by this readback fixture.
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
        }
        barriers[0].buffer = item.current;
        barriers[1].buffer = item.previous;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 2;
        dependency.pBufferMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(command_buffer, &dependency);
    };
    bool submitted = matter::submit_immediate(
        *vulkan_, callback, &record, error,
        matter::ImmediateSubmitPhase::compute_dispatch,
        {source.lifetime, influences.lifetime, current_palette.lifetime,
         previous_palette.lifetime, work.lifetime, current_output.lifetime,
         previous_output.lifetime});
    if (submitted)
        submitted = matter::readback_buffer(*vulkan_, current_output,
                                             result.current.data(),
                                             current_output.size, 0, error) &&
                    matter::readback_buffer(*vulkan_, previous_output,
                                             result.previous.data(),
                                             previous_output.size, 0, error);
    destroy_pool();
    if (!submitted) result = {};
    return submitted;
}

bool VkSceneRenderer::test_readback_animation_skin_output(
    uint32_t frame_slot, uint32_t vertex_count,
    std::vector<VkSkinVertex>& output, std::string& error) {
    output.clear();
    if (frame_slot >= frames_.size() || vertex_count == 0) {
        error = "animation skin output readback has an invalid frame slot or count";
        return false;
    }
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(vertex_count) * sizeof(VkSkinVertex);
    if (frames_[frame_slot].skin_current_output.size < bytes) {
        error = "animation skin output readback exceeds the frame buffer";
        return false;
    }
    output.resize(vertex_count);
    return matter::readback_buffer(
        *vulkan_, frames_[frame_slot].skin_current_output, output.data(),
        bytes, 0, error);
}
#endif

bool VkSceneRenderer::record_gi_temporal(const matter::VulkanFrame& frame,
                                         std::string& error, bool retain) {
    return record_gi_temporal_signal(frame, 0u, error, retain) &&
           record_gi_temporal_signal(frame, 1u, error, retain) &&
           record_gi_temporal_signal(frame, 2u, error, retain);
}

bool VkSceneRenderer::record_gi_temporal_signal(
    const matter::VulkanFrame& frame, uint32_t signal_mode,
    std::string& error, bool retain) {
    if (frame.frame_slot >= frames_.size() ||
        gi_temporal_pipeline_ == VK_NULL_HANDLE)
        return true;
    const uint32_t previous_index = gi_presented_history_index_;
    const uint32_t candidate_index = previous_index ^ 1u;
    GiHistorySet* histories = signal_mode == 0u   ? gi_history_
                              : signal_mode == 1u ? gi_spec_history_
                                                  : gi_trans_history_;
    GiHistorySet& previous = histories[previous_index];
    GiHistorySet& candidate = histories[candidate_index];
    matter::VkImageResource& raw_signal =
        signal_mode == 0u   ? raw_diffuse_
        : signal_mode == 1u ? raw_specular_
                            : raw_transmission_;
    // Mode 0 samples but ignores the aux (the shader's aux logic is gated on
    // signalMode >= 1), so binding the specular aux there stays correct.
    matter::VkImageResource& raw_aux =
        signal_mode == 2u ? raw_transmission_aux_ : raw_specular_aux_;
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
        !record_gi_atrous_signal(frame, 1u, error, retain) ||
        !record_gi_atrous_signal(frame, 2u, error, retain))
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
        (signal_mode == 0u   ? gi_history_
         : signal_mode == 1u ? gi_spec_history_
                             : gi_trans_history_)[gi_composite_history_index_];
    matter::VkImageResource* filtered =
        signal_mode == 0u   ? gi_atrous_
        : signal_mode == 1u ? gi_spec_atrous_
                            : gi_trans_atrous_;
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
        // No epoch bump is needed for the promotion itself. Promoting drops the
        // shared_ptr to the BLAS a candidate replaced, but a candidate can only
        // exist because build_ray_geometry recorded a build for it earlier in
        // this same frame, and that already bumped the epoch — which retired
        // every cached TLAS describing the superseded structure. Bumping again
        // here would additionally retire the TLAS built alongside the
        // candidate, which is still perfectly valid (promotion moves ownership,
        // not the device address).
    }
    // Commit or discard this frame's staged TLAS. On success the slot's
    // structure now genuinely holds the staged records and may be reused; on
    // failure the frame was never submitted, so the structure's contents are
    // unknown and the slot must rebuild.
    for (FrameResources& slot : frames_) {
        if (slot.rt_tlas_pending_serial != frame_serial) continue;
        slot.rt_tlas_pending_serial = 0;
        if (!succeeded) continue;
        slot.rt_tlas_geometry_epoch = slot.rt_tlas_pending_epoch;
        slot.rt_tlas_valid = true;
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
        lighting.emission_multiplier != lighting_.emission_multiplier ||
        // A wider sun redistributes both direct light (softer shadow cone) and
        // the reflection prefilter, so it invalidates GI history exactly like a
        // sun move does.
        lighting.sun_angular_diameter_deg != lighting_.sun_angular_diameter_deg;
    if (lighting_initialized_ && source_changed)
        gi_history_reset_pending_ = true;
    lighting_ = lighting;
    // The two disc cosines are DERIVED, not passed in: computing them here is
    // what guarantees every consumer (composite sky, RT environment, RT
    // reflection prefilter) sees the same thresholds for the same diameter,
    // and that they are CPU floats rather than a GPU cos() that is only good
    // to a few ULP. Overwriting whatever the caller put in these two fields is
    // deliberate — see the comment on them in the header.
    lighting_.sun_disc_cos_edge =
        matter::sun_disc_cos_edge(lighting_.sun_angular_diameter_deg);
    lighting_.sun_disc_cos_core =
        matter::sun_disc_cos_core(lighting_.sun_angular_diameter_deg);
    lighting_initialized_ = true;
    // Task 11: mirror the fresh sun direction/intensity into the tileset UBO
    // every frame -- write_tileset_params_buffer() no-ops until
    // ensure_tileset_infra() has run, and re-derives the (cheap) slot table
    // from tileset_slots_ each call, so this stays a plain memcpy+flush.
    if (source_changed) write_tileset_params_buffer();
}

void VkSceneRenderer::set_display_exposure(float exposure_ev) {
    display_exposure_ev_ = exposure_ev;
}

void VkSceneRenderer::set_volumetrics_settings(
    const matter::VulkanVolumetricsSettings& s,
    const matter::FogSettings& fog) {
    volumetrics_enabled_ = s.enabled;
    volumetrics_debug_view_ = s.vol_debug_view;
    // The composite pass skips froxel integration entirely for a ray whose
    // camera AND terrain hit are both above the clouds — a straight ray with
    // both endpoints above a horizontal deck cannot pass through it, and
    // without this, coarse distant slices leak valley density over mountain
    // summits.
    //
    // Two things had to change for layered clouds. The ceiling is now the
    // highest enabled deck rather than the one layer's top; and the early-out
    // is only VALID when there is no ground fog, because ground fog has no
    // upper bound and so does fill the space above every deck. The old code
    // got away without that second test because the bounded-layer mode
    // suppressed ground fog by construction.
    float ceiling = 0.0f;
    int enabled = 0;
    const int32_t live = matter::active_cloud_count(fog);
    for (int32_t i = 0; i < live; ++i) {
        if (enabled == 0 || fog.clouds[i].max_height > ceiling)
            ceiling = fog.clouds[i].max_height;
        ++enabled;
    }
    volumetrics_height_layer_ = enabled > 0 && fog.density <= 0.0f;
    volumetrics_cloud_top_ = ceiling;
    if (volumetrics_)
        volumetrics_->update_settings(s, fog);
}

void VkSceneRenderer::set_tileset_pom_settings(
    const matter::TilesetPomSettings& s) {
    tileset_pom_settings_ = s;
    // write_tileset_params_buffer() no-ops until ensure_tileset_infra() has
    // run and re-derives the (cheap) slot table from tileset_slots_ every
    // call, same pattern as the per-frame sun_dir_intensity mirror in
    // set_lighting -- calling it here keeps the UBO in lockstep with the
    // settings the instant they change rather than waiting for the next
    // lighting update.
    write_tileset_params_buffer();
}

void VkSceneRenderer::set_vt_near_band_settings(
    const matter::VtNearBandSettings& s) {
    vt_near_band_settings_ = s;
    // Same UBO, same lockstep rationale as set_tileset_pom_settings above.
    write_tileset_params_buffer();
}

// ---- Free-range recycling ------------------------------------------------
// The static staging arrays and their GPU buffers are managed as ranges: a
// released part's ranges quarantine in `pending` until every frame that could
// still read the old bytes has retired, then become allocatable. Uniformly
// sized sector parts make fragmentation low; when nothing fits, allocation
// falls back to tail growth (the append/grow path that already existed).

void VkSceneRenderer::FreeRangeList::release(uint32_t start, uint32_t count,
                                             uint64_t serial) {
    if (count == 0) return;
    pending.push_back({{start, count}, serial});
}

void VkSceneRenderer::FreeRangeList::settle(uint64_t safe_serial) {
    size_t write = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        const PendingRange& entry = pending[i];
        if (entry.freed_serial > safe_serial) {
            pending[write++] = pending[i];
            continue;
        }
        // Insert sorted by start and coalesce with both neighbours.
        Range range = entry.range;
        auto after = std::lower_bound(
            free_ranges.begin(), free_ranges.end(), range,
            [](const Range& a, const Range& b) { return a.start < b.start; });
        if (after != free_ranges.begin()) {
            auto before = std::prev(after);
            if (before->start + before->count == range.start) {
                range.start = before->start;
                range.count += before->count;
                after = free_ranges.erase(before);
            }
        }
        if (after != free_ranges.end() &&
            range.start + range.count == after->start) {
            range.count += after->count;
            after = free_ranges.erase(after);
        }
        free_ranges.insert(after, range);
    }
    pending.resize(write);
}

uint32_t VkSceneRenderer::FreeRangeList::allocate(uint32_t count) {
    if (count == 0) return UINT32_MAX;
    for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
        if (it->count < count) continue;
        const uint32_t start = it->start;
        if (it->count == count) {
            free_ranges.erase(it);
        } else {
            it->start += count;
            it->count -= count;
        }
        return start;
    }
    return UINT32_MAX;
}

void VkSceneRenderer::FreeRangeList::clear() {
    free_ranges.clear();
    pending.clear();
}

void VkSceneRenderer::settle_free_ranges() {
    const uint64_t safe_serial =
        static_frame_serial_ > static_frame_window_
            ? static_frame_serial_ - static_frame_window_
            : 0;
    free_clusters_.settle(safe_serial);
    free_vertices_.settle(safe_serial);
    free_indices_.settle(safe_serial);
}

uint32_t VkSceneRenderer::allocate_cluster_range(uint32_t count) {
    const uint32_t reused = free_clusters_.allocate(count);
    if (reused != UINT32_MAX) return reused;
    const uint32_t start = static_cast<uint32_t>(cluster_staging_.size());
    cluster_staging_.resize(cluster_staging_.size() + count);
    cluster_lods_.resize(cluster_lods_.size() + count);
    return start;
}

uint32_t VkSceneRenderer::allocate_vertex_range(uint32_t count) {
    const uint32_t reused = free_vertices_.allocate(count);
    if (reused != UINT32_MAX) return reused;
    const uint32_t start = static_cast<uint32_t>(vertex_staging_.size());
    vertex_staging_.resize(vertex_staging_.size() + count);
    return start;
}

uint32_t VkSceneRenderer::allocate_index_range(uint32_t count) {
    const uint32_t reused = free_indices_.allocate(count);
    if (reused != UINT32_MAX) return reused;
    const uint32_t start = static_cast<uint32_t>(index_staging_.size());
    index_staging_.resize(index_staging_.size() + count);
    return start;
}

void VkSceneRenderer::release_part(uint64_t part_hash) {
    if (poisoned()) return;
    // Releasing a part destroys its bottom-level structures, so no cached TLAS
    // may keep referencing them.
    ++rt_geometry_epoch_;
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return;
    // Belt and braces for update_instances()' fast path: the slot_of_ erase is
    // caught by its snapshot compare, but this rewrites instance_staging_
    // directly, so retire the snapshot outright rather than relying on that.
    instance_snapshot_valid_ = false;
    const uint32_t released_slot = static_cast<uint32_t>(found->second);
    slot_of_.erase(found);
    // Covers both the erase and the `parts_[released_slot] = {}` below; nothing
    // reads the version in between (see slot_of_version_).
    ++slot_of_version_;
    PartRecord& record = parts_[released_slot];
    // Return the geometry ranges to the recycler. No compaction and no static
    // re-upload: the bytes stay where they are, unreferenced (the instance
    // filter below removes every reader), until a later registration reuses
    // the range after the in-flight window has retired.
    free_clusters_.release(record.cluster_start, record.cluster_count,
                           static_frame_serial_);
    free_vertices_.release(record.vertex_start, record.vertex_count,
                           static_frame_serial_);
    free_indices_.release(record.index_start, record.index_count,
                          static_frame_serial_);
    // Disable the freed clusters CPU-side so the next command-template
    // rebuild emits nothing for them. The stale GPU copies are never visited
    // (no live instance spans the range) and are rewritten on reuse.
    for (uint32_t i = 0; i < record.cluster_count; ++i) {
        cluster_staging_[record.cluster_start + i] = GpuCluster{};
        cluster_lods_[record.cluster_start + i].clear();
    }
    // WP-E: drop the variant's indirection layer, pinned tail and CPU mesh
    // copies before the record goes away. The compositor's GPU mesh cache for
    // the same variant is dropped later (see vt_pending_invalidate_): a fill
    // recorded this frame may still be reading those buffers.
    if (vt_) vt_->release_variant(part_hash);
    // WP-H: the enricher caches the same streams plus the variant's own
    // acceleration structure and has the same kMaxBatchesInFlight retirement
    // horizon, so it rides the same deferred invalidation.
    if (vt_compositor_ || vt_enricher_)
        vt_pending_invalidate_.emplace_back(part_hash,
                                            vt_invalidate_retire_serial());
    vt_draw_slots_dirty_ = true;
    part_draw_overrides_dirty_ = true;
    if (record.vt_rung_mask != 0u && vt_deferred_parts_ != 0u)
        --vt_deferred_parts_;
    // The slot itself is never reused (cluster.part_slot values and rt_lods
    // stay stable); the emptied record just stops matching every liveness
    // test. Its RT buffers drop here, exactly as before.
    parts_[released_slot] = {};

    // Strip any dynamic tails so the filter below walks index-aligned arrays
    // (prepare_frame re-merges tails every frame anyway).
    if (instance_staging_.size() > static_instance_count_)
        instance_staging_.resize(static_instance_count_);
    if (rt_instances_.size() > static_rt_instance_count_)
        rt_instances_.resize(static_rt_instance_count_);
    size_t write = 0;
    for (size_t i = 0; i < instance_staging_.size(); ++i) {
        if (instance_part_slots_[i] == released_slot) continue;
        instance_staging_[write] = instance_staging_[i];
        instance_part_slots_[write] = instance_part_slots_[i];
        ++write;
    }
    instance_staging_.resize(write);
    instance_part_slots_.resize(write);
    rt_instances_.erase(
        std::remove_if(rt_instances_.begin(), rt_instances_.end(),
                       [part_hash](const RtInstance& instance) {
                           return instance.part_hash == part_hash;
                       }),
        rt_instances_.end());
    static_instance_count_ = instance_staging_.size();
    static_rt_instance_count_ = rt_instances_.size();
    max_clusters_per_instance_ = 0;
    for (const auto& instance : instance_staging_)
        max_clusters_per_instance_ =
            std::max(max_clusters_per_instance_, instance.cluster_count);
    ++instance_generation_;
    // Per-part instance counts changed; one rebuild per frame covers any
    // number of releases (flush_command_template / update_instances).
    command_template_dirty_ = true;
}

void VkSceneRenderer::set_temporal_frame(const TemporalFrame& frame) {
    // Track, for update_instances()' unchanged-input fast path, whether the
    // history data it actually reads changed. That is exactly:
    //   * `reset` (gates the whole history lookup), and
    //   * per entry and IN ORDER (the id index keeps the first match, so order
    //     is load-bearing when ids repeat): instance_id, history_valid, and
    //     previous_object_to_world -- the last only when history_valid is set,
    //     because it is not read otherwise.
    // current_object_to_world is never read by update_instances().
    //
    // The flag is sticky: it is only cleared when update_instances() takes a
    // fresh snapshot, so any number of set_temporal_frame() calls between two
    // update_instances() calls are covered.
    if (!temporal_copy_fuse_enabled()) {
        if (!temporal_history_changed_) {
            vk_build_profile::Scope compare_scope(
                vk_build_profile::kSetTemporalCompare);
            bool same = temporal_frame_.reset == frame.reset;
            // When both frames are reset the history contributes nothing to the
            // output in either case, so the per-entry data cannot matter.
            if (same && !frame.reset) {
                same =
                    temporal_frame_.instances.size() == frame.instances.size();
                for (size_t index = 0; same && index < frame.instances.size();
                     ++index) {
                    const TemporalInstanceFrame& previous =
                        temporal_frame_.instances[index];
                    const TemporalInstanceFrame& next = frame.instances[index];
                    if (previous.instance_id != next.instance_id ||
                        previous.history_valid != next.history_valid ||
                        (next.history_valid &&
                         std::memcmp(
                             previous.previous_object_to_world.m,
                             next.previous_object_to_world.m,
                             sizeof(next.previous_object_to_world.m)) != 0))
                        same = false;
                }
            }
            if (!same) temporal_history_changed_ = true;
        }
        {
            vk_build_profile::Scope copy_scope(
                vk_build_profile::kSetTemporalCopy);
            temporal_frame_ = frame;
        }
        return;
    }
    // Perf: the compare above and the `temporal_frame_ = frame` below each walk
    // the same ~90k-entry, ~13 MB TemporalInstanceFrame array, so the pair cost
    // three full traversals per frame -- and vector's copy-assign allocates
    // EXACTLY the source size when it has to grow, which during a fill is every
    // frame (13 MB allocated, copied and freed each time). One pass does both,
    // and resize_geometric restores amortised growth. The comparison result and
    // the resulting contents are identical.
    const bool compare_needed = !temporal_history_changed_;
    const size_t count = frame.instances.size();
    // Exactly the branch structure of the legacy path above: the per-entry data
    // (and therefore a size mismatch) only matters when neither frame is a
    // reset, because on a reset the history contributes nothing either way.
    bool same = temporal_frame_.reset == frame.reset;
    const bool history_matters = compare_needed && same && !frame.reset;
    if (history_matters && temporal_frame_.instances.size() != count)
        same = false;
    const bool per_entry_compare = history_matters && same;
    {
        vk_build_profile::Scope copy_scope(vk_build_profile::kSetTemporalCopy);
        // Grow first so every destination entry exists. When the sizes differ
        // `same` is already settled, so the stale tail is never compared.
        vk_perf::resize_geometric(temporal_frame_.instances, count);
        for (size_t index = 0; index < count; ++index) {
            TemporalInstanceFrame& previous = temporal_frame_.instances[index];
            const TemporalInstanceFrame& next = frame.instances[index];
            if (per_entry_compare && same &&
                (previous.instance_id != next.instance_id ||
                 previous.history_valid != next.history_valid ||
                 (next.history_valid &&
                  std::memcmp(previous.previous_object_to_world.m,
                              next.previous_object_to_world.m,
                              sizeof(next.previous_object_to_world.m)) != 0)))
                same = false;
            previous = next;
        }
    }
    if (compare_needed && !same) temporal_history_changed_ = true;
    // Everything except the instance array, which the loop above already owns.
    temporal_frame_.current_unjittered = frame.current_unjittered;
    temporal_frame_.previous_unjittered = frame.previous_unjittered;
    temporal_frame_.current_jittered = frame.current_jittered;
    temporal_frame_.previous_jittered = frame.previous_jittered;
    temporal_frame_.internal_extent = frame.internal_extent;
    temporal_frame_.output_extent = frame.output_extent;
    temporal_frame_.jitter_pixels[0] = frame.jitter_pixels[0];
    temporal_frame_.jitter_pixels[1] = frame.jitter_pixels[1];
    temporal_frame_.reset = frame.reset;
    temporal_frame_.attempt_token = frame.attempt_token;
    temporal_frame_.presented_frame_index = frame.presented_frame_index;
}

bool VkSceneRenderer::instance_inputs_match_snapshot(
    const std::vector<VkSceneInstance>& instances) const noexcept {
    if (instance_input_snapshot_.size() != instances.size()) return false;
    // VkSceneInstance is uint64 + float[16] + uint64 with no padding, so a
    // bytewise compare is exact. It is also bit-exact rather than value-exact,
    // which only ever makes this MORE conservative: pack_glsl_mat4 copies bits,
    // so any input bit difference is an output bit difference too.
    if (!instances.empty() &&
        std::memcmp(instance_input_snapshot_.data(), instances.data(),
                    instances.size() * sizeof(VkSceneInstance)) != 0)
        return false;
    // Inputs (2) and (3) collapse to one integer: slot_of_version_ moves on
    // every mutation of slot_of_ and on every write of the PartRecord cluster
    // range fields (see its declaration). Walking the std::map here cost an
    // O(parts) pointer chase per frame for a question a counter answers.
    if (snapshot_version_enabled())
        return snapshot_slot_of_version_ == slot_of_version_;
    if (part_cluster_snapshot_.size() != parts_.size()) return false;
    for (size_t slot = 0; slot < parts_.size(); ++slot) {
        if (part_cluster_snapshot_[slot].first != parts_[slot].cluster_start ||
            part_cluster_snapshot_[slot].second != parts_[slot].cluster_count)
            return false;
    }
    if (slot_of_snapshot_.size() != slot_of_.size()) return false;
    size_t index = 0;
    for (const auto& entry : slot_of_) {
        if (slot_of_snapshot_[index].first != entry.first ||
            slot_of_snapshot_[index].second != entry.second)
            return false;
        ++index;
    }
    return true;
}

void VkSceneRenderer::snapshot_instance_inputs(
    const std::vector<VkSceneInstance>& instances) {
    // resize + memcpy, not copy-assign: vector's operator= allocates exactly
    // the source size when it has to grow, so a fill that gains instances every
    // frame reallocated this ~8 MB block every frame.
    vk_perf::resize_geometric(instance_input_snapshot_, instances.size());
    if (!instances.empty())
        std::memcpy(instance_input_snapshot_.data(), instances.data(),
                    instances.size() * sizeof(VkSceneInstance));
    snapshot_slot_of_version_ = slot_of_version_;
    if (!snapshot_version_enabled()) {
        slot_of_snapshot_.assign(slot_of_.begin(), slot_of_.end());
        part_cluster_snapshot_.resize(parts_.size());
        for (size_t slot = 0; slot < parts_.size(); ++slot)
            part_cluster_snapshot_[slot] = {parts_[slot].cluster_start,
                                            parts_[slot].cluster_count};
    }
    instance_snapshot_generation_ = instance_generation_;
    temporal_history_changed_ = false;
    instance_snapshot_valid_ = true;
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
    // Strip any dynamic tail appended by the previous prepare_frame().
    if (instance_staging_.size() > static_instance_count_)
        instance_staging_.resize(static_instance_count_);
    if (rt_instances_.size() > static_rt_instance_count_)
        rt_instances_.resize(static_rt_instance_count_);
    // Perf: unchanged-input fast path.
    //
    // The candidate set built below -- and therefore the `identical` early-out
    // at the bottom of this function -- is a pure function of five inputs, all
    // of which are snapshotted by snapshot_instance_inputs() on every call that
    // returns true:
    //   1. the `instances` span: part_hash, object_to_world and instance_id,
    //      in order (bytewise compare).
    //   2. slot_of_: decides both whether an instance resolves at all and the
    //      part_slot it gets (flattened compare, O(parts)).
    //   3. parts_[slot].cluster_start / .cluster_count: the only PartRecord
    //      fields read here (compare, O(parts)).
    //   4. temporal_frame_: reset, plus per-entry instance_id/history_valid/
    //      previous_object_to_world, folded into temporal_history_changed_ by
    //      set_temporal_frame() while it already has both copies in hand.
    //   5. instance_staging_ / instance_part_slots_, which the candidate set is
    //      compared against. Every writer outside this function either bumps
    //      instance_generation_ (reset(), the release_part() success path,
    //      prepare_frame()'s dynamic merge) or mutates slot_of_/parts_ and so
    //      is caught by 2/3 (the release_part() rebuild-failure path erases
    //      from slot_of_ before clearing the staging vectors).
    // prepare_frame()'s dynamic tail is stripped immediately above, so the two
    // vectors hold exactly the static set the snapshot was taken from.
    //
    // When all five still match, the build below would reproduce
    // instance_staging_ byte for byte and fall into `if (identical) return
    // true;` -- which returns WITHOUT touching the GPU buffer, bumping a
    // generation, or updating max_clusters_per_instance_. Returning here does
    // exactly the same thing, minus ~24 MB of per-frame allocation (candidate
    // instances/slots/RT plus one hash-map node per instance).
    {
        vk_build_profile::Scope fast_scope(vk_build_profile::kUiFastPath);
        if (instance_snapshot_valid_ && !temporal_history_changed_ &&
            instance_generation_ == instance_snapshot_generation_ &&
            instance_part_slots_.size() == instance_staging_.size() &&
            instance_inputs_match_snapshot(instances))
            return true;
    }
    std::vector<GpuInstance>& candidate_instances = candidate_instances_scratch_;
    std::vector<uint32_t>& candidate_slots = candidate_slots_scratch_;
    std::vector<RtInstance>& candidate_rt = candidate_rt_scratch_;
    candidate_instances.clear();
    candidate_slots.clear();
    candidate_rt.clear();
    // Geometric, not exact: reserve(n) allocates exactly n, so a fill whose
    // instance count ticks up every frame reallocated all three of these
    // (~14 MB + ~7 MB + 0.4 MB) on every frame instead of O(log N) times.
    vk_perf::reserve_geometric(candidate_instances, instances.size());
    vk_perf::reserve_geometric(candidate_slots, instances.size());
    vk_perf::reserve_geometric(candidate_rt, instances.size());
    uint32_t candidate_max_clusters = 0;
    // Perf: the per-instance history lookup below used to be a std::find_if
    // linear scan of temporal_frame_.instances — O(instances^2), ~848M
    // comparisons over a ~6MB array at 41k instances — and then a hash map
    // rebuilt per call, ~60k node allocations per streaming frame.
    //
    // In the engine's call pattern the TemporalFrame handed to
    // set_temporal_frame() was built this same frame from this same
    // `instances` span, so entry i describes instance i. Verify the id per
    // element and read positionally; the first element where the sequences
    // disagree (a different caller, a stale frame) builds the keyed index
    // once and every later lookup goes through it.
    //
    // Behaviour-preserving details:
    //   * find_if returned the FIRST entry with a matching id; emplace() keeps
    //     the first insertion, so duplicate ids resolve to the same entry —
    //     and begin() computes identical history fields for every entry of a
    //     repeated id, so positional resolution cannot diverge from it.
    //   * the result is only ever consulted under `!temporal_frame_.reset`, so
    //     on reset frames no index is built and every lookup misses, exactly
    //     as the discarded scan result did.
    const bool temporal_usable = !temporal_frame_.reset;
    const bool temporal_positional =
        temporal_usable &&
        temporal_frame_.instances.size() == instances.size();
    std::unordered_map<uint64_t, const TemporalInstanceFrame*> temporal_by_id;
    bool temporal_by_id_built = false;
    const auto temporal_lookup =
        [&](uint64_t stable_id,
            size_t source_index) -> const TemporalInstanceFrame* {
        if (!temporal_usable) return nullptr;
        if (temporal_positional) {
            const TemporalInstanceFrame& entry =
                temporal_frame_.instances[source_index];
            if (entry.instance_id == stable_id) return &entry;
        }
        if (!temporal_by_id_built) {
            temporal_by_id.reserve(temporal_frame_.instances.size());
            for (const TemporalInstanceFrame& item : temporal_frame_.instances)
                temporal_by_id.emplace(item.instance_id, &item);
            temporal_by_id_built = true;
        }
        const auto found = temporal_by_id.find(stable_id);
        return found != temporal_by_id.end() ? found->second : nullptr;
    };
    vk_build_profile::Scope build_scope(vk_build_profile::kUiBuildLoop);
    for (size_t source_index = 0; source_index < instances.size();
         ++source_index) {
        const VkSceneInstance& source = instances[source_index];
        // Perf: this was slot_of_.find() -- a red-black-tree descent per
        // instance, i.e. ~90k walks of a several-thousand-node std::map per
        // frame during a fill, each costing multiple dependent cache misses.
        // part_slot_lookup answers the identical question in one probe of a
        // flat table derived from the same map (see slot_of_version_).
        const int found_slot = part_slot_lookup(source.part_hash);
        if (found_slot < 0) continue;
        const PartRecord& part = parts_[static_cast<size_t>(found_slot)];
        GpuInstance instance{};
        instance.object_to_world = pack_glsl_mat4(source.object_to_world);
        instance.previous_object_to_world = instance.object_to_world;
        const uint64_t stable_id = source.instance_id;
        instance.instance_token =
            stable_id != 0
                ? vulkan_history_token(stable_id)
                : static_cast<uint32_t>(source_index) + 1u;
        instance.animation_instance_slot = source.animation_instance_slot;
        // Static scene records have no generational dynamic-slot identity.
        instance.animation_instance_generation = 0;
        const TemporalInstanceFrame* temporal =
            temporal_lookup(stable_id, source_index);
        if (!temporal_frame_.reset && temporal != nullptr &&
            temporal->history_valid) {
            instance.previous_object_to_world =
                pack_glsl_mat4(temporal->previous_object_to_world);
            instance.history_valid = 1;
        }
        instance.part_slot = static_cast<uint32_t>(found_slot);
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
    build_scope.stop();
    vk_build_profile::Scope compare_scope(vk_build_profile::kUiCompare);
    // One slots compare, not two: `identical` and `layout_changed` asked the
    // same O(instances) question and both used to walk the array.
    const bool slots_equal = candidate_slots == instance_part_slots_;
    const bool identical =
        candidate_instances.size() == instance_staging_.size() && slots_equal &&
        std::equal(candidate_instances.begin(), candidate_instances.end(),
                   instance_staging_.begin(),
                   [](const GpuInstance& left, const GpuInstance& right) {
                       return std::memcmp(&left, &right, sizeof(left)) == 0;
                   });
    compare_scope.stop();
    if (identical) {
        vk_build_profile::Scope snapshot_scope(vk_build_profile::kUiSnapshot);
        snapshot_instance_inputs(instances);
        return true;
    }

    const bool layout_changed = !slots_equal;
    if (!layout_changed) {
        // Swap, not move: the retired staging keeps its capacity inside the
        // scratch vectors for the next rebuild.
        std::swap(instance_staging_, candidate_instances);
        std::swap(instance_part_slots_, candidate_slots);
        std::swap(rt_instances_, candidate_rt);
        max_clusters_per_instance_ = candidate_max_clusters;
        static_instance_count_ = instance_staging_.size();
        static_rt_instance_count_ = rt_instances_.size();
        ++instance_generation_;
        vk_build_profile::Scope snapshot_scope(vk_build_profile::kUiSnapshot);
        snapshot_instance_inputs(instances);
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
    vk_build_profile::Scope layout_scope(vk_build_profile::kUiLayout);
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
        instance_snapshot_valid_ = false;
        return false;
    }
    layout_scope.stop();
    static_instance_count_ = instance_staging_.size();
    static_rt_instance_count_ = rt_instances_.size();
    ++instance_generation_;
    if (layout_changed) {
        note_command_layout_rebuild();
        vk_build_profile::note_layout_rebuild();
    }
    {
        vk_build_profile::Scope snapshot_scope(vk_build_profile::kUiSnapshot);
        snapshot_instance_inputs(instances);
    }
    // Recycle the retired staging as next call's scratch capacity. (The
    // rollback path above returns before this and keeps the old vectors.)
    candidate_instances = std::move(old_instances);
    candidate_slots = std::move(old_slots);
    candidate_rt = std::move(old_rt);
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
        if (cluster.part_slot >= per_part.size()) {
            error = "cluster part bucket is outside the active part table";
            return false;
        }
        // GpuCluster::lod_count IS cluster_lods_[i].size(): ensure_part writes
        // both from the same source.lods (and admission caps it at kVkMaxLod),
        // release_part zeroes both, reset() clears both, and
        // allocate_cluster_range resizes them together. Reading the flat copy
        // avoids chasing a separate heap block per cluster in a loop that runs
        // over every cluster in the world on every layout rebuild.
        for (uint32_t lod = 0; lod < cluster.lod_count; ++lod) {
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

    // Geometric growth: assign() sizes the block exactly, so a streaming fill
    // that adds clusters every frame reallocated the whole command template
    // (clusters x 9 x 20 B) and its enable mask on every frame.
    vk_perf::reserve_geometric(command_template_,
                               static_cast<size_t>(command_count));
    vk_perf::reserve_geometric(raster_command_enabled_,
                               static_cast<size_t>(command_count));
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
                command.index_count = lods[lod].index_count;
                command.first_index = lods[lod].first_index;      // already global (Task 3)
                command.vertex_offset =
                    static_cast<int32_t>(parts_[cluster.part_slot].vertex_start);
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
    // Buckets own [0, first_instance); the skin tail follows, one slot per
    // dynamic-instance slot. capacities.w must stay at the BUCKET total
    // (see upload_frame_constants) or cull.comp's last bucket would
    // reserve into the tail.
    skin_transform_base_ = first_instance;
    draw_transform_slots_ = first_instance +
        static_cast<uint32_t>(dynamic_instance_staging_.size());
    part_instance_counts_ = std::move(per_part);
    part_command_ranges_ = std::move(next_part_ranges);
    // Any successful rebuild covers every deferred registration.
    command_template_dirty_ = false;
    return true;
}

bool VkSceneRenderer::flush_command_template(std::string& error) {
    if (!command_template_dirty_) return true;
    if (!rebuild_command_template(error)) return false;
    note_command_layout_rebuild();
    return true;
}

// Task 7 (dynamic lane): rebuild_command_template() sizes the per-bucket
// transform regions and the per-part indirect draw ranges from STATIC
// instances only (part_instance_counts_ is that baseline and is never
// modified here). When prepare_frame() merges dynamic instances into
// instance_staging_, the culler needs room in each bucket for them, or
// reserve_transform_slot() in shaders_vk/cull.comp finds
// `next.first_instance - this.first_instance` exhausted and drops the
// instance (the "dynamic entities cast RT shadows but never rasterize" bug).
// This re-derives, from the static baseline plus the active dynamic slots:
//   - command_template_[].first_instance (per-bucket transform offsets)
//   - draw_transform_slots_ (total transform-buffer slots / capacities.w)
//   - part_command_ranges_ (a part with only dynamic instances must still
//     record its indirect draws)
// The recompute always starts from the static baseline, so it is idempotent
// across frames and reproduces rebuild_command_template()'s exact output when
// no dynamic instances are active (which restores the static layout).
bool VkSceneRenderer::apply_dynamic_command_layout(std::string& error) {
    if (command_template_.size() !=
            cluster_staging_.size() * static_cast<size_t>(kVkMaxLod) ||
        part_instance_counts_.size() != parts_.size()) {
        error = "dynamic command layout requires a valid static command template";
        return false;
    }
    std::vector<uint32_t> merged_counts = part_instance_counts_;
    for (uint32_t part_slot : dynamic_instance_part_slots_) {
        if (part_slot == UINT32_MAX) continue;
        if (part_slot >= merged_counts.size()) {
            error = "dynamic instance part bucket is outside the active part table";
            return false;
        }
        if (!checked_u32_add(merged_counts[part_slot], 1u,
                             merged_counts[part_slot],
                             "dynamic instance part bucket", error)) {
            return false;
        }
    }
    uint32_t first_instance = 0;
    for (size_t cluster_index = 0; cluster_index < cluster_staging_.size();
         ++cluster_index) {
        const GpuCluster& cluster = cluster_staging_[cluster_index];
        if (cluster.part_slot >= merged_counts.size()) {
            error = "cluster part bucket is outside the active part table";
            return false;
        }
        // cluster.lod_count == cluster_lods_[cluster_index].size(); see
        // rebuild_command_template for why that holds.
        const uint32_t lod_count = cluster.lod_count;
        for (uint32_t lod = 0; lod < kVkMaxLod; ++lod) {
            DrawCommand& command =
                command_template_[cluster_index * kVkMaxLod + lod];
            command.first_instance = first_instance;
            if (lod < lod_count &&
                !checked_u32_add(first_instance,
                                 merged_counts[cluster.part_slot],
                                 first_instance, "draw transform slots",
                                 error)) {
                return false;
            }
        }
    }
    VkDeviceSize transform_bytes = 0;
    if (!vk_scene_detail::checked_mul_to_device_size(
            first_instance, sizeof(GpuDrawTransform), transform_bytes,
            "draw-transform buffer", error)) {
        return false;
    }
    const VkDeviceSize storage_limit =
        std::min(limits_.max_storage_buffer_range, limits_.max_buffer_size);
    if (storage_limit != 0 && transform_bytes > storage_limit) {
        error = "draw-transform buffer exceeds Vulkan storage descriptor limit";
        return false;
    }
    // Buckets own [0, first_instance); the skin tail follows, one slot per
    // dynamic-instance slot. capacities.w must stay at the BUCKET total
    // (see upload_frame_constants) or cull.comp's last bucket would
    // reserve into the tail.
    skin_transform_base_ = first_instance;
    draw_transform_slots_ = first_instance +
        static_cast<uint32_t>(dynamic_instance_staging_.size());
    std::vector<PartCommandRange> next_part_ranges;
    next_part_ranges.reserve(parts_.size());
    for (uint32_t slot = 0; slot < parts_.size(); ++slot) {
        const PartRecord& part = parts_[slot];
        if (!part.live || merged_counts[slot] == 0 || part.cluster_count == 0 ||
            part.vertex_count == 0)
            continue;
        next_part_ranges.push_back(
            {part.cluster_start * kVkMaxLod,
             part.cluster_count * kVkMaxLod, slot});
    }
    part_command_ranges_ = std::move(next_part_ranges);
    ++command_generation_;
    return true;
}

// Static-upload census (VkSceneRenderer::static_upload_census). The kFull
// branch recreates the cluster/vertex/index buffers and rewrites ALL of them
// into host-visible memory -- hundreds of MB at streaming scale -- and it runs
// inside prepare_frame (build region) or dispatch_culling (draw region). Both
// buckets carry a max because the mean hides exactly the spike worth finding.
namespace {
std::atomic<uint64_t> g_su_full_count{0}, g_su_full_us{0}, g_su_full_max_us{0};
std::atomic<uint64_t> g_su_append_count{0}, g_su_append_us{0};
void su_note(std::atomic<uint64_t>& count, std::atomic<uint64_t>& total,
             std::atomic<uint64_t>* mx, uint64_t us) {
    count.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(us, std::memory_order_relaxed);
    if (!mx) return;
    uint64_t prev = mx->load(std::memory_order_relaxed);
    while (us > prev &&
           !mx->compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
}
}  // namespace

VkSceneRenderer::StaticUploadCensus VkSceneRenderer::static_upload_census() {
    StaticUploadCensus c;
    c.full_count   = g_su_full_count.load(std::memory_order_relaxed);
    c.full_us      = g_su_full_us.load(std::memory_order_relaxed);
    c.full_max_us  = g_su_full_max_us.load(std::memory_order_relaxed);
    c.append_count = g_su_append_count.load(std::memory_order_relaxed);
    c.append_us    = g_su_append_us.load(std::memory_order_relaxed);
    return c;
}

bool VkSceneRenderer::upload_scene_buffers(
    FrameResources& frame, VkCommandBuffer material_command_buffer,
    bool reset_stats, std::string& error) {
    VkDeviceSize cluster_bytes = 0;
    VkDeviceSize instance_bytes = 0;
    VkDeviceSize command_bytes = 0;
    VkDeviceSize transform_bytes = 0;
    VkDeviceSize vertex_bytes = 0;
    VkDeviceSize index_bytes = 0;
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
            index_staging_.size(), sizeof(uint32_t), index_bytes,
            "index buffer", error) ||
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
    if (std::max<VkDeviceSize>(index_bytes, 1) > limits_.max_buffer_size) {
        error = "index buffer exceeds Vulkan maxBufferSize";
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
    const auto upload_at = [&](matter::VkBufferResource& buffer,
                               const void* data, VkDeviceSize size,
                               VkDeviceSize offset) {
        if (size == 0) return true;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (uploads == test_fail_after_uploads_) {
            error = "forced scene buffer upload failure";
            return poison(error);
        }
#endif
        if (!matter::upload_buffer(*vulkan_, buffer, data, size, offset, error))
            return poison(error);
        ++uploads;
        return true;
    };
    const auto upload = [&](matter::VkBufferResource& buffer, const void* data,
                            VkDeviceSize size) {
        return upload_at(buffer, data, size, 0);
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
    vk_build_profile::Scope static_scope(vk_build_profile::kPfUploadStatic);
    const auto su_append_t0 = std::chrono::steady_clock::now();
    if (static_upload_dirty_ == StaticUpload::kAppend) {
        // Streaming fast path. Every static mutation since the last upload
        // was a register_part() ranged write — into a recycled interior range
        // whose old bytes sat unreferenced for a full in-flight window, or a
        // tail extension no recorded frame reads past — so writing just those
        // ranges in place is race-free and costs O(new parts); the full path
        // below recreates the buffers and rewrites O(world). During sector
        // streaming that full path ran nearly every frame and dominated
        // build_ms (issues/render-streaming-build-cpu).
        if (clusters_.size >= cluster_bytes &&
            vertices_.size >= vertex_bytes &&
            indices_.size >= index_bytes) {
            const auto upload_ranges =
                [&](matter::VkBufferResource& buffer,
                    const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                    const void* base, size_t element_size,
                    uint64_t* upload_counter) {
                for (const auto& range : ranges) {
                    const VkDeviceSize offset =
                        VkDeviceSize{range.first} * element_size;
                    const VkDeviceSize size =
                        VkDeviceSize{range.second} * element_size;
                    if (!upload_at(buffer,
                                   static_cast<const char*>(base) + offset,
                                   size, offset))
                        return false;
                }
                if (!ranges.empty() && upload_counter) ++*upload_counter;
                return true;
            };
            if (!upload_ranges(clusters_, dirty_cluster_ranges_,
                               cluster_staging_.data(), sizeof(GpuCluster),
                               &upload_counters_.cluster_uploads) ||
                !upload_ranges(vertices_, dirty_vertex_ranges_,
                               vertex_staging_.data(), sizeof(VkRasterVertex),
                               &upload_counters_.vertex_uploads) ||
                !upload_ranges(indices_, dirty_index_ranges_,
                               index_staging_.data(), sizeof(uint32_t),
                               nullptr)) {
                return false;
            }
            ++upload_counters_.static_append_uploads;
            dirty_cluster_ranges_.clear();
            dirty_vertex_ranges_.clear();
            dirty_index_ranges_.clear();
            uploaded_cluster_count_ =
                static_cast<uint32_t>(cluster_staging_.size());
            uploaded_vertex_count_ =
                static_cast<uint32_t>(vertex_staging_.size());
            uploaded_index_count_ =
                static_cast<uint32_t>(index_staging_.size());
            static_upload_dirty_ = StaticUpload::kClean;
            su_note(g_su_append_count, g_su_append_us, nullptr,
                    (uint64_t)std::chrono::duration_cast<
                        std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - su_append_t0).count());
        } else {
            // A buffer outgrew its capacity: take the recreate + full-rewrite
            // path. Capacity doubles there, so this happens O(log N) times
            // over a streaming load.
            static_upload_dirty_ = StaticUpload::kFull;
        }
    }
    const auto su_full_t0 = std::chrono::steady_clock::now();
    if (static_upload_dirty_ == StaticUpload::kFull) {
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
        VkDeviceSize index_capacity = 0;
        if (!replacement_capacity(clusters_.size, cluster_bytes, "cluster buffer",
                                  cluster_capacity) ||
            !replacement_capacity(vertices_.size, vertex_bytes, "vertex buffer",
                                  vertex_capacity) ||
            !replacement_capacity(indices_.size, index_bytes, "index buffer",
                                  index_capacity)) {
            return false;
        }
        matter::VkBufferResource next_clusters;
        matter::VkBufferResource next_vertices;
        matter::VkBufferResource next_indices;
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
        if (!allow_replacement()) return false;
        if (!matter::create_buffer(
                *vulkan_, index_capacity,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                next_indices, error)) {
            return false;
        }
        ++replacements;
        if (!upload(next_clusters, cluster_staging_.data(), cluster_bytes) ||
            !upload(next_vertices, vertex_staging_.data(), vertex_bytes) ||
            !upload(next_indices, index_staging_.data(), index_bytes)) {
            return false;
        }
        // Replacing these drops the previous buffers' owning references. Any
        // frame still in flight keeps them alive ONLY through the lifetimes it
        // retained at record time (retain_for_frame below) — every buffer
        // recorded into a frame's command buffer must be on that list.
        clusters_ = std::move(next_clusters);
        vertices_ = std::move(next_vertices);
        indices_ = std::move(next_indices);
        static_upload_dirty_ = StaticUpload::kClean;
        su_note(g_su_full_count, g_su_full_us, &g_su_full_max_us,
                (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - su_full_t0).count());
        // The full rewrite covers any pending ranged writes.
        dirty_cluster_ranges_.clear();
        dirty_vertex_ranges_.clear();
        dirty_index_ranges_.clear();
        ++upload_counters_.static_full_uploads;
        if (cluster_bytes != 0) ++upload_counters_.cluster_uploads;
        if (vertex_bytes != 0) ++upload_counters_.vertex_uploads;
        uploaded_cluster_count_ =
            static_cast<uint32_t>(cluster_staging_.size());
        uploaded_vertex_count_ =
            static_cast<uint32_t>(vertex_staging_.size());
        uploaded_index_count_ =
            static_cast<uint32_t>(index_staging_.size());
    }

    static_scope.stop();
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
    // WP-E: per-(part_slot, lod) vt slots for cull.comp. Small (parts x 9
    // uint32) and uploaded unconditionally each frame — a stale table would
    // hand a draw the wrong variant's pages, which is exactly the kind of bug
    // that only shows up under streaming.
    vk_build_profile::Scope vt_slots_scope(vk_build_profile::kPfVtSlots);
    if (vt_draw_slots_dirty_) rebuild_vt_draw_slots();
    const VkDeviceSize vt_slot_bytes =
        static_cast<VkDeviceSize>(vt_draw_slot_table_.size()) * sizeof(uint32_t);
    if (!ensure_buffer(frame.vt_draw_slots, vt_slot_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error, &replaced))
        return poison(error);
    descriptors_changed |= replaced;
    // Per-module draw overrides, one entry per part slot. Same shape as the vt
    // slot table above and the same reason for an unconditional upload: the
    // table is tiny (parts x 8 B, or a single neutral entry when nothing is
    // overridden) and a stale one would cull or bias the wrong parts.
    if (part_draw_overrides_dirty_) rebuild_part_draw_overrides();
    const VkDeviceSize part_override_bytes =
        static_cast<VkDeviceSize>(part_draw_override_table_.size()) *
        sizeof(matter::PartDrawOverrideGpu);
    if (!ensure_buffer(frame.part_draw_overrides, part_override_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error, &replaced))
        return poison(error);
    descriptors_changed |= replaced;
    if (descriptors_changed) update_frame_descriptors(frame);
    if (vt_slot_bytes != 0 &&
        !upload(frame.vt_draw_slots, vt_draw_slot_table_.data(), vt_slot_bytes))
        return false;
    if (part_override_bytes != 0 &&
        !upload(frame.part_draw_overrides, part_draw_override_table_.data(),
                part_override_bytes))
        return false;
    vt_slots_scope.stop();

    {
        vk_build_profile::Scope instances_scope(
            vk_build_profile::kPfUploadInstances);
        if (frame.instance_generation != instance_generation_) {
            if (!upload(frame.instances, instance_staging_.data(),
                        instance_bytes))
                return false;
            if (instance_bytes != 0) ++upload_counters_.instance_uploads;
            frame.instance_generation = instance_generation_;
        }
    }
    // Fill the skin transform tail from the same dynamic-instance records the
    // static lane uses. cull.comp never writes here (explicit skinned draws are
    // not in any bucket), so without this the draw reads a bucket slot that
    // belongs to some unrelated static instance.
    if (skin_transform_base_ < draw_transform_slots_ &&
        !dynamic_instance_staging_.empty()) {
        skin_transform_staging_.assign(
            draw_transform_slots_ - skin_transform_base_, GpuDrawTransform{});
        for (size_t slot = 0;
             slot < dynamic_instance_staging_.size() &&
             slot < skin_transform_staging_.size(); ++slot) {
            const GpuInstance& source = dynamic_instance_staging_[slot];
            GpuDrawTransform& target = skin_transform_staging_[slot];
            target.current = source.object_to_world;
            target.previous = source.previous_object_to_world;
            target.history_valid = source.history_valid;
            target.instance_token = source.instance_token;
        }
        const VkDeviceSize tail_bytes =
            static_cast<VkDeviceSize>(skin_transform_staging_.size()) *
            sizeof(GpuDrawTransform);
        const VkDeviceSize tail_offset =
            static_cast<VkDeviceSize>(skin_transform_base_) *
            sizeof(GpuDrawTransform);
        if (tail_bytes != 0 &&
            !matter::upload_buffer(*vulkan_, frame.draw_transforms,
                                   skin_transform_staging_.data(), tail_bytes,
                                   tail_offset, error))
            return false;
    }
    vk_build_profile::Scope commands_scope(vk_build_profile::kPfUploadCommands);
    if (frame.command_generation != command_generation_)
        frame.command_generation = command_generation_;
    // Unconditional by design: cull.comp writes instance_count into these
    // records on the GPU, so the CPU template has to be restored every frame.
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
    // Copy-assign reallocates exactly; pre-grow so a fill does not rebuild this
    // mirror's block every frame.
    vk_perf::reserve_geometric(uploaded_raster_command_enabled_,
                               raster_command_enabled_.size());
    uploaded_raster_command_enabled_ = raster_command_enabled_;
    uploaded_raster_draw_command_count_ = raster_draw_command_count_;
    commands_scope.stop();
    // Every writer of rt_instances_ content bumps instance_generation_
    // (update_instances, the dynamic merge under dynamic_dirty_,
    // release_part, reset), so an unchanged generation means the mirror is
    // already current and the per-frame deep copy can be skipped.
    if (uploaded_rt_instances_generation_ != instance_generation_ ||
        uploaded_rt_instances_.size() != rt_instances_.size()) {
        vk_build_profile::Scope rt_scope(vk_build_profile::kPfUploadOther);
        vk_perf::reserve_geometric(uploaded_rt_instances_,
                                   rt_instances_.size());
        uploaded_rt_instances_ = rt_instances_;
        uploaded_rt_instances_generation_ = instance_generation_;
    }
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
    constants.counts[3] = static_cast<uint32_t>(animation_bounds_.dynamic_bounds().size());
    constants.capacities[0] = static_cast<uint32_t>(cluster_staging_.size());
    constants.capacities[1] = static_cast<uint32_t>(instance_staging_.size());
    constants.capacities[2] = static_cast<uint32_t>(command_template_.size());
    // BUCKET total, not the buffer total: cull.comp's reserve_transform_slot
    // treats this as the end of the last bucket's region.
    constants.capacities[3] = skin_transform_base_;
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
    // Advance the range recycler's notion of time: a freed range becomes
    // reusable once every frame that could read its old bytes has retired.
    if (frame.serial > static_frame_serial_)
        static_frame_serial_ = frame.serial;
    static_frame_window_ =
        std::max<uint64_t>(frame.frame_slot_count, 1);
    // Deferred registrations (register_part) must materialise their command
    // template before apply_dynamic_command_layout or the uploads read it.
    {
        vk_build_profile::Scope flush_scope(vk_build_profile::kPfFlushTemplate);
        if (!flush_command_template(error)) return false;
    }
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
    // Merge active dynamic instances into the static staging vectors so the
    // upload, constants, and dispatch code all see one contiguous array.
    // The dynamic tails are stripped at the start of the next update_instances()
    // and defensively here, so repeated prepare_frame() calls cannot stack
    // duplicate tails.
    vk_build_profile::Scope dynamic_scope(vk_build_profile::kPfDynamic);
    if (instance_staging_.size() > static_instance_count_)
        instance_staging_.resize(static_instance_count_);
    if (rt_instances_.size() > static_rt_instance_count_)
        rt_instances_.resize(static_rt_instance_count_);
    if (dynamic_instance_count_ > 0) {
        for (size_t i = 0; i < dynamic_instance_staging_.size(); ++i) {
            if (dynamic_instance_part_slots_[i] != UINT32_MAX) {
                const GpuInstance& inst = dynamic_instance_staging_[i];
                instance_staging_.push_back(inst);
                // The cull dispatch fans out counts.y threads per instance;
                // a dynamic-only part with more clusters than any static
                // instance must widen the fan-out or its tail clusters are
                // never visited.
                max_clusters_per_instance_ =
                    std::max(max_clusters_per_instance_, inst.cluster_count);
                const uint32_t slot = dynamic_instance_part_slots_[i];
                RtInstance rt{};
                if (slot < parts_.size())
                    rt.part_hash = parts_[slot].hash;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        rt.transform[r * 4 + c] =
                            inst.object_to_world.elements[c * 4 + r];
                // `i` is the dynamic instance slot the skin lane keys on
                // (see the compaction loop's candidate.instance_slot), so the
                // tracer can resolve the same animation-bounds union it does.
                rt.animation_instance_slot = static_cast<uint32_t>(i);
                rt.animation_instance_generation =
                    inst.animation_instance_generation;
                rt_instances_.push_back(rt);
            }
        }
        if (dynamic_dirty_) {
            ++instance_generation_;
            dynamic_dirty_ = false;
        }
    }
    // Re-derive the culler's transform regions and the per-part draw ranges
    // for the merged instance set. Also runs once more after the last dynamic
    // instance disappears to restore the static-only baseline.
    if (dynamic_instance_count_ > 0 || dynamic_command_layout_applied_) {
        if (!apply_dynamic_command_layout(error)) return false;
        dynamic_command_layout_applied_ = dynamic_instance_count_ > 0;
    }
    dynamic_scope.stop();
    vk_build_profile::Scope anim_scope(vk_build_profile::kPfAnimBounds);
    // Bounds are resolved from the immutable current/previous pose pair before
    // this frame's cull dispatch. An empty set still uploads one inert record
    // so the storage descriptor remains valid on implementations that reject
    // zero-sized buffers.
    std::vector<VkAnimationBoundsGpuRecord> animation_bound_records =
        animation_bounds_.gpu_records();
    if (animation_bound_records.empty())
        animation_bound_records.push_back({});
    bool animation_bounds_replaced = false;
    const VkDeviceSize animation_bounds_bytes =
        static_cast<VkDeviceSize>(animation_bound_records.size()) *
        sizeof(VkAnimationBoundsGpuRecord);
    if (!ensure_buffer(selected.animation_bounds, animation_bounds_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error,
                       &animation_bounds_replaced)) {
        return false;
    }
    // A successful replacement is live even when the following upload fails.
    // Refresh immediately so retrying this frame can never reuse a descriptor
    // that points at the retired allocation.
    if (animation_bounds_replaced) {
        update_descriptor(selected.descriptor_sets[1], 8,
                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          selected.animation_bounds);
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (test_fail_animation_bounds_upload_once_) {
        test_fail_animation_bounds_upload_once_ = false;
        error = "forced animation bounds upload failure";
        return false;
    }
#endif
    if (!matter::upload_buffer(
            *vulkan_, selected.animation_bounds, animation_bound_records.data(),
            animation_bounds_bytes, 0, error)) {
        return false;
    }
    anim_scope.stop();
    if (!upload_scene_buffers(selected, frame.command_buffer, false, error))
        return false;
    {
        vk_build_profile::Scope constants_scope(
            vk_build_profile::kPfUploadOther);
        if (!upload_frame_constants(selected, matrices, camera_eye,
                                    pixel_budget, error))
            return false;
    }
    vk_build_profile::Scope stats_scope(vk_build_profile::kPfStats);
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
    stats_scope.stop();
    active_frame_index_ = frame.frame_slot;
    vk_build_profile::Scope retain_scope(vk_build_profile::kPfRetain);
    std::vector<std::shared_ptr<void>> resources{
        clusters_.lifetime,
        vertices_.lifetime,
        // indices_ belongs here with vertices_: record_raster binds it with
        // vkCmdBindIndexBuffer and every draw is a vkCmdDrawIndexedIndirect, so
        // the frame references it for its whole lifetime. Without this, adding a
        // part large enough to grow the index buffer while an earlier frame was
        // still in flight let ensure_index_buffer's `indices_ = std::move(...)`
        // destroy a buffer a submitted command buffer was still using. Only the
        // legacy immediate render_gbuffer_and_composite path retained it, so the
        // production path failed intermittently, depending on when a part landed
        // relative to frame completion.
        indices_.lifetime,
        selected.frame_constants.lifetime,
        selected.instances.lifetime,
        selected.commands.lifetime,
        selected.draw_transforms.lifetime,
        selected.stats.lifetime,
        selected.animation_bounds.lifetime,
        selected.material_upload.lifetime,
        selected.materials.lifetime,
        albedo_.lifetime,
        normal_.lifetime,
        orm_.lifetime,
        depth_.lifetime,
        hdr_.lifetime};
    const bool retained =
        vulkan_->retain_for_frame(frame, std::move(resources), error);
    retain_scope.stop();
    // Last statement of the build region the engine times as stats.build_ms.
    // Renders at most one aggregate line per interval, and only under
    // MATTER_VK_BUILD_PROFILE=1.
    vk_build_profile::frame_end(instance_staging_.size(),
                                cluster_staging_.size(), parts_.size(),
                                command_template_.size());
    return retained;
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
                            &raw_specular_aux_, &raw_transmission_,
                            &raw_transmission_aux_}) {
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
    // Same conservative all-LOD union the CPU skin lane and cull.comp plan
    // against. Fetched once: gpu_records() materializes a vector.
    const std::vector<VkAnimationBoundsGpuRecord> rt_planning_bounds =
        animation_bounds_.gpu_records();
    for (const RtInstance& source : rt_instances_) {
        // Same flat mirror update_instances() uses -- this is the other
        // per-instance std::map descent in the frame (draw_ms's share).
        const int found_slot = part_slot_lookup(source.part_hash);
        if (found_slot < 0) continue;
        PartRecord& part = parts_[static_cast<size_t>(found_slot)];
        matter::Mat4f object_to_world{};
        std::memcpy(object_to_world.m, source.transform,
                    sizeof(object_to_world.m));
        for (uint32_t cluster_index = 0;
             cluster_index < part.cluster_count; ++cluster_index) {
            // The raster lanes already treat an accepted skin draw as the sole
            // owner of its (instance, generation, cluster): cull.comp drops the
            // static bind-pose draw the moment the bounds record carries the
            // skin-raster flag. Apply the same exclusion here. This BLAS is
            // immutable bind-pose geometry (see the build contract below), so
            // tracing it under a compute-skinned gbuffer buries posed pixels
            // inside the bind-pose silhouette -- their GI and sun rays self-hit
            // immediately and carve hard-edged dark patches across the animated
            // surface. Until a deforming-BLAS phase exists, a skinned cluster
            // contributes no traced geometry at all: a missing occluder reads
            // as a soft lighting omission, a wrong-pose occluder reads as
            // geometry. Skin fallbacks need no special case -- a BindPose
            // fallback publishes no draw, so the raster mesh IS the bind pose
            // and the traced copy aligns with it again.
            if (source.animation_instance_slot != UINT32_MAX &&
                animation_skin_raster_owns_cluster(
                    selected.ready_skin_raster_draws,
                    source.animation_instance_slot,
                    source.animation_instance_generation, cluster_index))
                continue;
            const uint32_t global_cluster =
                part.cluster_start + cluster_index;
            const GpuCluster& gpu_cluster = cluster_staging_[global_cluster];
            // A deforming instance's static cluster AABB is the part's whole
            // bind-pose extent (for a partitioned animated part, an
            // origin-centered box covering skin AND every rigid segment), so
            // its radius runs well above the animated skin's. Selecting from it
            // holds the traced rung one level finer than the rasterized one
            // across a wide band of distances, and both surfaces are on screen
            // at once: the raster rung shades the gbuffer while the finer
            // traced rung supplies GI and shadow rays, so the two silhouettes
            // interpenetrate and the mesh reads as two overlapping copies.
            // Resolving the same union the raster lanes use keeps the lanes on
            // the same rung at every distance.
            VkAnimationBoundsAabb planning_bounds{
                {gpu_cluster.aabb_min[0], gpu_cluster.aabb_min[1],
                 gpu_cluster.aabb_min[2]},
                {gpu_cluster.aabb_max[0], gpu_cluster.aabb_max[1],
                 gpu_cluster.aabb_max[2]}};
            float planning_radius = gpu_cluster.radius;
            if (source.animation_instance_slot != UINT32_MAX &&
                resolve_animation_cluster_union(
                    rt_planning_bounds, source.animation_instance_slot,
                    source.animation_instance_generation, cluster_index,
                    planning_bounds)) {
                const float dx = planning_bounds.max[0] - planning_bounds.min[0];
                const float dy = planning_bounds.max[1] - planning_bounds.min[1];
                const float dz = planning_bounds.max[2] - planning_bounds.min[2];
                planning_radius =
                    0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            const uint32_t lod_index = vk_scene_detail::select_cluster_lod_view(
                {planning_bounds.min[0], planning_bounds.min[1],
                 planning_bounds.min[2]},
                {planning_bounds.max[0], planning_bounds.max[1],
                 planning_bounds.max[2]},
                planning_radius, gpu_cluster.thresholds,
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
        if (!part.rt_geometry || !part.rt_index ||
            lod.candidate_serial != 0 ||
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
        triangles.vertexData.deviceAddress = part.rt_geometry->address;   // part base, no LOD offset
        triangles.vertexStride = sizeof(VkRasterVertex);
        triangles.maxVertex = part.vertex_count - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        // lod.first_index is part-local (stored that way in RtLodRecord to
        // remain compaction-invariant); use it directly as the byte offset
        // into the per-part rt_index buffer.
        triangles.indexData.deviceAddress =
            part.rt_index->address +
            static_cast<VkDeviceSize>(lod.first_index) * sizeof(uint32_t);
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
        // C4's ray-tracing contract is intentionally conservative: the
        // compute-skinned raster stream never enters an RT BLAS. The immutable
        // part bind pose remains build-once until a later deforming-RT phase.
        // The selection loop above enforces the complement: while a skin draw
        // owns a cluster, its bind-pose BLAS stays out of the TLAS entirely,
        // so build-once geometry only ever traces where it is also rasterized.
        assert(skinned_rt_uses_bind_pose_blas());
        assert((item.build.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) == 0);
        assert(item.build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
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
    // Any recorded BLAS build changes geometry an already-built TLAS may
    // reference (rebuilt in place, or relocated to a freshly created candidate
    // structure). Retire every slot's cached TLAS so none can reference stale
    // or superseded bottom-level data.
    if (has_blas_work) ++rt_geometry_epoch_;
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
        if (!traced_blas || !part.rt_geometry || !part.rt_index) continue;
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
        record.vertex_address = part.rt_geometry->address;    // part base
        record.index_address =
            part.rt_index->address +
            static_cast<uint64_t>(lod.first_index) * sizeof(uint32_t);
        record.vertex_stride = sizeof(viewer::VkRasterVertex);
        record.vertex_count = part.vertex_count;
        record.primitive_count = lod.primitive_count;
        record.valid = 1u;
        // WP-G: the VT slot for the exact rung this BLAS traces.
        // RtLodRecord::cluster_index is PART-LOCAL; cluster_lods_ (and the
        // raster vt_draw_slots table) are indexed globally, so rebase first.
        record.vt_slot =
            vt_slot_for_lod(part, part.cluster_start + lod.cluster_index,
                            lod.lod_index);
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

    // The per-part geometry table is descriptor-bound and read by the RT
    // shaders every frame, so it is refreshed unconditionally -- before the
    // acceleration-structure work below, which may be skipped entirely. These
    // are plain host-visible writes with no command-buffer ordering against the
    // build, so hoisting them above it is behaviour-neutral.
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

    // Perf: rebuilding the TLAS from scratch every frame was by far the largest
    // GPU cost in dense streamed scenes (~15 ms at 58k instances) even though
    // the tracing that consumes it costs a fraction of that. An acceleration
    // structure is device resident and stays valid until either the records it
    // was built from or the bottom-level structures they reference change, so a
    // slot whose TLAS already encodes exactly these records can skip the
    // rebuild outright.
    //
    // Reuse requires ALL of:
    //   * a previously *completed* build for this slot -- rt_tlas_valid is only
    //     set from finish_ray_tracing_frame, on a frame that reported success;
    //   * an unchanged geometry epoch, bumped by every BLAS build, part release
    //     and reset(), i.e. every event that can change or free memory a cached
    //     TLAS points at; and
    //   * byte-identical instance records. Every field of
    //     VkAccelerationStructureInstanceKHR is assigned above from a
    //     value-initialised struct and the type is fully packed (each bitfield
    //     pair shares one 32-bit word), so memcmp is exact -- in particular it
    //     catches accelerationStructureReference changing when a cluster picks
    //     a different LOD.
    //
    // A slot only ever reuses its own TLAS, and a slot is not re-recorded until
    // its previous submission has retired, so the structure being read is
    // always fully built. Any miss falls through to the original full rebuild.
    const bool tlas_reusable =
        selected.rt_tlas_valid &&
        selected.rt_tlas.handle != VK_NULL_HANDLE &&
        selected.rt_tlas_geometry_epoch == rt_geometry_epoch_ &&
        selected.rt_tlas_instances.size() == instances.size() &&
        std::memcmp(selected.rt_tlas_instances.data(), instances.data(),
                    static_cast<size_t>(instance_bytes)) == 0;
    if (tlas_reusable) {
        // No acceleration-structure write is recorded this frame, so the
        // build->trace barrier below has nothing to order and is skipped too.
        ++rt_tlas_reuses_;
        return true;
    }
    ++rt_tlas_builds_;

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
        // The previous structure is gone; nothing cached about it survives.
        selected.rt_tlas_valid = false;
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

    // Stage this build as the slot's cache candidate. Deliberately NOT marked
    // valid here: the build is only *recorded*, and a frame abandoned before
    // submission would leave the TLAS holding its previous contents.
    // finish_ray_tracing_frame promotes the candidate once the frame reports
    // success, and leaves rt_tlas_valid false on failure so the slot rebuilds.
    selected.rt_tlas_instances = std::move(instances);
    selected.rt_tlas_pending_epoch = rt_geometry_epoch_;
    selected.rt_tlas_pending_serial = frame.serial;
    selected.rt_tlas_valid = false;
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
                                 &raw_transmission_, &raw_transmission_aux_}) {
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
    VkDescriptorImageInfo raw_transmission_aux_info{
        VK_NULL_HANDLE, raw_transmission_aux_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo part_info{selected.rt_parts.buffer, 0,
                                     selected.rt_parts.size};
    VkDescriptorBufferInfo material_info{selected.materials.buffer, 0,
                                         selected.materials.size};
    VkDescriptorBufferInfo error_info{selected.rt_error_counter.buffer, 0,
                                      selected.rt_error_counter.size};
    VkDescriptorBufferInfo test_output_info{selected.rt_test_output.buffer, 0,
                                            selected.rt_test_output.size};
    // Phase 1 tileset Vulkan port (Task 6): bindings 15/16 mirror raster set
    // 1's bindings 6/7. Rebuilt from current renderer state (loaded slots or
    // dummies) every frame here, the same way the rest of this array already
    // is — no separate "on slot load" write is needed for the RT set.
    VkDescriptorImageInfo
        tileset_image_infos[tileset::kMaxTilesetSlots * kTilesetChannelCount]{};
    for (int slot = 0; slot < tileset::kMaxTilesetSlots; ++slot) {
        for (int channel = 0; channel < kTilesetChannelCount; ++channel) {
            VkDescriptorImageInfo& info =
                tileset_image_infos[slot * kTilesetChannelCount + channel];
            info.sampler = tileset_sampler_;
            info.imageView = tileset_channel_view(slot, channel);
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    VkDescriptorBufferInfo tileset_params_info{tileset_params_.buffer, 0,
                                               sizeof(TilesetParamsGpu)};
    // WP-G: bindings 17/18/19 mirror raster set 1's VT bindings 10/11/12,
    // built from exactly the same live/dummy state
    // write_vt_descriptors_for_frame() uses so a ray and a fragment resolve
    // against the identical pool, indirection and variant table.
    VkDescriptorImageInfo vt_pool_infos[vt::kVtChannelCount]{};
    const bool vt_live = vt_ && vt_->available();
    for (uint32_t c = 0; c < vt::kVtChannelCount; ++c) {
        vt_pool_infos[c].sampler =
            vt_live ? vt_->pool_sampler() : tileset_sampler_;
        vt_pool_infos[c].imageView =
            vt_live ? vt_->pool_view(c) : tileset_dummy_rgba8_.view;
        vt_pool_infos[c].imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // The indirection is a storage buffer since the buffer-indirection
    // redesign; the dummy storage buffer stands in until VT is live.
    VkDescriptorBufferInfo vt_indirection_info{
        vt_live ? vt_->indirection_buffer() : vt_dummy_storage_.buffer, 0,
        vt_live ? vt_->indirection_buffer_size() : VkDeviceSize{64}};
    VkDescriptorBufferInfo vt_variants_info{
        vt_live ? vt_->variant_buffer() : vt_dummy_storage_.buffer, 0,
        vt_live ? vt_->variant_buffer_size() : VkDeviceSize{64}};
    VkWriteDescriptorSet writes[21]{};
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
    writes[15].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[15].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[15].dstBinding = 15;
    writes[15].descriptorCount =
        tileset::kMaxTilesetSlots * kTilesetChannelCount;
    writes[15].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[15].pImageInfo = tileset_image_infos;
    writes[16].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[16].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[16].dstBinding = 16;
    writes[16].descriptorCount = 1;
    writes[16].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[16].pBufferInfo = &tileset_params_info;
    for (uint32_t i = 17; i < 20; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[17].descriptorCount = vt::kVtChannelCount;
    writes[17].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[17].pImageInfo = vt_pool_infos;
    writes[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[18].pBufferInfo = &vt_indirection_info;
    writes[19].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[19].pBufferInfo = &vt_variants_info;
    // RT PBR Phase 1: binding 20 is the transmission aux storage image.
    writes[20].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[20].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[20].dstBinding = 20;
    writes[20].descriptorCount = 1;
    writes[20].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[20].pImageInfo = &raw_transmission_aux_info;
    vkUpdateDescriptorSets(vulkan_->device(), 21, writes, 0, nullptr);
    struct alignas(16) ShadowConstants {
        GpuMat4 clip_to_world;
        float to_sun_max_distance[4];
        float bias;
        uint32_t samples;
        uint32_t debug_view;
        // Ground-POM roof escape (mirrors rt_lighting.rgen's shading-origin
        // lift): world-space distance to lift the shadow ray origin along
        // the sun direction for materials that carry a ground tileset
        // detail slot. Sourced from the LIVE tileset POM settings so the
        // tuning UI's relief-cap slider keeps working (see
        // write_tileset_params_buffer's identical pom_max_relief_m source).
        float pom_lift;
        // Sun angular size as a multiple of the shipped default, scaling the
        // radius of the cone the shadow rays are jittered inside. EXACTLY
        // 1.0f at the default diameter (a float divided by itself), so the
        // shader's `0.002 * sun_cone_scale` is bit-identical to the 0.002 it
        // used to hardcode. See matter/sun_angles.h.
        float sun_cone_scale;
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
    // With POM disabled the ground stays flat at the datum, so there is no
    // displaced roof to escape -- lifting by the relief cap would then push
    // the shadow origin past real contact occluders. Keep only the epsilon.
    constants.pom_lift = tileset_pom_settings_.enabled
                             ? tileset_pom_settings_.relief_cap_m + 0.02f
                             : 0.02f;
    constants.sun_cone_scale =
        matter::sun_size_scale(lighting_.sun_angular_diameter_deg);
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
            // Sun size, in the three forms rt_lighting.rgen needs it. These
            // occupy what were pad0/pad1/pad2, so the block did not grow.
            // cos_edge/cos_core are the same CPU-computed disc thresholds
            // composite.frag gets (see VkSceneLighting); size_scale squares
            // into the reflection prefilter's solid angle.
            float sun_disc_cos_edge;
            float sun_disc_cos_core;
            float sun_size_scale;
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
        gi.sun_disc_cos_edge = lighting_.sun_disc_cos_edge;
        gi.sun_disc_cos_core = lighting_.sun_disc_cos_core;
        gi.sun_size_scale =
            matter::sun_size_scale(lighting_.sun_angular_diameter_deg);
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
    // RT PBR Phase 1: the composite now reads the denoised transmission when
    // the GI chain ran (update_composite_descriptor picks the same image).
    matter::VkImageResource& composite_transmission =
        gi_settings_.enabled && gi_filtered_valid_
            ? gi_trans_atrous_[gi_filtered_index_] : raw_transmission_;
    transition_for_use(frame.command_buffer, composite_transmission,
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
        raw_transmission_aux_.lifetime,
        composite_specular.lifetime, composite_transmission.lifetime,
        selected.rt_instances.lifetime, selected.rt_scratch.lifetime,
        selected.rt_tlas_scratch.lifetime,
        selected.rt_tlas.lifetime, selected.rt_parts.lifetime,
        selected.rt_error_counter.lifetime, selected.materials.lifetime,
        selected.rt_test_output.lifetime, rt_sbt_.lifetime};
    for (const auto& part : parts_) {
        if (part.rt_geometry) retained.push_back(part.rt_geometry->lifetime);
        if (part.rt_index) retained.push_back(part.rt_index->lifetime);
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
    // Demand-driven VT: stamp wanted rungs against this frame's camera,
    // surface registration requests (the engine drains them before its next
    // render call) and reclaim lingering variants. Before prepare_frame so a
    // release lands in this frame's vt_draw_slots upload.
    update_vt_demand(camera_eye, pixel_budget);
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
    // WP-E/WP-G: this frame slot's fence has already been waited on, so
    // consuming its previous feedback readback and rewriting its VT
    // descriptors is safe. It must happen BEFORE anything binds scene set 1
    // into this command buffer -- write_vt_descriptors_for_frame() updates
    // that very set, and updating a set already bound in a RECORDING command
    // buffer invalidates the buffer (VUID-...-commandBuffer-recording; without
    // UPDATE_AFTER_BIND the binding is not allowed to change). The cull
    // dispatch below is the first such binding, so the call sits here rather
    // than next to the raster record where the immediate path keeps it.
    // ensure_raster_targets() above has already published raster_extent_,
    // which is what sizes the feedback target.
    vt_begin_frame(selected, frame.frame_slot);
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
        raw_transmission_.lifetime, raw_transmission_aux_.lifetime,
        vol_dummy_3d_.lifetime,
        gi_atrous_[0].lifetime, gi_atrous_[1].lifetime,
        gi_spec_atrous_[0].lifetime, gi_spec_atrous_[1].lifetime,
        gi_trans_atrous_[0].lifetime, gi_trans_atrous_[1].lifetime};
    if (volumetrics_ && volumetrics_->active())
        attachments.push_back(volumetrics_->vol_integrated().lifetime);
    for (auto* histories : {&gi_history_, &gi_spec_history_,
                            &gi_trans_history_}) {
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

    // Resolve/record skin work before culling. Only draws whose source and
    // output ranges survived renderer validation may be removed from the
    // static indirect path.
    if (!record_animation_skinning(frame, selected, error)) return false;
    std::vector<VkAnimationBoundsGpuRecord> animation_bound_records =
        animation_bounds_.gpu_records();
    if (selected.skin_raster_ready) {
        mark_animation_skin_raster_records(
            animation_bound_records, selected.ready_skin_raster_draws);
    }
    if (animation_bound_records.empty())
        animation_bound_records.push_back({});
    if (!matter::upload_buffer(
            *vulkan_, selected.animation_bounds,
            animation_bound_records.data(),
            static_cast<VkDeviceSize>(animation_bound_records.size()) *
                sizeof(VkAnimationBoundsGpuRecord),
            0, error)) {
        return false;
    }

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
        composite_debug_override_ > 0.0f
            ? composite_debug_override_
            : (ray_tracing_settings_.enabled && vulkan_->ray_tracing_available() &&
                       ray_tracing_settings_.debug_view
                   ? 1.0f
                   : 0.0f);
    frame_lighting.camera_fwd_x = -matrices.world_to_view.m[8];
    frame_lighting.camera_fwd_y = -matrices.world_to_view.m[9];
    frame_lighting.camera_fwd_z = -matrices.world_to_view.m[10];
    frame_lighting.tan_half_fov =
        matrices.view_to_clip.m[5] != 0.0f
            ? 1.0f / matrices.view_to_clip.m[5] : 1.0f;
    frame_lighting.aspect_ratio =
        matrices.view_to_clip.m[0] != 0.0f
            ? matrices.view_to_clip.m[5] / matrices.view_to_clip.m[0] : 1.0f;
    const VkExtent2D ie = temporal_frame_.internal_extent.width != 0
                              ? temporal_frame_.internal_extent
                              : frame.extent;
    frame_lighting.jitter_offset_u =
        ie.width  != 0 ? temporal_frame_.jitter_pixels[0] /
                             static_cast<float>(ie.width)
                       : 0.0f;
    frame_lighting.jitter_offset_v =
        ie.height != 0 ? temporal_frame_.jitter_pixels[1] /
                             static_cast<float>(ie.height)
                       : 0.0f;
    const bool vol_active = volumetrics_ && volumetrics_->active();
    frame_lighting.vol_enabled = vol_active ? 1.0f : 0.0f;
    frame_lighting.vol_debug_view = volumetrics_debug_view_;
    // Reversed-Z projection identities: m[10] = near/(far-near),
    // m[11] = far*near/(far-near) (near->NDC 1, far->NDC 0), so
    // m[11]/m[10] = far and m[11]/(m[10]+1) = near -- the near/far roles are
    // swapped relative to the old standard-ZO recovery.
    frame_lighting.camera_far = matrices.view_to_clip.m[11] /
                                matrices.view_to_clip.m[10];
    frame_lighting.camera_near = matrices.view_to_clip.m[11] /
                                 (matrices.view_to_clip.m[10] + 1.0f);
    frame_lighting.camera_y = camera_eye.y;
    frame_lighting.vol_cloud_top = volumetrics_cloud_top_;
    frame_lighting.vol_height_layer =
        volumetrics_height_layer_ ? 1.0f : 0.0f;
    std::vector<VkBuffer> skin_current_buffers(frames_.size(), VK_NULL_HANDLE);
    std::vector<VkBuffer> skin_previous_buffers(frames_.size(), VK_NULL_HANDLE);
    std::vector<uint32_t> skin_vertex_counts(frames_.size(), 0);
    for (uint32_t slot = 0; slot < frames_.size(); ++slot) {
        skin_current_buffers[slot] = frames_[slot].skin_current_output.buffer;
        skin_previous_buffers[slot] = frames_[slot].skin_previous_output.buffer;
        skin_vertex_counts[slot] =
            animation_skinning_.frame(slot).current_output_vertices;
    }
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
                        skinned_raster_pipeline_,
                        pipeline_layout_,
                        {selected.descriptor_sets[0], selected.descriptor_sets[1]},
                        composite_pipeline_,
                        composite_pipeline_layout_,
                        selected.composite_descriptor_set,
                        vertices_.buffer,
                        indices_.buffer,
                        selected.commands.buffer,
                        command_template_.data(),
                        static_cast<uint32_t>(command_template_.size()),
                        static_cast<uint32_t>(index_staging_.size()),
                        skin_current_buffers.data(),
                        skin_previous_buffers.data(),
                        skin_vertex_counts.data(),
                        static_cast<uint32_t>(skin_current_buffers.size()),
                        selected.ready_skin_raster_draws.data(),
                        static_cast<uint32_t>(
                            selected.ready_skin_raster_draws.size()),
                        draw_transform_slots_,
                        skin_transform_base_,
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
                        kGpuZoneGBuffer,
                        volumetrics_.get(),
                        frame.frame_slot,
                        static_cast<float>(frame.serial) * (1.0f / 60.0f),
                        kGpuZoneVolumetrics,
                        &selected.rt_tlas,
                        this,              // WP-E: vt_hooks
                        kGpuZoneVt};       // WP-E: vt_zone
    if (volumetrics_)
        volumetrics_->set_lighting(frame_lighting);
    // (vt_begin_frame ran near the top of this function — see the note there:
    // it rewrites scene set 1, which the cull dispatch has already bound by
    // the time control reaches here.)
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
    // Test-path frame progression for the range recycler (no VulkanFrame
    // serial here; each dispatch is its own settled frame in practice).
    ++static_frame_serial_;
    if (!flush_command_template(error)) return false;
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
        raw_transmission_aux_.image != VK_NULL_HANDLE &&
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
    matter::VkImageResource raw_transmission_aux;
    GiHistorySet history[2];
    GiHistorySet spec_history[2];
    GiHistorySet trans_history[2];
    matter::VkImageResource atrous[2];
    matter::VkImageResource spec_atrous[2];
    matter::VkImageResource trans_atrous[2];
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
            raw_transmission, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, raw_extent,
            visibility_usage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, raw_transmission_aux,
            error)) {
        return false;
    }
    const VkImageUsageFlags history_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (auto* sets : {&history, &spec_history, &trans_history}) {
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
    for (auto& image : trans_atrous) {
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
    raw_transmission_aux_ = std::move(raw_transmission_aux);
    gi_history_[0] = std::move(history[0]);
    gi_history_[1] = std::move(history[1]);
    gi_spec_history_[0] = std::move(spec_history[0]);
    gi_spec_history_[1] = std::move(spec_history[1]);
    gi_trans_history_[0] = std::move(trans_history[0]);
    gi_trans_history_[1] = std::move(trans_history[1]);
    gi_atrous_[0] = std::move(atrous[0]);
    gi_atrous_[1] = std::move(atrous[1]);
    gi_spec_atrous_[0] = std::move(spec_atrous[0]);
    gi_spec_atrous_[1] = std::move(spec_atrous[1]);
    gi_trans_atrous_[0] = std::move(trans_atrous[0]);
    gi_trans_atrous_[1] = std::move(trans_atrous[1]);
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
    if (uploaded_command_count_ == 0 || uploaded_vertex_count_ == 0 ||
        uploaded_index_count_ == 0) {
        error = "raster render requires uploaded draw commands, vertices, and indices";
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
    // Name the fields: production raster records gained per-skin LOD/cluster
    // streams and this legacy static-only path must keep every skin field empty.
    RasterRecord record{};
    record.albedo = &albedo_;
    record.normal = &normal_;
    record.orm = &orm_;
    record.velocity = &velocity_;
    record.material_instance = &material_instance_;
    record.depth = &depth_;
    record.hdr = &hdr_;
    record.visibility = &visibility_;
    record.raw_diffuse = &raw_diffuse_;
    record.raw_specular = &raw_specular_;
    record.raw_transmission = &raw_transmission_;
    record.extent = raster_extent_;
    record.raster_pipeline = raster_pipeline_;
    record.raster_layout = pipeline_layout_;
    record.raster_sets[0] = selected.descriptor_sets[0];
    record.raster_sets[1] = selected.descriptor_sets[1];
    record.composite_pipeline = composite_pipeline_;
    record.composite_layout = composite_pipeline_layout_;
    record.composite_set = selected.composite_descriptor_set;
    record.vertex_buffer = vertices_.buffer;
    record.index_buffer = indices_.buffer;
    record.indirect_buffer = selected.commands.buffer;
    record.static_commands = command_template_.data();
    record.static_command_count =
        static_cast<uint32_t>(command_template_.size());
    record.index_count = uploaded_index_count_;
    record.draw_ranges = part_command_ranges_.data();
    record.draw_range_count =
        static_cast<uint32_t>(part_command_ranges_.size());
    record.max_draw_indirect_count = limits_.max_draw_indirect_count;
    record.lighting = [&] {
        auto lighting = lighting_;
        lighting.debug_view = composite_debug_override_;
        return lighting;
    }();
    record.pixel_budget = 1.0f;
    // WP-E: this legacy immediate path is what the smoke suite drives, so it
    // has to run the VT frame hooks too — otherwise no page ever fills here
    // and the vt smoke mode would only ever see empty pages. submit_immediate
    // waits for completion, so consuming this slot's readback is safe.
    record.vt_hooks = this;
    vt_begin_frame(selected, 0);
    std::vector<std::shared_ptr<void>> dependencies{
        albedo_.lifetime, normal_.lifetime, orm_.lifetime, velocity_.lifetime,
        material_instance_.lifetime, depth_.lifetime, hdr_.lifetime,
        visibility_.lifetime, raw_diffuse_.lifetime,
        vertices_.lifetime, indices_.lifetime, selected.commands.lifetime,
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
        // Reversed-Z projection identities: m[10] = near/(far-near),
        // m[11] = far*near/(far-near) (near->NDC 1, far->NDC 0), so
        // m[11]/m[10] = far and m[11]/(m[10]+1) = near -- swapped from the
        // old standard-ZO recovery (camera_near/camera_far remain true
        // linear distances in meters for downstream consumers).
        constants.camera_far = projection.m[11] / projection.m[10];
        constants.camera_near =
            projection.m[11] / (projection.m[10] + 1.0f);
        constants.camera_fov = 2.0f * std::atan(1.0f / projection.m[5]);
        constants.camera_aspect_ratio = projection.m[5] / projection.m[0];
        constants.depth_inverted = true;
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
    // composite.frag's debug_view 3.0 packs linear depth across R/G/B; it has
    // to reach the swapchain byte-for-byte, so the display pass runs in
    // passthrough for it (every lit view, including the normal buffer, keeps
    // its exposure + ACES exactly as before). The > 2.5 test now also covers
    // debug_view 4.0, the raw GBuffer albedo -- and must: that view exists to
    // read a diagnostic FIELD off the pixels (render.pom.horizon_debug), and a
    // tone curve would make the bytes mean something other than the value the
    // shader wrote. srgb_output
    // cancels the hardware OETF when the swapchain is an _SRGB format --
    // choose_surface_format prefers B8G8R8A8_SRGB, and the readback hands the
    // raw swapchain bytes back with only a BGR swap.
    const float display_push[3] = {
        display_exposure_ev_,
        composite_debug_override_ > 2.5f ? 1.0f : 0.0f,
        (display_pipeline_format_ == VK_FORMAT_B8G8R8A8_SRGB ||
         display_pipeline_format_ == VK_FORMAT_R8G8B8A8_SRGB)
            ? 1.0f
            : 0.0f,
    };
    vkCmdPushConstants(frame.command_buffer, display_pipeline_layout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(display_push),
                       display_push);
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
    // Fallback/error default represents "background" (nothing rendered);
    // reversed-Z's background depth is 0.0 (was 1.0 under standard-Z). This
    // is overwritten by the actual GBuffer readback below on success.
    pixel.depth = 0.0f;
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
    constexpr VkDeviceSize readback_size = 112;
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
    matter::VkImageResource& accumulated_transmission =
        gi_filtered_valid_ ? gi_trans_atrous_[gi_filtered_index_]
                           : raw_transmission_;
    RasterReadbackRecord record{{&albedo_, &normal_, &orm_, &velocity_, &depth_,
                                 &hdr_, &visibility_, &material_instance_,
                                 &raw_diffuse_,
                                 &accumulated_diffuse, &raw_specular_,
                                 &accumulated_specular,
                                 &raw_transmission_,
                                 &accumulated_transmission,
                                 &raw_transmission_aux_},
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
        raw_transmission_.lifetime, accumulated_transmission.lifetime,
        raw_transmission_aux_.lifetime,
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
    uint16_t raw_transmission_half[4]{};
    std::memcpy(raw_transmission_half, bytes.data() + 88,
                sizeof(raw_transmission_half));
    pixel.raw_transmission = {
        half_to_float(raw_transmission_half[0]),
        half_to_float(raw_transmission_half[1]),
        half_to_float(raw_transmission_half[2]),
        half_to_float(raw_transmission_half[3])};
    uint16_t accumulated_transmission_half[4]{};
    std::memcpy(accumulated_transmission_half, bytes.data() + 96,
                sizeof(accumulated_transmission_half));
    pixel.accumulated_transmission = {
        half_to_float(accumulated_transmission_half[0]),
        half_to_float(accumulated_transmission_half[1]),
        half_to_float(accumulated_transmission_half[2]),
        half_to_float(accumulated_transmission_half[3])};
    uint16_t transmission_aux_half[2]{};
    std::memcpy(transmission_aux_half, bytes.data() + 104,
                sizeof(transmission_aux_half));
    pixel.transmission_aux = {half_to_float(transmission_aux_half[0]),
                              half_to_float(transmission_aux_half[1]), 0.0f};
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
        indices_.reset();
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
    ++slot_of_version_;
    vt_deferred_parts_ = 0;
    vt_rung_requests_.clear();
    cluster_staging_.clear();
    cluster_lods_.clear();
    free_clusters_.clear();
    free_vertices_.clear();
    free_indices_.clear();
    dirty_cluster_ranges_.clear();
    dirty_vertex_ranges_.clear();
    dirty_index_ranges_.clear();
    instance_staging_.clear();
    instance_part_slots_.clear();
    // Retire update_instances()' unchanged-input snapshot with the state it
    // describes, and force the next set_temporal_frame() to report a change.
    instance_snapshot_valid_ = false;
    temporal_history_changed_ = true;
    instance_input_snapshot_.clear();
    instance_input_snapshot_.shrink_to_fit();
    slot_of_snapshot_.clear();
    part_cluster_snapshot_.clear();
    static_instance_count_ = 0;
    part_instance_counts_.clear();
    command_template_.clear();
    part_command_ranges_.clear();
    recorded_draw_ranges_.clear();
    raster_command_enabled_.clear();
    uploaded_raster_command_enabled_.clear();
    rt_instances_.clear();
    static_rt_instance_count_ = 0;
    vertex_staging_.clear();
    max_clusters_per_instance_ = 0;
    draw_transform_slots_ = 0;
    uploaded_command_count_ = 0;
    uploaded_transform_slots_ = 0;
    uploaded_cluster_count_ = 0;
    uploaded_vertex_count_ = 0;
    uploaded_index_count_ = 0;
    raster_draw_command_count_ = 0;
    uploaded_raster_draw_command_count_ = 0;
    uploaded_rt_instances_.clear();
    cached_stats_ = {};
    visible_skin_instances_.clear();
    pending_visible_skin_instances_.clear();
    pending_skin_visibility_frame_slot_ = UINT32_MAX;
    // Every part (and therefore every bottom-level structure) is gone; retire
    // all cached top-level structures with it.
    ++rt_geometry_epoch_;
    for (FrameResources& frame : frames_) {
        frame.rt_tlas_instances.clear();
        frame.rt_tlas_valid = false;
        frame.rt_tlas_pending_serial = 0;
    }
    for (FrameResources& frame : frames_) frame.stats_valid = false;
    raster_attachments_ready_ = false;
    ++static_generation_;
    ++instance_generation_;
    static_upload_dirty_ = StaticUpload::kFull;
    std::string ignored_error;
    if (rebuild_command_template(ignored_error)) note_command_layout_rebuild();
    poison_reason_.clear();
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    test_fail_after_replacements_ = std::numeric_limits<uint32_t>::max();
    test_fail_after_uploads_ = std::numeric_limits<uint32_t>::max();
    test_fail_after_frame_resource_allocations_ =
        std::numeric_limits<uint32_t>::max();
    test_fail_after_skin_allocations_ = std::numeric_limits<uint32_t>::max();
    test_fail_after_skin_uploads_ = std::numeric_limits<uint32_t>::max();
#endif
}

// Dynamic lane (Task 7): consumes CPU-side slot changes produced by
// matter::render::DynamicInstanceSlots. Slot indices are stable across calls,
// so dynamic_instance_staging_/dynamic_instance_part_slots_ are indexed
// directly by DynamicSlotChange::slot_index, growing lazily as new slots are
// bound. A part_slot value of UINT32_MAX marks an inactive (removed/unbound)
// slot.
bool VkSceneRenderer::update_dynamic_instances(
    const matter::render::DynamicSlotChange* changes, uint32_t count,
    uint64_t submit_serial, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    dynamic_submit_serial_ = submit_serial;
    if (count > 0 && changes == nullptr) {
        error = "update_dynamic_instances: null changes with nonzero count";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const matter::render::DynamicSlotChange& change = changes[i];
        if (change.slot_index == UINT32_MAX) {
            error = "update_dynamic_instances: invalid slot_index";
            return false;
        }
        if (change.slot_index >= dynamic_instance_staging_.size()) {
            dynamic_instance_staging_.resize(change.slot_index + 1, GpuInstance{});
            dynamic_instance_part_slots_.resize(change.slot_index + 1, UINT32_MAX);
        }
        switch (change.kind) {
            case matter::render::DynamicSlotChangeKind::Bind: {
                const auto found = slot_of_.find(change.part_hash);
                if (found == slot_of_.end()) {
                    error = "update_dynamic_instances: unknown part_hash for Bind";
                    return false;
                }
                const PartRecord& part = parts_[static_cast<size_t>(found->second)];
                GpuInstance instance{};
                instance.object_to_world = pack_glsl_mat4(change.object_to_world);
                instance.previous_object_to_world = pack_glsl_mat4(change.previous_object_to_world);
                instance.part_slot = static_cast<uint32_t>(found->second);
                instance.cluster_start = part.cluster_start;
                instance.cluster_count = part.cluster_count;
                instance.instance_token =
                    vulkan_history_token(change.entity_id.value);
                instance.animation_instance_slot = change.slot_index;
                instance.animation_instance_generation = change.slot_generation;
                dynamic_instance_staging_[change.slot_index] = instance;
                dynamic_instance_part_slots_[change.slot_index] = instance.part_slot;
                dynamic_dirty_ = true;
                break;
            }
            case matter::render::DynamicSlotChangeKind::Transform: {
                if (dynamic_instance_part_slots_[change.slot_index] == UINT32_MAX) {
                    error =
                        "update_dynamic_instances: Transform on unbound slot";
                    return false;
                }
                GpuInstance& instance = dynamic_instance_staging_[change.slot_index];
                if (instance.animation_instance_generation != change.slot_generation) {
                    error = "update_dynamic_instances: Transform generation mismatch";
                    return false;
                }
                instance.previous_object_to_world = pack_glsl_mat4(change.previous_object_to_world);
                instance.object_to_world = pack_glsl_mat4(change.object_to_world);
                instance.history_valid = 1;
                dynamic_dirty_ = true;
                break;
            }
            case matter::render::DynamicSlotChangeKind::Remove: {
                animation_bounds_.remove_instance(change.slot_index,
                                                  change.slot_generation);
                dynamic_instance_staging_[change.slot_index] = GpuInstance{};
                dynamic_instance_part_slots_[change.slot_index] = UINT32_MAX;
                dynamic_dirty_ = true;
                break;
            }
        }
    }
    uint32_t active = 0;
    for (uint32_t part_slot : dynamic_instance_part_slots_) {
        if (part_slot != UINT32_MAX) ++active;
    }
    dynamic_instance_count_ = active;
    return true;
}

void VkSceneRenderer::finish_dynamic_frame(uint64_t completed_serial) {
    dynamic_completed_serial_ =
        std::max(dynamic_completed_serial_, completed_serial);
}

}  // namespace viewer
