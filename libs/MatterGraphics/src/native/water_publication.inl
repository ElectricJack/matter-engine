bool VkSceneRenderer::publish_water_field(
    const PackedWaterField& candidate, const WaterFieldBinding* replacing,
    std::uint64_t retire_after_serial, WaterFieldBinding& binding,
    WaterFieldError& error) {
    if (!initialized_) {
        std::string init_error;
        if (!init(init_error)) {
            error.code = WaterFieldErrorCode::UploadFailure;
            error.message = "water field renderer initialization failed: " +
                            init_error;
            return false;
        }
    }
    WaterFieldVkResources::StagedImages staged;
    if (!water_field_resources_.stage(candidate, staged, error)) return false;
    if (!water_fields_.publish(candidate, replacing, retire_after_serial,
                               binding, error))
        return false;
    water_field_resources_.commit(binding, std::move(staged));
    return true;
}

bool VkSceneRenderer::release_water_field(
    WaterFieldBinding binding, std::uint64_t retire_after_serial,
    WaterFieldError& error) {
    if (!water_fields_.release(binding, retire_after_serial, error))
        return false;
    water_field_resources_.release(binding, retire_after_serial);
    return true;
}

void VkSceneRenderer::collect_water_fields(
    std::uint64_t completed_serial) noexcept {
    water_fields_.collect(completed_serial);
    water_field_resources_.collect(completed_serial);
}

VkImageView VkSceneRenderer::test_water_field_image_view(
    WaterFieldBinding binding, std::uint32_t channel) const noexcept {
    return water_field_resources_.image_view(binding, channel);
}

VkSampler VkSceneRenderer::test_water_field_sampler(
    std::uint32_t channel) const noexcept {
    return water_field_resources_.sampler(channel);
}

WaterFieldGpuRecord VkSceneRenderer::test_water_field_gpu_record(
    std::uint32_t slot) const noexcept {
    const auto records = water_fields_.gpu_records();
    return slot < records.size() ? records[slot] : WaterFieldGpuRecord{};
}

bool VkSceneRenderer::test_water_field_descriptors_match(
    std::uint32_t frame_slot, WaterFieldBinding binding) const noexcept {
    if (!binding.valid() || frame_slot >= frames_.size()) return false;
    const FrameResources& frame = frames_[frame_slot];
    if (!frame.water_field_raster_descriptors_valid ||
        (vulkan_->ray_tracing_available() &&
         !frame.water_field_rt_descriptors_valid) ||
        frame.water_field_records.buffer == VK_NULL_HANDLE ||
        frame.water_field_generations[binding.slot] != binding.generation)
        return false;
    for (std::uint32_t channel = 0u; channel != 4u; ++channel) {
        if (frame.water_field_views[channel][binding.slot] !=
            water_field_resources_.image_view(binding, channel))
            return false;
    }
    return true;
}

bool VkSceneRenderer::publish_water_animation(
    std::uint64_t generation, std::uint32_t frame_slots,
    const WaterAnimationGpuCapacity& capacity,
    std::uint64_t retire_after_serial, WaterAnimationGpuError& error) {
    error = {};
    if (!initialized_) {
        std::string init_error;
        if (!init(init_error)) {
            error = {WaterAnimationGpuErrorCode::InvalidConfiguration,
                     "water animation renderer initialization failed: " +
                         init_error};
            return false;
        }
    }
    if (generation == 0u || frame_slots == 0u || !capacity.valid() ||
        capacity.gpu_bytes_per_slot() == 0u ||
        generation <= water_animation_schedule_.generation()) {
        error = {generation <= water_animation_schedule_.generation()
                     ? WaterAnimationGpuErrorCode::StaleGeneration
                     : WaterAnimationGpuErrorCode::InvalidConfiguration,
                 "water animation Vulkan publication is invalid or stale"};
        return false;
    }
    if (capacity.packed_vertex_bytes > limits_.max_buffer_size ||
        capacity.index_bytes > limits_.max_buffer_size) {
        error = {WaterAnimationGpuErrorCode::CapacityExceeded,
                 "water animation buffers exceed Vulkan device limits"};
        return false;
    }

    WaterAnimationVulkanGeneration candidate{};
    candidate.generation = generation;
    candidate.frames.resize(frame_slots);
    const auto reject_candidate = [&](const std::string& message) {
        candidate.frames.clear();
        error = {WaterAnimationGpuErrorCode::InvalidConfiguration, message};
        return false;
    };

    std::string vk_error;
    for (std::uint32_t slot = 0u; slot != frame_slots; ++slot) {
        WaterAnimationVulkanFrame& frame = candidate.frames[slot];
        if (!matter::create_buffer(
                *vulkan_, capacity.packed_vertex_bytes,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                kWaterAnimationRequiredMemory,
                kWaterAnimationPreferredMemory,
                frame.vertices, vk_error) ||
            !matter::create_buffer(
                *vulkan_, capacity.index_bytes,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                kWaterAnimationRequiredMemory,
                kWaterAnimationPreferredMemory,
                frame.indices, vk_error))
            return reject_candidate(
                "water animation Vulkan allocation failed: " + vk_error);
    }

    WaterAnimationGpuError schedule_error{};
    if (!water_animation_schedule_.publish(
            generation, frame_slots, capacity, retire_after_serial,
            schedule_error))
        return reject_candidate(schedule_error.message);
    if (water_animation_resources_.generation != 0u ||
        !water_animation_resources_.frames.empty()) {
        retired_water_animation_resources_.emplace_back();
        auto& retired = retired_water_animation_resources_.back();
        retired.retire_after_serial = retire_after_serial;
        retired.resources.generation = std::exchange(
            water_animation_resources_.generation, 0u);
        retired.resources.frames =
            std::move(water_animation_resources_.frames);
    }
    water_animation_resources_ = std::move(candidate);
    water_animation_proxy_instance_indices_.assign(capacity.draw_count,
                                                    UINT32_MAX);
    water_animation_direct_transform_slots_.resize(capacity.draw_count);
    command_template_dirty_ = true;
    dynamic_dirty_ = true;
    return true;
}

