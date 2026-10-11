void VkSceneRenderer::update_local_light_descriptor(FrameResources& frame) {
    const matter::VkBufferResource* buffers[] = {
        &frame.local_light_records,
        &frame.local_light_cells,
        &frame.local_light_indices,
        &frame.local_light_oversized_indices,
        &frame.local_light_meta,
    };
    VkDescriptorBufferInfo infos[5]{};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t binding = 0; binding < 5; ++binding) {
        infos[binding] = {buffers[binding]->buffer, 0, buffers[binding]->size};
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = frame.local_light_descriptor_set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &infos[binding];
    }
    update_descriptor_sets(vulkan_->device(), 5, writes, 0, nullptr);
}

bool VkSceneRenderer::upload_local_lights(FrameResources& frame,
                                          std::string& error) {
    const bool publication_changed = frame.local_light_generation != local_light_generation_;
    if (!publication_changed && frame.primary_light_extent.width == raster_extent_.width &&
        frame.primary_light_extent.height == raster_extent_.height) return true;

    const auto& publication = local_light_publication_;
    const auto& index = publication.index;
    const auto checked_bytes = [&](size_t count, size_t stride,
                                   const char* label,
                                   VkDeviceSize& result) -> bool {
        if (count > std::numeric_limits<VkDeviceSize>::max() / stride) {
            error = std::string(label) + " byte size overflow";
            return false;
        }
        result = static_cast<VkDeviceSize>(count) * stride;
        if (result > limits_.max_storage_buffer_range ||
            result > limits_.max_buffer_size) {
            error = std::string(label) + " exceeds Vulkan storage-buffer limits";
            return false;
        }
        return true;
    };

    VkDeviceSize record_bytes = 0, cell_bytes = 0, compact_bytes = 0,
                 oversized_bytes = 0;
    if (!checked_bytes(publication.records.size(),
                       sizeof(world_lights::LocalLight), "local-light records",
                       record_bytes) ||
        !checked_bytes(index.cells.size(), sizeof(world_lights::LocalLightCell),
                       "local-light cells", cell_bytes) ||
        !checked_bytes(index.light_indices.size(), sizeof(uint32_t),
                       "local-light compact indices", compact_bytes) ||
        !checked_bytes(index.oversized_light_indices.size(), sizeof(uint32_t),
                       "local-light oversized indices", oversized_bytes)) {
        return false;
    }

    const world_lights::LocalLight zero_light{};
    const world_lights::LocalLightCell zero_cell{};
    const uint32_t zero_index = 0u;
    LocalLightGpuMeta meta{};
    meta.light_count = static_cast<uint32_t>(publication.records.size());
    meta.cell_bucket_count = static_cast<uint32_t>(index.cells.size());
    meta.oversized_light_count =
        static_cast<uint32_t>(index.oversized_light_indices.size());
    meta.direct_owner = static_cast<uint32_t>(LocalDirectOwner::Raster);
    meta.cell_size = index.cell_size;
    meta.inverse_cell_size = index.cell_size > 0.0f ? 1.0f / index.cell_size : 0.0f;
    meta.max_candidates_per_cell = index.stats.max_candidates_per_cell;
    meta.selection_budgets = local_light_budget_word();

    bool enable_mask = primary_light_culling_;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    enable_mask = enable_mask && !test_force_primary_light_fallback_;
#endif
    const auto mask_layout = world_lights::primary_light_mask_layout(
        enable_mask ? raster_extent_.width : 0u, raster_extent_.height,
        meta.light_count, std::min<uint64_t>(limits_.max_storage_buffer_range, 64ull * 1024 * 1024),
        limits_.max_buffer_size);
    meta.primary_tiles_x = mask_layout.tiles_x;
    meta.primary_tiles_y = mask_layout.tiles_y;
    meta.primary_words_per_tile = mask_layout.words_per_tile;
    meta.primary_culling_enabled = mask_layout.enabled ? 1u : 0u;
    matter::VkBufferResource records, cells, compact, oversized, metadata;
    const auto make_buffer = [&](matter::VkBufferResource& output,
                                 VkDeviceSize payload_bytes,
                                 VkDeviceSize empty_bytes,
                                 const void* payload,
                                 const void* empty_payload) -> bool {
        const VkDeviceSize bytes = payload_bytes == 0 ? empty_bytes
                                                       : payload_bytes;
        if (!matter::create_buffer(
                *vulkan_, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                output, error)) {
            return false;
        }
        return matter::upload_buffer(*vulkan_, output,
                                     payload_bytes == 0 ? empty_payload : payload,
                                     static_cast<size_t>(bytes), 0, error);
    };
    if (publication_changed && (!make_buffer(records, record_bytes, sizeof(zero_light),
                     publication.records.data(), &zero_light) ||
        !make_buffer(cells, cell_bytes, sizeof(zero_cell), index.cells.data(),
                     &zero_cell) ||
        !make_buffer(compact, compact_bytes, sizeof(zero_index),
                     index.light_indices.data(), &zero_index) ||
        !make_buffer(oversized, oversized_bytes, sizeof(zero_index),
                     index.oversized_light_indices.data(), &zero_index))) {
        return false;
    }

    if (!matter::create_buffer(*vulkan_, mask_layout.byte_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                (mask_layout.enabled ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                     : VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
            metadata, error) ||
        !matter::upload_buffer(*vulkan_, metadata, &meta, sizeof(meta), 0, error))
        return false;
    if (mask_layout.enabled)
        MATTER_LOGI("primary-light-cull", "tiles=%ux%u words=%u bytes=%llu memory=0x%x",
            mask_layout.tiles_x, mask_layout.tiles_y, mask_layout.words_per_tile,
            static_cast<unsigned long long>(mask_layout.byte_size),
            static_cast<unsigned>(metadata.memory_properties));
    // Commit all five resources together. Until this point the frame slot and
    // its descriptor still name the previous complete publication.
    if (publication_changed) {
        frame.local_light_records = std::move(records);
        frame.local_light_cells = std::move(cells);
        frame.local_light_indices = std::move(compact);
        frame.local_light_oversized_indices = std::move(oversized);
    }
    frame.local_light_header = meta;
    frame.primary_light_extent = raster_extent_;
    frame.local_light_meta = std::move(metadata);
    frame.local_light_generation = local_light_generation_;
    frame.local_direct_owner = LocalDirectOwner::Raster;
    frame.local_direct_lane_revision = 0;
    update_local_light_descriptor(frame);
    return true;
}

bool VkSceneRenderer::record_primary_light_cull(
    const matter::VulkanFrame& frame, const FrameMatrices& matrices,
    std::string& error) {
    if (!primary_light_culling_) return true;
    if (frame.frame_slot >= frames_.size()) {
        error = "primary light cull frame slot out of range";
        return false;
    }
    auto& selected = frames_[frame.frame_slot];
    const auto& header = selected.local_light_header;
    if (!header.primary_culling_enabled) return true;
    const VkDevice device = vulkan_->device();
    if (!primary_light_cull_pipeline_) {
        // Commit the complete pipeline atomically: a failed creation can be
        // retried without overwriting a live layout or leaking intermediate handles.
        struct PipelineCandidate {
            VkDevice device;
            VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkShaderModule module = VK_NULL_HANDLE;
            ~PipelineCandidate() {
                if (module) vkDestroyShaderModule(device, module, nullptr);
                if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
                if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
                if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            }
        } candidate{device};
        // A private compute layout keeps all graphics/RT set layouts identical
        // to the reference path (even unused RT bindings affect driver cost).
        VkDescriptorSetLayoutBinding bindings[] = {
            descriptor_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
            descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
            descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
            descriptor_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)};
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_info.bindingCount = 4;
        set_info.pBindings = bindings;
        VkResult result = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &candidate.set_layout);
        if (result != VK_SUCCESS) return fail_vk("primary light cull set layout", result, error);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &candidate.set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        result = vkCreatePipelineLayout(device, &layout_info, nullptr, &candidate.layout);
        if (result != VK_SUCCESS) return fail_vk("primary light cull pipeline layout", result, error);
        if (!create_shader_module(device, "primary_light_cull.comp.spv", candidate.module, error)) return false;
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = candidate.module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = candidate.layout;
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &candidate.pipeline);
        if (result != VK_SUCCESS) return fail_vk("primary light cull pipeline", result, error);
        primary_light_cull_set_layout_ = candidate.set_layout;
        primary_light_cull_layout_ = candidate.layout;
        primary_light_cull_pipeline_ = candidate.pipeline;
        candidate.set_layout = VK_NULL_HANDLE;
        candidate.layout = VK_NULL_HANDLE;
        candidate.pipeline = VK_NULL_HANDLE;
    }
    if (!selected.primary_light_cull_pool) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkResult result = vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
        if (result != VK_SUCCESS) return fail_vk("primary light cull descriptor pool", result, error);
        auto pool_lifetime = std::shared_ptr<void>(new VkDescriptorPool(pool),
            [device](void* pointer) {
                auto* value = static_cast<VkDescriptorPool*>(pointer);
                vkDestroyDescriptorPool(device, *value, nullptr);
                delete value;
            });
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &primary_light_cull_set_layout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        result = vkAllocateDescriptorSets(device, &allocation, &set);
        if (result != VK_SUCCESS) return fail_vk("primary light cull descriptor set", result, error);
        selected.primary_light_cull_pool = std::move(pool_lifetime);
        selected.primary_light_cull_set = set;
    }
    VkDescriptorImageInfo depth_info{composite_sampler_, depth_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo buffer_infos[] = {
        {selected.local_light_records.buffer, 0, selected.local_light_records.size},
        {selected.local_light_meta.buffer, 0, selected.local_light_meta.size},
        {selected.local_light_oversized_indices.buffer, 0, selected.local_light_oversized_indices.size}};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = selected.primary_light_cull_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 0) writes[i].pImageInfo = &depth_info;
        else writes[i].pBufferInfo = &buffer_infos[i - 1];
    }
    update_descriptor_sets(device, 4, writes, 0, nullptr);
    // The slot owns this persistent descriptor pool until frame-resource
    // replacement/renderer teardown, both of which wait for device idle. Do
    // not also place its raw-device deleter in VulkanFrame::retained: those
    // references can survive vkDestroyDevice (unlike registered buffers).
    if (!vulkan_->retain_for_frame(frame,
            {depth_.lifetime, selected.local_light_records.lifetime,
             selected.local_light_meta.lifetime,
             selected.local_light_oversized_indices.lifetime}, error)) return false;
    matter::record_image_transition(frame.command_buffer, depth_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    memory.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    struct Push { GpuMat4 clip_to_world; uint32_t width, height, count, words; } push{};
    static_assert(sizeof(Push) == 80);
    push.clip_to_world = pack_glsl_mat4(matrices.clip_to_world);
    push.width = raster_extent_.width;
    push.height = raster_extent_.height;
    push.count = header.light_count;
    push.words = header.primary_words_per_tile;
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, primary_light_cull_pipeline_);
    vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        primary_light_cull_layout_, 0, 1, &selected.primary_light_cull_set, 0, nullptr);
    vkCmdPushConstants(frame.command_buffer, primary_light_cull_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    write_gpu_timestamp(frame.command_buffer, kGpuZonePrimaryLightCull, false, selected);
    vkCmdDispatch(frame.command_buffer, header.primary_tiles_x, header.primary_tiles_y, 1);
    write_gpu_timestamp(frame.command_buffer, kGpuZonePrimaryLightCull, true, selected);
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    if (vulkan_->ray_tracing_available()) memory.dstStageMask |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    memory.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
    return true;
}

