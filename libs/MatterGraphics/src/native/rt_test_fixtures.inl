VkDeviceAddress VkSceneRenderer::test_rt_geometry_address(
    uint64_t part_hash) const {
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) return 0;
    const PartRecord& part = parts_[static_cast<size_t>(found->second)];
    return !part.geometry_raster_only && part.vertex_count && vertices_.address
        ? vertices_.address + VkDeviceSize{part.vertex_start} * sizeof(VkRasterVertex)
        : 0;
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
    if (!vulkan_->ray_tracing_available() || rt_surface_test_pipeline_.pipeline == VK_NULL_HANDLE ||
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
    // record_test_surface_ray() is appended to a production cull/render pass.
    // That pass already published and bound selected.environment_descriptor_set.
    // Rewriting it here invalidates the still-recording command buffer because
    // the layout deliberately does not use UPDATE_AFTER_BIND. Keep the same
    // descriptor snapshot for both traces; a later frame publishes any newer
    // cloud-shadow ping before either trace is bound.
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
    // raw_transmission_aux_ (binding 20, RT PBR Phase 1) and the local-direct
    // output (binding 26).
    for (auto* image : {&raw_specular_, &raw_specular_aux_, &raw_transmission_,
                        &raw_transmission_aux_, &raw_local_direct_})
        transition_for_use(frame.command_buffer, *image,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdBindPipeline(frame.command_buffer,
                      VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_surface_test_pipeline_.pipeline);
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
    const VkStridedDeviceAddressRegionKHR callable{};
    trace(frame.command_buffer, &rt_surface_test_pipeline_.raygen,
          &rt_surface_test_pipeline_.miss, &rt_surface_test_pipeline_.hit,
          &callable, 1, 1, 1);
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
                                   32 * sizeof(uint32_t), error) ||
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
    hit.vt_normal = {as_float(words[29]), as_float(words[30]), as_float(words[31])};
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

bool VkSceneRenderer::test_poison_rt_counters(uint32_t value,
                                              std::string& error) {
    error.clear();
    for (FrameResources& slot : frames_) {
        if (slot.rt_error_counter.buffer == VK_NULL_HANDLE) continue;
        if (!matter::map_buffer(slot.rt_error_counter, error)) return false;
        auto* words = static_cast<uint32_t*>(slot.rt_error_counter.mapped);
        for (size_t i = 0; i < sizeof(GpuRtCounters) / sizeof(uint32_t); ++i)
            words[i] = value;
        if (!matter::flush_buffer(slot.rt_error_counter, 0,
                                  sizeof(GpuRtCounters), error)) return false;
    }
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