bool VkSceneRenderer::prepare_water_animation_frame(
    std::uint64_t generation, std::uint32_t frame_slot,
    const WaterAnimationFrameSelection& selection,
    const std::vector<std::uint32_t>& proxy_transform_slots,
    WaterAnimationGpuError& error) {
    if (proxy_transform_slots.size() !=
        water_animation_proxy_instance_indices_.size()) {
        error = {WaterAnimationGpuErrorCode::InvalidSelection,
                 "water animation static proxy mapping changed size"};
        return false;
    }
    water_animation_proxy_instance_indices_ = proxy_transform_slots;
    for (std::size_t index = 0u;
         index != water_animation_direct_transform_slots_.size();
         ++index)
        water_animation_direct_transform_slots_[index] =
            water_animation_transform_base_ +
            static_cast<std::uint32_t>(index);
    if (!water_animation_schedule_.prepare(
            generation, frame_slot, selection,
            water_animation_direct_transform_slots_, error))
        return false;
    const WaterAnimationGpuFrame* prepared =
        water_animation_schedule_.frame(frame_slot);
    if (!prepared ||
        water_animation_resources_.generation != generation ||
        frame_slot >= water_animation_resources_.frames.size()) {
        error = {WaterAnimationGpuErrorCode::StaleGeneration,
                 "water animation Vulkan resources are stale"};
        return false;
    }
    if (!prepared->upload_required) return true;
    WaterAnimationVulkanFrame& resources =
        water_animation_resources_.frames[frame_slot];
    std::string upload_error;
    if (!matter::upload_buffer(
            *vulkan_, resources.vertices,
            prepared->packed_vertices.data(),
            prepared->packed_vertices.size(), 0u, upload_error) ||
        !matter::upload_buffer(
            *vulkan_, resources.indices, prepared->indices.data(),
            prepared->indices.size() * sizeof(std::uint32_t), 0u,
            upload_error)) {
        error = {WaterAnimationGpuErrorCode::InvalidConfiguration,
                 "water animation Vulkan upload failed: " + upload_error};
        return false;
    }
    ++water_animation_upload_count_;
    return true;
}

void VkSceneRenderer::clear_water_animation(
    std::uint64_t retire_after_serial) {
    water_animation_schedule_.clear(retire_after_serial);
    if (water_animation_resources_.generation != 0u ||
        !water_animation_resources_.frames.empty()) {
        retired_water_animation_resources_.emplace_back();
        auto& retired = retired_water_animation_resources_.back();
        retired.retire_after_serial = retire_after_serial;
        retired.resources.generation = std::exchange(
            water_animation_resources_.generation, 0u);
        retired.resources.frames =
            std::move(water_animation_resources_.frames);
    }
    water_animation_proxy_instance_indices_.clear();
    water_animation_direct_transform_slots_.clear();
    command_template_dirty_ = true;
    dynamic_dirty_ = true;
}

void VkSceneRenderer::collect_water_animation(
    std::uint64_t completed_serial) noexcept {
    water_animation_schedule_.collect(completed_serial);
    retired_water_animation_resources_.erase(
        std::remove_if(
            retired_water_animation_resources_.begin(),
            retired_water_animation_resources_.end(),
            [completed_serial](
                RetiredWaterAnimationVulkanGeneration& retired) {
                if (retired.retire_after_serial > completed_serial)
                    return false;
                retired.resources.frames.clear();
                return true;
            }),
        retired_water_animation_resources_.end());
}