bool VkSceneRenderer::readback_primary_light_mask(
    uint32_t frame_slot, uint32_t tile_x, uint32_t tile_y,
    LocalLightGpuMeta& header, std::vector<uint32_t>& words, std::string& error) {
    words.clear();
    header = {};
    if (frame_slot >= frames_.size()) {
        error = "primary light mask readback frame slot out of range";
        return false;
    }
    auto& buffer = frames_[frame_slot].local_light_meta;
    if (!matter::map_buffer(buffer, error) ||
        !matter::invalidate_buffer(buffer, 0, buffer.size, error)) return false;
    std::memcpy(&header, buffer.mapped, sizeof(header));
    if (!header.primary_culling_enabled) return true;
    if (tile_x >= header.primary_tiles_x || tile_y >= header.primary_tiles_y) {
        error = "primary light mask tile out of range";
        return false;
    }
    const uint64_t offset = sizeof(header) +
        (uint64_t(tile_y) * header.primary_tiles_x + tile_x) * header.primary_words_per_tile * sizeof(uint32_t);
    const uint64_t bytes = uint64_t(header.primary_words_per_tile) * sizeof(uint32_t);
    if (offset > buffer.size || bytes > buffer.size - offset) {
        error = "primary light mask header exceeds allocation";
        return false;
    }
    words.resize(header.primary_words_per_tile);
    std::memcpy(words.data(), static_cast<const uint8_t*>(buffer.mapped) + offset, static_cast<size_t>(bytes));
    return true;
}

