bool VkSceneRenderer::test_dispatch_gi_temporal_fixture(
    const GiTemporalGpuFixture& fixture, GiTemporalGpuResult& result,
    std::string& error) {
    result = {};
    // Test-only diagnostics may run immediately after an asynchronous smoke
    // frame. Recycle descriptor sets only after that submitted work is done.
    vulkan_->wait_idle();
    const VkExtent2D fixture_extent = gi_signal_extent(fixture.signal_mode);
    if (fixture.signal_mode > 3u ||
        (fixture.reuse_previous_output && fixture.signal_mode != 0u) ||
        frames_.empty() || raw_diffuse_.image == VK_NULL_HANDLE ||
        fixture.output_pixel.x < 0 || fixture.output_pixel.y < 0 ||
        fixture.output_pixel.x >= static_cast<int>(fixture_extent.width) ||
        fixture.output_pixel.y >= static_cast<int>(fixture_extent.height)) {
        error = "GI temporal GPU fixture requires initialized in-bounds targets";
        return false;
    }
    for (const auto& patch : fixture.current_patches) {
        if (patch.pixel.x < 0 || patch.pixel.y < 0 ||
            patch.pixel.x >= static_cast<int>(fixture_extent.width) ||
            patch.pixel.y >= static_cast<int>(fixture_extent.height)) {
            error = "GI temporal current patch is outside the signal extent";
            return false;
        }
    }
    // History count plus optional previous radiance occupy sixteen bytes;
    // every rgba16f/rg32ui patch remains eight-byte aligned.
    const VkDeviceSize upload_size = 16u + 32u * fixture.current_patches.size();
    matter::VkBufferResource upload;
    matter::VkBufferResource readback;
    if (!matter::create_buffer(
            *vulkan_, upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
    const uint16_t previous_raw[4] = {
        float_to_half(fixture.previous_radiance_patch.x),
        float_to_half(fixture.previous_radiance_patch.y),
        float_to_half(fixture.previous_radiance_patch.z),
        float_to_half(fixture.previous_radiance_patch.w)};
    std::memcpy(static_cast<uint8_t*>(upload.mapped) + 8u,
                previous_raw, sizeof(previous_raw));
    const auto fixture_half = [](float value) {
        return std::isnan(value) ? uint16_t{0x7e00u} : float_to_half(value);
    };
    for (size_t i = 0; i < fixture.current_patches.size(); ++i) {
        const auto& patch = fixture.current_patches[i];
        auto* bytes = static_cast<uint8_t*>(upload.mapped) + 16u + 32u * i;
        const uint16_t raw[4] = {fixture_half(patch.raw.x),
            fixture_half(patch.raw.y), fixture_half(patch.raw.z),
            fixture_half(patch.raw.w)};
        const uint16_t normal[4] = {float_to_half(patch.normal.x),
            float_to_half(patch.normal.y), float_to_half(patch.normal.z),
            float_to_half(patch.normal.w)};
        const uint32_t identity[2] = {patch.material_index, patch.instance_token};
        std::memcpy(bytes, raw, sizeof(raw));
        std::memcpy(bytes + 8, &patch.depth, sizeof(patch.depth));
        std::memcpy(bytes + 16, normal, sizeof(normal));
        std::memcpy(bytes + 24, identity, sizeof(identity));
    }
    if (!matter::flush_buffer(upload, 0, upload_size, error)) return false;

    struct FixtureRecord {
        VkSceneRenderer* renderer;
        const GiTemporalGpuFixture* fixture;
        VkBuffer upload;
        VkBuffer readback;
        bool ok = true;
        std::string* error;
    } record{this, &fixture, upload.buffer, readback.buffer, true, &error};
    const uint64_t saved_presented_token = gi_presented_attempt_token_;
    const uint64_t saved_candidate_scene_key = gi_candidate_scene_key_;
    gi_candidate_scene_key_ = gi_presented_scene_key_;
    const uint64_t saved_candidate_serial = gi_candidate_frame_serial_;
    const uint64_t saved_candidate_token = gi_candidate_attempt_token_;
    const bool saved_candidate_reset = gi_candidate_was_reset_;
    const bool saved_composite = last_composite_used_gi_temporal_;
    const TemporalFrame saved_temporal = temporal_frame_;
    const auto saved_direct = local_direct_history_state_;
    const bool saved_history_reset_pending = gi_history_reset_pending_;
    const bool saved_diffuse_reset_pending = gi_diffuse_history_reset_pending_;
    const bool saved_reflection_reset_pending = gi_reflection_history_reset_pending_;
    // Fixtures own their reset bit, including before a renderer has presented
    // any RT frame and while independently probing a newly resized target.
    gi_history_reset_pending_ = false;
    gi_diffuse_history_reset_pending_ = false;
    gi_reflection_history_reset_pending_ = false;
    const uint32_t saved_presented_index = gi_presented_history_index_;
    if (fixture.reuse_previous_output)
        gi_presented_history_index_ = gi_candidate_history_index_;
    if (fixture.signal_mode == 3u) {
        local_direct_history_state_.presented_token = 1;
        local_direct_history_state_.reset_pending = false;
        local_direct_history_state_.candidate_scene_key =
            local_direct_history_state_.presented_scene_key;
    }
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
            f.signal_mode == 3u ? renderer.raw_local_direct_
            : f.signal_mode == 0u ? renderer.raw_diffuse_
            : f.signal_mode == 1u ? renderer.raw_specular_
                                 : renderer.raw_transmission_;
        clear_color(raw_signal, value);
        value = {};
        value.float32[0] = f.raw_aux.x;
        value.float32[1] = f.raw_aux.y;
        clear_color(f.signal_mode == 3u ? renderer.orm_
                    : f.signal_mode == 2u ? renderer.raw_transmission_aux_
                                         : renderer.raw_specular_aux_, value);
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
        value = {};
        value.float32[0] = f.reactivity;
        clear_color(renderer.reactivity_, value);
        const auto patch_texel = [&](matter::VkImageResource& image,
                VkDeviceSize offset, GiPixelCoord pixel, VkImageAspectFlags aspect) {
            transition_for_use(command_buffer, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, aspect);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = aspect;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = {pixel.x, pixel.y, 0};
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(command_buffer, item.upload, image.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        };
        const VkExtent2D signal_extent = renderer.gi_signal_extent(f.signal_mode);
        for (size_t i = 0; i < f.current_patches.size(); ++i) {
            const auto& patch = f.current_patches[i];
            const VkDeviceSize base = 16u + 32u * i;
            patch_texel(raw_signal, base, patch.pixel, VK_IMAGE_ASPECT_COLOR_BIT);
            if (!patch.patch_geometry) continue;
            const GiPixelCoord source{
                static_cast<int>((static_cast<float>(patch.pixel.x) + 0.5f) *
                    static_cast<float>(renderer.raster_extent_.width) /
                    static_cast<float>(signal_extent.width)),
                static_cast<int>((static_cast<float>(patch.pixel.y) + 0.5f) *
                    static_cast<float>(renderer.raster_extent_.height) /
                    static_cast<float>(signal_extent.height))};
            patch_texel(renderer.depth_, base + 8u, source, VK_IMAGE_ASPECT_DEPTH_BIT);
            patch_texel(renderer.normal_, base + 16u, source, VK_IMAGE_ASPECT_COLOR_BIT);
            patch_texel(renderer.material_instance_, base + 24u, source,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        }
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

        GiHistorySet* histories = f.signal_mode == 3u
            ? renderer.local_direct_history_ : f.signal_mode == 0u
            ? renderer.gi_history_ : f.signal_mode == 1u
            ? renderer.gi_spec_history_ : renderer.gi_trans_history_;
        GiHistorySet& previous =
            histories[f.signal_mode == 3u
                ? renderer.local_direct_history_state_.presented_index
                : renderer.gi_presented_history_index_];
        if (!f.reuse_previous_output) {
        value = {};
        value.float32[0] = f.previous_radiance.x;
        value.float32[1] = f.previous_radiance.y;
        value.float32[2] = f.previous_radiance.z;
        value.float32[3] = f.previous_radiance.w;
        clear_color(previous.radiance, value);
        if (f.patch_previous_radiance)
            patch_texel(previous.radiance, 8u, f.history_patch_pixel,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        value = {};
        value.float32[0] = f.previous_moments.x;
        value.float32[1] = f.previous_moments.y;
        clear_color(previous.moments, value);
        value = {};
        value.uint32[0] = std::min(kGiHistoryLengthMax,
                                  f.previous_history_background_length);
        clear_color(previous.history_length, value);
        value = {};
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
        }
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
            histories[f.signal_mode == 3u
                ? renderer.local_direct_history_state_.candidate_index
                : renderer.gi_candidate_history_index_];
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
         raw_transmission_.lifetime, raw_transmission_aux_.lifetime,
         velocity_.lifetime, depth_.lifetime, normal_.lifetime,
         material_instance_.lifetime, reactivity_.lifetime,
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
         gi_spec_history_[1].aux.lifetime,
         gi_trans_history_[0].radiance.lifetime,
         gi_trans_history_[0].moments.lifetime,
         gi_trans_history_[0].history_length.lifetime,
         gi_trans_history_[0].depth.lifetime,
         gi_trans_history_[0].normal.lifetime,
         gi_trans_history_[0].identity.lifetime,
         gi_trans_history_[0].rejection.lifetime,
         gi_trans_history_[0].aux.lifetime,
         gi_trans_history_[1].radiance.lifetime,
         gi_trans_history_[1].moments.lifetime,
         gi_trans_history_[1].history_length.lifetime,
         gi_trans_history_[1].depth.lifetime,
         gi_trans_history_[1].normal.lifetime,
         gi_trans_history_[1].identity.lifetime,
         gi_trans_history_[1].rejection.lifetime,
         gi_trans_history_[1].aux.lifetime});
    gi_presented_attempt_token_ = saved_presented_token;
    gi_candidate_scene_key_ = saved_candidate_scene_key;
    gi_candidate_frame_serial_ = saved_candidate_serial;
    gi_candidate_attempt_token_ = saved_candidate_token;
    gi_candidate_was_reset_ = saved_candidate_reset;
    last_composite_used_gi_temporal_ = saved_composite;
    temporal_frame_ = saved_temporal;
    local_direct_history_state_ = saved_direct;
    gi_history_reset_pending_ = saved_history_reset_pending;
    gi_diffuse_history_reset_pending_ = saved_diffuse_reset_pending;
    gi_reflection_history_reset_pending_ = saved_reflection_reset_pending;
    gi_presented_history_index_ = saved_presented_index;
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
        fixture.history_length.size() != pixel_count ||
        (!fixture.instance_token.empty() && fixture.instance_token.size() != pixel_count)) {
        error = "GI A-trous GPU fixture requires complete inputs at least 33 pixels wide";
        return false;
    }
    vulkan_->wait_idle();
    const float saved_scale = gi_settings_.trace_scale;
    const float saved_reflection_scale = gi_settings_.reflection_trace_scale;
    gi_settings_.trace_scale = 1.0f;
    gi_settings_.reflection_trace_scale = 1.0f;
    if (!ensure_raster_targets(fixture.extent.width, fixture.extent.height,
                               error)) {
        gi_settings_.trace_scale = saved_scale;
        gi_settings_.reflection_trace_scale = saved_reflection_scale;
        return false;
    }
    gi_settings_.trace_scale = saved_scale;
    gi_settings_.reflection_trace_scale = saved_reflection_scale;

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
        const uint32_t identity[2] = {fixture.material_index[i],
            fixture.instance_token.empty() ? 1u : fixture.instance_token[i]};
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
        uint32_t signal_mode;
        bool ok = true;
        std::string* error;
    } record{this, upload.buffer, readback.buffer,
             {signal_offset, moments_offset, depth_offset, normal_offset,
              identity_offset, history_offset}, fixture.extent, fixture.signal_mode, true, &error};
    const uint64_t saved_candidate_serial = gi_candidate_frame_serial_;
    const uint32_t saved_composite_index = gi_composite_history_index_;
    const bool saved_filtered_valid = gi_filtered_valid_;
    const uint32_t saved_filtered_index = gi_filtered_index_;
    const auto saved_direct = local_direct_history_state_;
    if (fixture.signal_mode == 3u)
        local_direct_history_state_.candidate_serial = kGiTestAtrousToken;
    gi_composite_history_index_ = gi_candidate_history_index_;
    gi_candidate_frame_serial_ = kGiTestAtrousToken;
    const auto callback = [](VkCommandBuffer command_buffer, void* opaque) {
        auto& item = *static_cast<FixtureRecord*>(opaque);
        VkSceneRenderer& renderer = *item.renderer;
        GiHistorySet& guide =
            item.signal_mode == 3u
                ? renderer.local_direct_history_[renderer.local_direct_history_state_.candidate_index]
                : renderer.gi_history_[renderer.gi_composite_history_index_];
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
        transition_for_use(command_buffer, renderer.reactivity_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        const VkClearColorValue zero_reactivity{};
        const VkImageSubresourceRange reactivity_range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command_buffer, renderer.reactivity_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zero_reactivity, 1, &reactivity_range);
        if (item.signal_mode == 3u) {
            transition_for_use(command_buffer, guide.aux,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdClearColorImage(command_buffer, guide.aux.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zero_reactivity, 1, &reactivity_range);
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
        item.ok = item.signal_mode == 3u
            ? renderer.record_gi_atrous_signal(fake, 3u, *item.error, false)
            : renderer.record_gi_atrous(fake, *item.error, false);
        if (!item.ok) return;
        matter::VkImageResource* outputs[2] = {
            item.signal_mode == 3u ? &renderer.local_direct_filtered_
                : &renderer.gi_atrous_[renderer.gi_filtered_index_],
            item.signal_mode == 3u ? &guide.radiance
                : &renderer.gi_atrous_[renderer.gi_filtered_index_ ^ 1u]};
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
         reactivity_.lifetime,
         gi_atrous_[0].lifetime, gi_atrous_[1].lifetime,
         frames_[active_frame_index_].gi_atrous_markers.lifetime});
    gi_candidate_frame_serial_ = saved_candidate_serial;
    gi_composite_history_index_ = saved_composite_index;
    gi_filtered_valid_ = saved_filtered_valid;
    gi_filtered_index_ = saved_filtered_index;
    local_direct_history_state_ = saved_direct;
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
