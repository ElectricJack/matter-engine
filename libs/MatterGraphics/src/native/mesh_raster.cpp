#include "mesh_raster.h"
#include "image_transitions.h"
#include "indirect_draw_runs.h"
#include "profile.h"
#include <climits>
namespace viewer {
namespace {
inline void write_ts(VkCommandBuffer cmd, VkQueryPool pool, uint32_t zone, bool end) {
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, zone * 2u + (end ? 1u : 0u));
}
void record_image_transition(VkCommandBuffer cmd, RasterImage& image,
    VkImageLayout layout, VkPipelineStageFlags2 before, VkAccessFlags2 source,
    VkPipelineStageFlags2 after, VkAccessFlags2 destination, VkImageAspectFlags aspect) {
    raster_detail::record_image_transition(cmd, image.image, *image.layout,
        image.mip_levels, image.array_layers, layout, before, source, after, destination, aspect);
}

void transition_for_use(VkCommandBuffer command_buffer,
                        RasterImage& image,
                        VkImageLayout new_layout,
                        VkPipelineStageFlags2 destination_stage,
                        VkAccessFlags2 destination_access,
                        VkImageAspectFlags aspect) {
    const bool undefined = *image.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    record_image_transition(
        command_buffer, image, new_layout,
        undefined ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        undefined ? 0 : VK_ACCESS_2_MEMORY_READ_BIT |
                            VK_ACCESS_2_MEMORY_WRITE_BIT,
        destination_stage, destination_access, aspect);
}

using raster_detail::gbuffer_sampled_stages;
using raster_detail::ray_depth_destination_stages;
}
void record_mesh_gbuffer(VkCommandBuffer command_buffer, const MeshRasterRecord& record) {
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
    // Recording cost only -- every scope in this function measures how long the
    // CPU takes to build the command buffer, never how long the GPU runs it.
    // GPU time for the same passes comes from the kGpuZone* timestamps.
    PROFILE_SCOPE_NAMED(z_gbuffer, "raster.gbuffer");
    // The host-visible packed/index upload buffers belong to this frame slot,
    // so their writes completed before this command buffer was submitted. A
    // changed 30 Hz selection writes packed vertices and indices exactly once
    // into this fence-owned slot; unchanged render frames skip this block.
    // Packed decoding itself occurs in the water vertex specialization.
    if (record.water_upload_required &&
        record.water_vertices != VK_NULL_HANDLE &&
        record.water_indices != VK_NULL_HANDLE &&
        record.water_vertex_bytes != 0u &&
        record.water_index_bytes != 0u) {
        VkBufferMemoryBarrier2 host_barriers[2]{};
        for (VkBufferMemoryBarrier2& barrier : host_barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.offset = 0u;
        }
        host_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        host_barriers[0].dstAccessMask =
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        host_barriers[0].buffer = record.water_vertices;
        host_barriers[0].size = record.water_vertex_bytes;
        host_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        host_barriers[1].dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        host_barriers[1].buffer = record.water_indices;
        host_barriers[1].size = record.water_index_bytes;
        VkDependencyInfo host_dependency{};
        host_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        host_dependency.bufferMemoryBarrierCount = 2u;
        host_dependency.pBufferMemoryBarriers = host_barriers;
        vkCmdPipelineBarrier2(command_buffer, &host_dependency);
    }
    RasterImage* colors[] = {
        record.albedo, record.normal, record.orm, record.velocity,
        record.material_instance, record.reactivity, record.vt_feedback};
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

    // --- GBuffer pass: six surface channels plus depth-tested VT demand ---
    const VkClearValue clear_color{{{0.0f, 0.0f, 0.0f, 0.0f}}};
    VkRenderingAttachmentInfo color_attachments[7]{};
    for (size_t i = 0; i < 6; ++i) {
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
    color_attachments[6].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachments[6].imageView = record.vt_feedback->view;
    color_attachments[6].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachments[6].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachments[6].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = record.depth->view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Reversed-Z: far plane maps to NDC depth 0, so the "no geometry yet"
    // clear value is 0.0 (was 1.0 under standard-Z).
    depth_attachment.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = record.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 7;
    rendering.pColorAttachments = color_attachments;
    rendering.pDepthAttachment = &depth_attachment;
    // WP-E: VT pool/indirection uploads and the feedback clear are transfers,
    // so they must land before dynamic rendering begins. Timed as its own GPU
    // zone under the SAME gate as the G-buffer zone -- record.ts_pool is
    // non-null only on the production frame path, which is the only path that
    // resets the query pool.
    const bool time_vt = record.ts_pool != VK_NULL_HANDLE &&
                         record.ts_written != nullptr && record.vt_pre;
    if (time_vt) {
        write_ts(command_buffer, record.ts_pool, record.vt_zone, false);
        record.ts_written[record.vt_zone] |= 1u;
    }
    if (record.vt_pre) {
        PROFILE_SCOPE("raster.vt_pre");
        record.vt_pre(command_buffer);
    }
    if (time_vt) {
        write_ts(command_buffer, record.ts_pool, record.vt_zone, true);
        record.ts_written[record.vt_zone] |= 2u;
    }
    if (record.ts_pool != VK_NULL_HANDLE && record.ts_written) {
        write_ts(command_buffer, record.ts_pool, record.gbuffer_zone, false);
        record.ts_written[record.gbuffer_zone] |= 1u;
    }
    vkCmdBeginRendering(command_buffer, &rendering);
    if (record.raster_stats_pool != VK_NULL_HANDLE)
        vkCmdBeginQuery(command_buffer, record.raster_stats_pool, 0, 0);
    vkCmdSetViewport(command_buffer, 0, 1, &raster_viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      record.raster_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            record.raster_layout, 0, 2,
                            record.raster_sets, 0, nullptr);
    vkCmdPushConstants(command_buffer, record.raster_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(record.raster_debug_push),
                       &record.raster_debug_push);
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &record.vertex_buffer,
                           &vertex_offset);
    vkCmdBindIndexBuffer(command_buffer, record.index_buffer, 0,
                         VK_INDEX_TYPE_UINT32);
    // Skin-raster instances are removed individually by cull.comp before a
    // command's instance_count is finalized, so every command in a range is
    // recorded unconditionally: that preserves bind fallbacks and ordinary
    // instances which share the same immutable mesh range.
    //
    // Because nothing is skipped per command, a part's whole range issues as
    // one multi-draw. ADJACENT PARTS COALESCE TOO: a range is just an offset
    // and a count into one indirect buffer with a uniform stride, and every
    // draw here shares one pipeline, one descriptor set, one push-constant
    // block and the same vertex/index bindings -- all bound before this loop --
    // so nothing distinguishes "part A's commands" from "part B's" at record
    // time. Merging runs whose spans touch issues the identical commands in the
    // identical order; only the number of vkCmd calls changes.
    //
    // A 2026-08-08 capture recorded 2175 ranges in 7.2 ms of render-thread
    // time. Ranges are `cluster_start * kVkMaxLod` (see the two builders in
    // rebuild_command_template / apply_dynamic_command_layout), so parts whose
    // clusters were allocated consecutively produce touching spans; holes left
    // by FreeRangeList recycling break a run, which is exactly when a separate
    // call is genuinely required.
    //
    // Deliberately NOT sorted first. Sorting would merge more, but it reorders
    // the draws, and that is a rasterization-order change this optimisation has
    // no business making silently. Merging in place is order-preserving.
    //
    // drawCount stays clamped to maxDrawIndirectCount; max(1) only keeps a
    // degenerate limit from spinning here -- the caller already refuses to
    // record when the device reports less than one.
    PROFILE_SCOPE_NAMED(z_static, "raster.draw_static");
    PROFILE_COUNT("raster.draw_ranges", record.draw_range_count);
    const auto issued = for_each_opaque_indirect_run(record.draw_ranges,
        record.draw_range_count, record.static_command_count,
        record.max_draw_indirect_count, [&](const PartCommandRange& range) {
            vkCmdDrawIndexedIndirect(command_buffer, record.indirect_buffer,
                static_cast<VkDeviceSize>(range.first_command) * sizeof(DrawCommand),
                range.command_count, sizeof(DrawCommand));
            if (record.recorded_draw_ranges) record.recorded_draw_ranges->push_back(range);
        });
    PROFILE_COUNT("raster.draw_calls", issued);
    (void)issued;
    // The mirrored table: the same ranges offset by mirror_command_base,
    // drawn with the clockwise front face (a negative-determinant transform
    // reverses the authored winding on screen). Descriptor sets, push
    // constants and buffers carry over: the pipelines share one layout.
    if (record.mirrored_raster_pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          record.mirrored_raster_pipeline);
        for_each_opaque_indirect_run(record.draw_ranges,
            record.draw_range_count, record.static_command_count,
            record.max_draw_indirect_count, [&](const PartCommandRange& range) {
                vkCmdDrawIndexedIndirect(command_buffer, record.indirect_buffer,
                    (static_cast<VkDeviceSize>(record.mirror_command_base) +
                     range.first_command) * sizeof(DrawCommand),
                    range.command_count, sizeof(DrawCommand));
            });
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          record.raster_pipeline);
    }
    z_static.stop();
    // Accepted skin work is drawn from the per-frame compute output, never
    // from the immutable bind-pose arena.  Each draw carries the exact
    // visibility-selected index span plus the source/output rebase.  Every
    // field is range-checked below, so a stale mapping cannot turn into an
    // out-of-bounds vertex fetch.
    if (record.skinned_raster_pipeline != VK_NULL_HANDLE) {
        PROFILE_SCOPE("raster.draw_skin");
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
            // A skinned draw is direct, so cull.comp never wrote a rung into
            // its transform slot. Hand the shader the rung this draw was
            // selected at, otherwise every animated part tints as rung 0.
            RasterDebugPushConstants skin_debug_push{};
            skin_debug_push.direct_lod = draw.lod;
            skin_debug_push.direct_lod_valid = 1u;
            skin_debug_push.lod_tint_enabled = record.raster_debug_push.lod_tint_enabled ? 1u : 0u;
            skin_debug_push.wireframe_enabled = record.raster_debug_push.wireframe_enabled ? 1u : 0u;
            skin_debug_push.impostor_parallax_enabled = record.raster_debug_push.impostor_parallax_enabled ? 1u : 0u;
            vkCmdPushConstants(command_buffer, record.raster_layout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(skin_debug_push), &skin_debug_push);
            // The dynamic slot indexes the SKIN TAIL. Passing it raw put the
            // draw in cull.comp's bucket space -- slot 0 there is the gallery
            // world's Crate floor slab, whose scale(4, 0.1, 4) squashed the
            // creature flat.
            vkCmdDrawIndexed(command_buffer, draw.index_count, 1,
                             draw.first_index, static_cast<int32_t>(rebase),
                             record.skin_transform_base + draw.instance_slot);
        }
    }
    if (record.sparse_draw) record.sparse_draw(command_buffer);
    if (record.raster_stats_pool != VK_NULL_HANDLE)
        vkCmdEndQuery(command_buffer, record.raster_stats_pool, 0);
    vkCmdEndRendering(command_buffer);
    // WP-E: extract visible requests into this frame slot's compact readback
    // buffer; it is consumed at the next begin_frame on the same slot.
    if (record.vt_post) {
        PROFILE_SCOPE("raster.vt_post");
        if (time_vt) {
            write_ts(command_buffer, record.ts_pool, record.vt_feedback_zone, false);
            record.ts_written[record.vt_feedback_zone] |= 1u;
        }
        record.vt_post(command_buffer);
        if (time_vt) {
            write_ts(command_buffer, record.ts_pool, record.vt_feedback_zone, true);
            record.ts_written[record.vt_feedback_zone] |= 2u;
        }
    }
    if (record.ts_pool != VK_NULL_HANDLE && record.ts_written) {
        write_ts(command_buffer, record.ts_pool, record.gbuffer_zone, true);
        record.ts_written[record.gbuffer_zone] |= 2u;
    }

    for (uint32_t index = 0; index < 6; ++index) {
        auto* color = colors[index];
        const VkPipelineStageFlags2 sampled_stages = gbuffer_sampled_stages(
            index, record.native_ray_tracing_available);
        record_image_transition(
            command_buffer, *color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            sampled_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    record_image_transition(
        command_buffer, *record.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        ray_depth_destination_stages(
            record.native_ray_tracing_available),
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    z_gbuffer.stop();
}
}
