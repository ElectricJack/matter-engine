// ---------------------------------------------------------------------------
// RT denoiser: temporal reprojection, then A-trous
//
// Four independent signals share one pipeline pair, selected by
// `signal_mode`: 0 = diffuse GI, 1 = specular, 2 = transmission,
// 3 = full-rate local direct (independent of optional GI). Each has its
// own double-buffered GiHistorySet (presented / candidate, swapped only when
// finish_ray_tracing_frame reports the frame succeeded) and its own pair of
// ping-ponged A-trous targets.
// ---------------------------------------------------------------------------

// Runs the temporal pass for all three signals. `retain` adds the images to
// the frame's retention list and is false only on the test-fixture paths that
// submit immediately and wait.
bool VkSceneRenderer::record_gi_temporal(const matter::VulkanFrame& frame,
                                         std::string& error, bool retain) {
    gi_candidate_was_reset_ = false;
    gi_candidate_used_diffuse_reset_ = false;
    gi_candidate_used_reflection_reset_ = false;
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
    const bool direct = signal_mode == 3u;
    auto& direct_state = local_direct_history_state_;
    const VkExtent2D signal_extent = gi_signal_extent(signal_mode);
    const uint32_t previous_index = direct ? direct_state.presented_index
                                            : gi_presented_history_index_;
    const uint32_t candidate_index = previous_index ^ 1u;
    GiHistorySet* histories = direct ? local_direct_history_
                              : signal_mode == 0u   ? gi_history_
                              : signal_mode == 1u ? gi_spec_history_
                                                  : gi_trans_history_;
    GiHistorySet& previous = histories[previous_index];
    GiHistorySet& candidate = histories[candidate_index];
    matter::VkImageResource& raw_signal =
        direct ? raw_local_direct_
        : signal_mode == 0u   ? raw_diffuse_
        : signal_mode == 1u ? raw_specular_
                            : raw_transmission_;
    // Diffuse ignores aux, but the shared shader still fetches/stores it.
    // Bind an in-bounds signal-sized image when the secondary extent differs.
    // Direct reads primary ORM; secondary signals read (hit distance, roughness).
    matter::VkImageResource& raw_aux =
        direct ? orm_
        : signal_mode == 2u ? raw_transmission_aux_
        : signal_mode == 0u &&
              (raw_diffuse_extent_.width != raw_reflection_extent_.width ||
               raw_diffuse_extent_.height != raw_reflection_extent_.height)
            ? raw_diffuse_ : raw_specular_aux_;
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
    sampled(reactivity_);
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

    VkDescriptorImageInfo infos[22]{};
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
    combined(21, reactivity_);
    matter::VkImageResource* outputs[] = {
        &candidate.radiance, &candidate.moments, &candidate.history_length,
        &candidate.depth, &candidate.normal, &candidate.identity,
        &candidate.rejection};
    for (uint32_t binding = 11; binding < 18; ++binding)
        infos[binding] = {VK_NULL_HANDLE, outputs[binding - 11]->view,
                          VK_IMAGE_LAYOUT_GENERAL};
    infos[20] = {VK_NULL_HANDLE, candidate.aux.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[22]{};
    FrameResources& resources = frames_[frame.frame_slot];
    const VkDescriptorSet temporal_set =
        direct ? resources.local_direct_temporal_descriptor_set
               : resources.gi_temporal_descriptor_sets[signal_mode];
    for (uint32_t binding = 0; binding < 22; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = temporal_set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            binding <= 10 || binding == 18 || binding == 19 || binding == 21
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[binding].pImageInfo = &infos[binding];
    }
    update_descriptor_sets(vulkan_->device(), 22, writes, 0, nullptr);
    VulkanGiTemporalConstants constants{};
    constants.temporal_extent[0] = signal_extent.width;
    constants.temporal_extent[1] = signal_extent.height;
    constants.gbuffer_extent[0] = raster_extent_.width;
    constants.gbuffer_extent[1] = raster_extent_.height;
    const bool selective_reset =
        direct ? direct_state.reset_pending
        : signal_mode == 0u ? gi_diffuse_history_reset_pending_
                          : gi_reflection_history_reset_pending_;
    constants.reset = temporal_frame_.reset || selective_reset ||
        (direct ? direct_state.presented_token == 0 ||
                      direct_state.candidate_scene_key != direct_state.presented_scene_key
                : gi_history_reset_pending_ || gi_presented_attempt_token_ == 0 ||
                      (signal_mode <= 1u &&
                       gi_candidate_scene_key_ != gi_presented_scene_key_));
    if (!direct) {
        gi_candidate_was_reset_ = gi_candidate_was_reset_ || constants.reset != 0;
        gi_candidate_used_diffuse_reset_ =
            gi_candidate_used_diffuse_reset_ ||
            (signal_mode == 0u && selective_reset);
        gi_candidate_used_reflection_reset_ =
            gi_candidate_used_reflection_reset_ ||
            (signal_mode != 0u && selective_reset);
    }
    constants.attempt_token_lo =
        static_cast<uint32_t>(temporal_frame_.attempt_token);
    constants.presented_attempt_token_lo =
        static_cast<uint32_t>(direct ? direct_state.presented_token
                                     : gi_presented_attempt_token_);
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
                  (signal_extent.width + 7u) / 8u,
                  (signal_extent.height + 7u) / 8u, 1);
    matter::record_image_transition(
        frame.command_buffer, candidate.radiance,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (direct) {
        direct_state.candidate_reset = constants.reset != 0;
        direct_state.candidate_index = candidate_index;
        direct_state.candidate_serial = frame.serial;
        direct_state.candidate_token = temporal_frame_.attempt_token;
        direct_state.filtered_valid = false;
    } else {
        gi_candidate_history_index_ = candidate_index;
        gi_composite_history_index_ = candidate_index;
        gi_candidate_frame_serial_ = frame.serial;
        gi_candidate_attempt_token_ = temporal_frame_.attempt_token;
    }
    update_composite_descriptor(resources);
    std::vector<std::shared_ptr<void>> retained{
        raw_signal.lifetime, raw_aux.lifetime, velocity_.lifetime, depth_.lifetime,
        normal_.lifetime, material_instance_.lifetime, reactivity_.lifetime};
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

// Edge-avoiding A-trous wavelet filter over one signal's temporal output,
// guided by that signal's depth / normal / identity / history-length images.
// Step widths 1,2,4,8,16; FIVE iterations for diffuse and THREE for the two
// sharper signals, ping-ponging between two targets -- which is why the final
// image is filtered[(iteration_count - 1) & 1] and not a fixed index.
//
// Three descriptor sets per signal, not one per iteration: iteration 0 reads
// the history, odd iterations read filtered[0] and even ones filtered[1], so
// the bindings repeat after the first.
bool VkSceneRenderer::record_gi_atrous_signal(
    const matter::VulkanFrame& frame, uint32_t signal_mode,
    std::string& error, bool retain) {
    const bool direct = signal_mode == 3u;
    auto& direct_state = local_direct_history_state_;
    const VkExtent2D signal_extent = gi_signal_extent(signal_mode);
    if (frame.frame_slot >= frames_.size() ||
        gi_atrous_pipeline_ == VK_NULL_HANDLE ||
        (direct ? direct_state.candidate_serial : gi_candidate_frame_serial_) == 0)
        return true;
    GiHistorySet& guide =
        (direct ? local_direct_history_
         : signal_mode == 0u   ? gi_history_
         : signal_mode == 1u ? gi_spec_history_
                             : gi_trans_history_)[direct ? direct_state.candidate_index
                                                      : gi_composite_history_index_];
    matter::VkImageResource* filtered =
        direct ? &local_direct_filtered_
        : signal_mode == 0u   ? gi_atrous_
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
    sampled(reactivity_);
    for (uint32_t output_index = 0; output_index < (direct ? 1u : 2u); ++output_index) {
        auto& output = filtered[output_index];
        transition_for_use(frame.command_buffer, output,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    }

    FrameResources& resources = frames_[frame.frame_slot];
    if (direct) {
        VkMemoryBarrier2 marker_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        marker_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        marker_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        marker_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        marker_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &marker_barrier;
        vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    }
    matter::VkImageResource* inputs[3] = {
        &guide.radiance, &filtered[0], &filtered[direct ? 0 : 1]};
    matter::VkImageResource* outputs[3] = {
        &filtered[0], &filtered[direct ? 0 : 1], &filtered[0]};
    for (uint32_t set_index = 0; set_index < (direct ? 1u : 3u); ++set_index) {
        const VkDescriptorSet descriptor = direct
            ? resources.local_direct_filter_descriptor_set
            : resources.gi_atrous_descriptor_sets[signal_mode * 3u + set_index];
        matter::VkImageResource* sampled_images[6] = {
            inputs[set_index], &guide.moments, &guide.depth,
            &guide.normal, &guide.identity, &guide.history_length};
        VkDescriptorImageInfo infos[7]{};
        VkWriteDescriptorSet writes[10]{};
        for (uint32_t binding = 0; binding < 6; ++binding) {
            infos[binding] = {composite_sampler_, sampled_images[binding]->view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = descriptor;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &infos[binding];
        }
        infos[6] = {VK_NULL_HANDLE, outputs[set_index]->view,
                    VK_IMAGE_LAYOUT_GENERAL};
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = descriptor;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[6].pImageInfo = &infos[6];
        const VkDescriptorBufferInfo marker_info{
            resources.gi_atrous_markers.buffer, 0,
            5 * sizeof(uint32_t)};
        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = descriptor;
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[7].pBufferInfo = &marker_info;
        VkDescriptorImageInfo aux_info{composite_sampler_, guide.aux.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = descriptor;
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[8].pImageInfo = &aux_info;
        VkDescriptorImageInfo reactivity_info{
            composite_sampler_, reactivity_.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[9].dstSet = descriptor;
        writes[9].dstBinding = 9;
        writes[9].descriptorCount = 1;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[9].pImageInfo = &reactivity_info;
        update_descriptor_sets(vulkan_->device(), 10, writes, 0, nullptr);
    }

    constexpr uint32_t steps[5] = {1, 2, 4, 8, 16};
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      gi_atrous_pipeline_);
    const uint32_t iteration_count = direct ? 1u : signal_mode == 0u ? 5u : 3u;
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
            direct ? resources.local_direct_filter_descriptor_set
                   : resources.gi_atrous_descriptor_sets[signal_mode * 3u + set_index];
        vkCmdBindDescriptorSets(frame.command_buffer,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                gi_atrous_pipeline_layout_, 0, 1, &set, 0,
                                nullptr);
        VulkanGiAtrousConstants constants{};
        constants.extent[0] = signal_extent.width;
        constants.extent[1] = signal_extent.height;
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
                      (signal_extent.width + 7u) / 8u,
                      (signal_extent.height + 7u) / 8u, 1);
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
    if (direct) {
        direct_state.filtered_valid = true;
        update_composite_descriptor(resources);
    } else {
        gi_filtered_index_ = (iteration_count - 1u) & 1u;
    }
    if (!retain) return true;
    return vulkan_->retain_for_frame(
        frame,
        {guide.radiance.lifetime, guide.moments.lifetime,
         guide.depth.lifetime, guide.normal.lifetime,
         guide.identity.lifetime, guide.history_length.lifetime,
         guide.aux.lifetime, reactivity_.lifetime,
         filtered[0].lifetime, filtered[direct ? 0 : 1].lifetime,
         resources.gi_atrous_markers.lifetime},
        error);
}
