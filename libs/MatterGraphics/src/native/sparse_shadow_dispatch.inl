bool VkSparseVoxelScene::record_shadows(VkCommandBuffer cmd,const FrameMatrices& matrices,uint32_t slot,
    VkExtent2D extent,matter::Float3 to_sun,float bias,float max_distance,
    const matter::VkImageResource& depth,const matter::VkImageResource& visibility,
    const SparseVoxelTemporal& temporal,std::string& error) const {
    error.clear();
    if(!allocation_ || !allocation_->shadow_only || slot>=kSparseVoxelFrameSlots || !cmd ||
       !extent.width || !extent.height || !depth.view || !visibility.view ||
       visibility.format!=VK_FORMAT_R16G16B16A16_SFLOAT || depth.extent.width!=extent.width ||
       depth.extent.height!=extent.height || visibility.extent.width!=extent.width || visibility.extent.height!=extent.height ||
       !std::isfinite(bias) || bias<0 || !std::isfinite(max_distance) || max_distance<=0) {
        error="invalid sparse shadow targets, slot or ray interval";return false;
    }
    const double norm=std::sqrt(double(to_sun.x)*to_sun.x+double(to_sun.y)*to_sun.y+double(to_sun.z)*to_sun.z);
    if(!std::isfinite(norm) || norm<1e-12) {error="invalid sparse shadow sun direction";return false;}
    for(float value:matrices.clip_to_world.m) if(!std::isfinite(value)) {error="invalid sparse shadow camera";return false;}
    auto& a=*allocation_;const VkDevice device=a.device();
    if(!device) {error="sparse shadow device was destroyed";return false;}
    const auto& frame=a.frames[slot];
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&a.shadow_tlas.handle;
    VkDescriptorImageInfo images[]={{a.shadow_sampler,depth.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE,visibility.view,VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo buffers[]={{a.bricks.buffer,0,VK_WHOLE_SIZE},{a.cells.buffer,0,VK_WHOLE_SIZE},{a.levels.buffer,0,VK_WHOLE_SIZE},{a.shadow_object_addresses.buffer,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[7]{};
    for(uint32_t i=0;i<7;++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
        if(i==0) {writes[i].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;writes[i].pNext=&acceleration;}
        else if(i<3) {writes[i].descriptorType=i==1?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&images[i-1];}
        else {writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i-3];}
    }
    vkUpdateDescriptorSets(device,7,writes,0,nullptr);
    ShadowPush push;push.clip_to_world=pack_glsl_mat4(matrices.clip_to_world);
    push.sun_distance[0]=float(to_sun.x/norm);push.sun_distance[1]=float(to_sun.y/norm);push.sun_distance[2]=float(to_sun.z/norm);
    push.sun_distance[3]=max_distance;push.bias_extent[0]=bias;push.bias_extent[2]=a.shared_shadows?1.0f:0.0f;
    const uint32_t sequence=temporal.accumulate_samples?uint32_t(temporal.presented_frame_index)^uint32_t(temporal.presented_frame_index>>32):0;
    std::memcpy(&push.bias_extent[1],&sequence,sizeof(sequence));
    if(frame.timestamps) {vkCmdResetQueryPool(cmd,frame.timestamps,0,2);vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,frame.timestamps,0);}
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,a.select_pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&frame.set,0,nullptr);
    vkCmdPushConstants(cmd,a.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(cmd,(extent.width+7)/8,(extent.height+7)/8,1);
    if(frame.timestamps) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,frame.timestamps,1);
    return true;
}

bool VkSparseVoxelScene::readback_shadow_ms(matter::VulkanDevice& vk,uint32_t slot,double& ms,std::string& error) const {
    ms=0;error.clear();
    if(!allocation_ || !allocation_->shadow_only || slot>=kSparseVoxelFrameSlots) {error="invalid sparse shadow timestamp snapshot/slot";return false;}
    const auto& a=*allocation_;if(!a.frames[slot].timestamps) return true;
    uint64_t times[2]{};
    if(!checked(vkGetQueryPoolResults(vk.device(),a.frames[slot].timestamps,0,2,sizeof(times),times,sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"sparse shadow timestamps",error)) return false;
    const uint64_t mask=a.timestamp_bits>=64?UINT64_MAX:((uint64_t(1)<<a.timestamp_bits)-1);
    ms=double((times[1]-times[0])&mask)*a.timestamp_period*1e-6;return true;
}
