void VkSceneRenderer::update_water_forward_descriptor(FrameResources& frame) {
    if (frame.water_forward_descriptor_set == VK_NULL_HANDLE ||
        frame.water_forward_constants.buffer == VK_NULL_HANDLE ||
        water_forward_sampler_ == VK_NULL_HANDLE ||
        opaque_hdr_.view == VK_NULL_HANDLE ||
        opaque_depth_.view == VK_NULL_HANDLE)
        return;
    const VkDescriptorImageInfo opaque_hdr_info{
        water_forward_sampler_, opaque_hdr_.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo opaque_depth_info{
        water_forward_sampler_, opaque_depth_.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo constants_info{
        frame.water_forward_constants.buffer, 0,
        sizeof(WaterForwardConstants)};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t index = 0; index != 3; ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = frame.water_forward_descriptor_set;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &opaque_hdr_info;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &opaque_depth_info;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &constants_info;
    update_descriptor_sets(vulkan_->device(), 3, writes, 0, nullptr);
}

bool VkSceneRenderer::upload_water_forward_constants(
    FrameResources& frame, const FrameMatrices& matrices, VkExtent2D extent,
    std::string& error) {
    WaterForwardConstants constants{};
    constants.clip_to_world = pack_glsl_mat4(matrices.clip_to_world);
    constants.to_sun = {atmosphere_replay_constants_.rt_to_sun.x,
                        atmosphere_replay_constants_.rt_to_sun.y,
                        atmosphere_replay_constants_.rt_to_sun.z, 0.0f};
    constants.viewport_refraction = {static_cast<float>(extent.width),
                                     static_cast<float>(extent.height),
                                     24.0f, 0.75f};
    constants.reflection_controls = {24.0f, 2.0f, 0.25f, 80.0f};
    constants.diagnostics[0] =
        static_cast<std::uint32_t>(water_diagnostic_view_);
    frame.water_forward_constants_cache = constants;
    return matter::upload_buffer(
        *vulkan_, frame.water_forward_constants, &constants,
        sizeof(constants), 0u, error);
}

bool VkSceneRenderer::write_water_field_descriptors_for_frame(
    FrameResources& frame, VkDescriptorSet rt_set, std::string& error) {
    const auto records = water_fields_.gpu_records();
    std::uint32_t generations[kWaterFieldBindingSlots]{};
    bool records_changed = !frame.water_field_raster_descriptors_valid;
    for (std::uint32_t slot = 0u; slot != kWaterFieldBindingSlots; ++slot) {
        generations[slot] = records[slot].extent_generation[2];
        records_changed = records_changed ||
            frame.water_field_generations[slot] != generations[slot];
    }
    if (records_changed &&
        !matter::upload_buffer(
            *vulkan_, frame.water_field_records, records.data(),
            sizeof(records), 0u, error))
        return false;

    std::array<std::array<VkDescriptorImageInfo,
                          kWaterFieldBindingSlots>, 4>
        image_infos{};
    bool views_changed = false;
    for (std::uint32_t channel = 0u; channel != image_infos.size();
         ++channel) {
        for (std::uint32_t slot = 0u; slot != kWaterFieldBindingSlots;
             ++slot) {
            VkDescriptorImageInfo& info = image_infos[channel][slot];
            info.sampler = water_field_resources_.sampler(channel);
            info.imageView =
                water_field_resources_.descriptor_view(slot, channel);
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            views_changed = views_changed ||
                frame.water_field_views[channel][slot] != info.imageView;
            frame.water_field_views[channel][slot] = info.imageView;
        }
    }
    const bool write_raster =
        !frame.water_field_raster_descriptors_valid || views_changed;
    const bool write_rt = rt_set != VK_NULL_HANDLE &&
        (!frame.water_field_rt_descriptors_valid || views_changed);
    if (!write_raster && !write_rt) {
        std::copy(std::begin(generations), std::end(generations),
                  std::begin(frame.water_field_generations));
        return true;
    }
    const VkDescriptorBufferInfo buffer_info{
        frame.water_field_records.buffer, 0u,
        sizeof(WaterFieldGpuRecord) * kWaterFieldBindingSlots};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::uint32_t channel = 0u; channel != 4u; ++channel) {
        VkWriteDescriptorSet& write = writes[channel];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frame.descriptor_sets[1];
        write.dstBinding = 20u + channel;
        write.descriptorCount = kWaterFieldBindingSlots;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = image_infos[channel].data();
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = frame.descriptor_sets[1];
    writes[4].dstBinding = 24u;
    writes[4].descriptorCount = 1u;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &buffer_info;
    if (write_raster) {
        update_descriptor_sets(vulkan_->device(),
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0u, nullptr);
        frame.water_field_raster_descriptors_valid = true;
    }

    if (write_rt) {
        for (std::uint32_t channel = 0u; channel != 4u; ++channel) {
            writes[channel].dstSet = rt_set;
            writes[channel].dstBinding = 21u + channel;
        }
        writes[4].dstSet = rt_set;
        writes[4].dstBinding = 25u;
        update_descriptor_sets(vulkan_->device(),
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0u, nullptr);
        frame.water_field_rt_descriptors_valid = true;
    }
    std::copy(std::begin(generations), std::end(generations),
              std::begin(frame.water_field_generations));
    return true;
}
