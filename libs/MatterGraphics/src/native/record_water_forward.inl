void VkSceneRenderer::record_raster_and_water(VkCommandBuffer command_buffer,
                                               void* user_data) {
    auto& raster = *static_cast<RasterRecord*>(user_data);
    record_raster(command_buffer, user_data);
    if (!raster.water_forward_owner || !raster.water_forward_record ||
        (raster.ray_trace_ok && !*raster.ray_trace_ok))
        return;
    auto& water = *static_cast<WaterForwardRecord*>(
        raster.water_forward_record);
    raster.water_forward_owner->record_water_forward(command_buffer, water);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (raster.recorded_water_draw_count && water.observation)
        *raster.recorded_water_draw_count = water.observation->direct_draws;
#endif
}

void VkSceneRenderer::record_water_forward(
    VkCommandBuffer command_buffer, const WaterForwardRecord& record) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (record.observation) *record.observation = {};
#endif
    if (record.extent.width == 0u || record.extent.height == 0u ||
        !record.hdr || !record.depth || !record.velocity ||
        !record.reactivity || !record.material_instance ||
        !record.opaque_hdr || !record.opaque_depth ||
        record.layout == VK_NULL_HANDLE)
        return;

    bool has_static = false;
    for (uint32_t range_index = 0u; range_index < record.range_count;
         ++range_index) {
        const PartCommandRange& range = record.ranges[range_index];
        if (range.raster_water_surface && range.command_count != 0u &&
            range.first_command <= record.static_command_count &&
            range.command_count <=
                record.static_command_count - range.first_command) {
            has_static = true;
            break;
        }
    }
    const bool has_direct =
        record.direct_pipeline != VK_NULL_HANDLE &&
        record.direct_vertices != VK_NULL_HANDLE &&
        record.direct_indices != VK_NULL_HANDLE &&
        record.direct_draws != nullptr && record.direct_draw_count != 0u;
    if ((!has_static || record.static_pipeline == VK_NULL_HANDLE ||
         record.static_vertices == VK_NULL_HANDLE ||
         record.static_indices == VK_NULL_HANDLE ||
         record.indirect == VK_NULL_HANDLE) &&
        !has_direct)
        return;

    const bool time_forward =
        record.timing_pool != VK_NULL_HANDLE &&
        record.timing_written != nullptr &&
        record.timing_zone < kGpuZoneCount;
    if (time_forward) {
        write_ts(command_buffer, record.timing_pool, record.timing_zone,
                 false);
        record.timing_written[record.timing_zone] |= 1u;
    }

    transition_for_use(command_buffer, *record.hdr,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    transition_for_use(command_buffer, *record.opaque_hdr,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCopy hdr_copy{};
    hdr_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hdr_copy.srcSubresource.layerCount = 1u;
    hdr_copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hdr_copy.dstSubresource.layerCount = 1u;
    hdr_copy.extent = {record.extent.width, record.extent.height, 1u};
    vkCmdCopyImage(command_buffer, record.hdr->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   record.opaque_hdr->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &hdr_copy);

    transition_for_use(command_buffer, *record.depth,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
    transition_for_use(command_buffer, *record.opaque_depth,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageCopy depth_copy{};
    depth_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_copy.srcSubresource.layerCount = 1u;
    depth_copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_copy.dstSubresource.layerCount = 1u;
    depth_copy.extent = {record.extent.width, record.extent.height, 1u};
    vkCmdCopyImage(command_buffer, record.depth->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   record.opaque_depth->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &depth_copy);

    transition_for_use(command_buffer, *record.opaque_hdr,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    transition_for_use(command_buffer, *record.opaque_depth,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);

    matter::VkImageResource* colors[] = {
        record.hdr, record.velocity, record.reactivity,
        record.material_instance};
    for (matter::VkImageResource* color : colors) {
        transition_for_use(command_buffer, *color,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    }
    transition_for_use(command_buffer, *record.depth,
                       VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo color_attachments[4]{};
    for (uint32_t index = 0u; index < 4u; ++index) {
        color_attachments[index].sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[index].imageView = colors[index]->view;
        color_attachments[index].imageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth_attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_attachment.imageView = record.depth->view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = record.extent;
    rendering.layerCount = 1u;
    rendering.colorAttachmentCount = 4u;
    rendering.pColorAttachments = color_attachments;
    rendering.pDepthAttachment = &depth_attachment;
    vkCmdBeginRendering(command_buffer, &rendering);

    const VkViewport viewport{
        0.0f, static_cast<float>(record.extent.height),
        static_cast<float>(record.extent.width),
        -static_cast<float>(record.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, record.extent};
    const VkDeviceSize vertex_offset = 0u;
    vkCmdSetViewport(command_buffer, 0u, 1u, &viewport);
    vkCmdSetScissor(command_buffer, 0u, 1u, &scissor);

    uint32_t issued_static = 0u;
    if (has_static && record.static_pipeline != VK_NULL_HANDLE &&
        record.static_vertices != VK_NULL_HANDLE &&
        record.static_indices != VK_NULL_HANDLE &&
        record.indirect != VK_NULL_HANDLE) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          record.static_pipeline);
        vkCmdBindDescriptorSets(command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                record.layout, 0u, 4u, record.sets, 0u,
                                nullptr);
        vkCmdPushConstants(command_buffer, record.layout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0u, sizeof(record.debug_push),
                           &record.debug_push);
        vkCmdBindVertexBuffers(command_buffer, 0u, 1u,
                               &record.static_vertices, &vertex_offset);
        vkCmdBindIndexBuffer(command_buffer, record.static_indices, 0u,
                             VK_INDEX_TYPE_UINT32);
        const uint32_t max_per_call =
            std::max(1u, record.max_draw_indirect_count);
        for (uint32_t range_index = 0u; range_index < record.range_count;
             ++range_index) {
            const PartCommandRange& range = record.ranges[range_index];
            if (!range.raster_water_surface || range.command_count == 0u ||
                range.first_command > record.static_command_count ||
                range.command_count >
                    record.static_command_count - range.first_command)
                continue;
            uint32_t first = range.first_command;
            uint32_t remaining = range.command_count;
            while (remaining != 0u) {
                const uint32_t count = std::min(remaining, max_per_call);
                vkCmdDrawIndexedIndirect(
                    command_buffer, record.indirect,
                    static_cast<VkDeviceSize>(first) * sizeof(DrawCommand),
                    count, sizeof(DrawCommand));
                ++issued_static;
                first += count;
                remaining -= count;
            }
        }
    }

    uint32_t issued_direct = 0u;
    const bool time_direct = has_direct &&
        record.timing_pool != VK_NULL_HANDLE &&
        record.timing_written != nullptr;
    if (time_direct) {
        write_ts(command_buffer, record.timing_pool,
                 kGpuZoneWaterDirectDraw, false);
        record.timing_written[kGpuZoneWaterDirectDraw] |= 1u;
    }
    if (has_direct) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          record.direct_pipeline);
        vkCmdBindDescriptorSets(command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                record.layout, 0u, 4u, record.sets, 0u,
                                nullptr);
        vkCmdBindVertexBuffers(command_buffer, 0u, 1u,
                               &record.direct_vertices, &vertex_offset);
        vkCmdBindIndexBuffer(command_buffer, record.direct_indices, 0u,
                             VK_INDEX_TYPE_UINT32);
        const RasterDebugPushConstants direct_base =
            make_raster_debug_push_constants(
                0u, true,
                record.debug_push.lod_tint_enabled != 0u
                    ? matter::GeometryDebugView::LodTint
                    : matter::GeometryDebugView::None,
                false,
                record.debug_push.impostor_parallax_enabled != 0u);
        for (uint32_t draw_index = 0u;
             draw_index < record.direct_draw_count; ++draw_index) {
            const VkWaterAnimationRasterDraw& draw =
                record.direct_draws[draw_index];
            if (draw.index_count == 0u || draw.index_count % 3u != 0u ||
                draw.first_index > record.direct_index_count ||
                draw.index_count >
                    record.direct_index_count - draw.first_index ||
                draw.vertex_count == 0u ||
                draw.vertex_offset > record.direct_vertex_count ||
                draw.vertex_count >
                    record.direct_vertex_count - draw.vertex_offset ||
                draw.vertex_offset > static_cast<uint32_t>(INT32_MAX) ||
                draw.proxy_transform_slot >= record.draw_transform_slots)
                continue;
            const RasterDebugPushConstants push =
                make_water_animation_push_constants(direct_base, draw);
            vkCmdPushConstants(command_buffer, record.layout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0u, sizeof(push), &push);
            vkCmdDrawIndexed(command_buffer, draw.index_count, 1u,
                             draw.first_index,
                             static_cast<int32_t>(draw.vertex_offset),
                             draw.proxy_transform_slot);
            ++issued_direct;
        }
    }
    if (time_direct) {
        write_ts(command_buffer, record.timing_pool,
                 kGpuZoneWaterDirectDraw, true);
        record.timing_written[kGpuZoneWaterDirectDraw] |= 2u;
    }
    vkCmdEndRendering(command_buffer);

    for (matter::VkImageResource* color : colors) {
        matter::record_image_transition(
            command_buffer, *color,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    matter::record_image_transition(
        command_buffer, *record.depth,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    if (time_forward) {
        write_ts(command_buffer, record.timing_pool, record.timing_zone,
                 true);
        record.timing_written[record.timing_zone] |= 2u;
    }

#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (record.observation) {
        // Indirect ranges with zero GPU instance counts are still submitted.
        // When direct animation owns this fixture, report visible ownership,
        // not the no-op indirect command-buffer call.
        record.observation->static_draws =
            issued_direct != 0u ? 0u : issued_static;
        record.observation->direct_draws = issued_direct;
        record.observation->copied_opaque_hdr = true;
        record.observation->copied_opaque_depth = true;
        record.observation->wrote_depth = true;
        record.observation->wrote_velocity = true;
        record.observation->wrote_reactivity = true;
        record.observation->wrote_identity = true;
    }
#endif
}