bool VkSceneRenderer::readback_primary_light_list(
    uint32_t frame_slot, uint32_t tile_x, uint32_t tile_y,
    std::vector<uint32_t>& ids, std::string& error) {
    ids.clear();
    if (frame_slot >= frames_.size()) {
        error = "primary light list readback frame slot out of range";
        return false;
    }
    auto& buffer = frames_[frame_slot].local_light_meta;
    LocalLightGpuMeta header{};
    if (buffer.size < sizeof(header)) {
        error = "primary light list metadata header is missing";
        return false;
    }
    if (!matter::map_buffer(buffer, error) ||
        !matter::invalidate_buffer(buffer, 0, buffer.size, error)) return false;
    std::memcpy(&header, buffer.mapped, sizeof(header));
    if (!header.primary_culling_enabled) return true;
    if (tile_x >= header.primary_tiles_x || tile_y >= header.primary_tiles_y) {
        error = "primary light list tile out of range";
        return false;
    }
    const uint64_t tile_count = uint64_t(header.primary_tiles_x) * header.primary_tiles_y;
    const uint64_t word_budget = (buffer.size - sizeof(header)) / sizeof(uint32_t);
    const uint64_t stride = uint64_t(header.light_count) + 1u;
    // Divide before multiplying: corrupt headers must never wrap an offset
    // back into the allocation. Check the complete mask/list publication.
    if (!header.primary_words_per_tile ||
        tile_count > word_budget / header.primary_words_per_tile) {
        error = "primary light list mask span exceeds allocation";
        return false;
    }
    const uint64_t mask_words = tile_count * header.primary_words_per_tile;
    if (tile_count > (word_budget - mask_words) / stride) {
        error = "primary light list span exceeds allocation";
        return false;
    }
    const uint64_t tile = uint64_t(tile_y) * header.primary_tiles_x + tile_x;
    const auto* words = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(buffer.mapped) + sizeof(header));
    const uint64_t start = mask_words + tile * stride;
    const uint32_t count = words[start];
    if (count > header.light_count) {
        error = "primary light list count exceeds light count";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (words[start + 1u + i] >= header.light_count) {
            error = "primary light list contains an invalid light index";
            return false;
        }
    }
    ids.assign(words + start + 1u, words + start + 1u + count);
    return true;
}

