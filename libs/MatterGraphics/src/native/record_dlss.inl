    const auto consume_bridge_reset = [this]() {
        if (!dlss_bridge_->consume_dlss_history_reset()) return;
        if (!dlss_history_reset_pending_) ++dlss_reset_count_;
        // A bridge mode/extent transition can already be represented by this
        // frame's GI reset. Coalesce only that bridge notification, never a
        // pending explicit RT/GI/light edit: resetting a GI lane does not
        // itself invalidate whole-frame temporal lighting.
        if (!gi_candidate_was_reset_)
            dlss_history_reset_pending_ = true;
        gi_history_reset_pending_ = true;
        local_direct_history_state_.reset_pending = true;
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