bool VkSceneRenderer::publish_local_direct_owner(FrameResources& frame,
                                                 LocalDirectOwner owner,
                                                 uint64_t lane_revision,
                                                 std::string& error) {
    if (owner == LocalDirectOwner::RayTraced &&
        lane_revision != local_light_revision_) {
        error = "RT local-direct ownership requires the current light revision";
        return false;
    }
    LocalLightGpuMeta meta{};
    meta.light_count =
        static_cast<uint32_t>(local_light_publication_.records.size());
    meta.cell_bucket_count =
        static_cast<uint32_t>(local_light_publication_.index.cells.size());
    meta.oversized_light_count = static_cast<uint32_t>(
        local_light_publication_.index.oversized_light_indices.size());
    meta.direct_owner = static_cast<uint32_t>(owner);
    meta.cell_size = local_light_publication_.index.cell_size;
    meta.inverse_cell_size = meta.cell_size > 0.0f ? 1.0f / meta.cell_size : 0.0f;
    meta.max_candidates_per_cell =
        local_light_publication_.index.stats.max_candidates_per_cell;
    meta.selection_budgets = local_light_budget_word();
    if (!matter::upload_buffer(*vulkan_, frame.local_light_meta, &meta,
                               offsetof(LocalLightGpuMeta, primary_tiles_x), 0, error)) {
        return false;
    }
    frame.local_direct_owner = owner;
    frame.local_direct_lane_revision = lane_revision;
    if (owner == LocalDirectOwner::Raster) {
        local_direct_owner_ = owner;
        local_direct_lane_revision_ = lane_revision;
    }
    return true;
}

bool VkSceneRenderer::update_local_lights(
    const world_lights::LocalLightPublication& publication,
    std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (publication.revision == 0u) {
        error = "local-light publication revision must be nonzero";
        return false;
    }
    if (publication.revision == local_light_revision_)
        return true;

    try {
        // Rebuild from records/config so malformed offsets, counts or hashes
        // never reach a shader. The revision comparison proves the provider
        // and renderer used the same canonical content.
        world_lights::LocalLightPublication candidate;
        candidate.records = publication.records;
        world_lights::LocalLightIndexConfig config;
        config.cell_size = publication.index.cell_size;
        config.max_cells_per_light = publication.index.max_cells_per_light;
        if (!world_lights::rebuild_local_light_publication(candidate, config,
                                                            error)) {
            error = "local-light publication rejected: " + error;
            return false;
        }
        if (candidate.revision != publication.revision) {
            error = "local-light publication revision does not match its records/index config";
            return false;
        }
        local_light_publication_ = std::move(candidate);
    } catch (const std::bad_alloc&) {
        error = "local-light renderer publication allocation failed";
        return false;
    }

    local_light_revision_ = publication.revision;
    local_direct_owner_ = LocalDirectOwner::Raster;
    local_direct_lane_revision_ = 0;
    ++local_light_generation_;
    if (local_light_generation_ == 0u) local_light_generation_ = 1u;

    // Local-light edits affect primary direct, RT bounce inputs and the final
    // presentation. Raster remains the sole direct owner until a future RT
    // lane explicitly proves itself ready for this same revision.
    gi_history_reset_pending_ = true;
    local_direct_history_state_.reset_pending = true;
    gi_diffuse_history_reset_pending_ = true;
    gi_reflection_history_reset_pending_ = true;
    dlss_history_reset_pending_ = true;
    temporal_history_changed_ = true;
    const LocalLightRenderStats stats = local_light_stats();
    MATTER_LOGI("local-light",
                "published %u lights, %u cells/%u buckets, %llu index bytes, max %u candidates\n",
                stats.light_count, stats.occupied_cell_count,
                stats.bucket_count,
                static_cast<unsigned long long>(stats.index_upload_bytes),
                stats.max_candidates_per_cell);
    return true;
}

LocalLightRenderStats VkSceneRenderer::local_light_stats() const noexcept {
    LocalLightRenderStats result{};
    const auto& publication = local_light_publication_;
    const auto& stats = publication.index.stats;
    result.light_count = static_cast<uint32_t>(publication.records.size());
    result.occupied_cell_count = stats.occupied_cell_count;
    result.bucket_count = stats.bucket_count;
    result.oversized_light_count = stats.oversized_light_count;
    result.max_candidates_per_cell = stats.max_candidates_per_cell;
    result.list_entry_count = stats.list_entry_count;
    result.light_upload_bytes =
        publication.records.size() * sizeof(world_lights::LocalLight);
    result.index_upload_bytes = stats.gpu_index_bytes + sizeof(LocalLightGpuMeta);
    result.publication_generation = local_light_generation_;
    return result;
}
