// Included in vk_sparse_voxel.cpp after Allocation is complete.
std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::with_shared_placements(
    matter::VulkanDevice& vk,const std::vector<std::vector<SparseVoxelInstance>>& objects,std::string& error) const {
    error.clear();
    if(!allocation_ || !allocation_->shared_shadows || objects.size()!=allocation_->shadow_bounds.size()) {
        error="shared placement update requires the same assembly catalog";return {};
    }
    const auto& old=*allocation_;auto a=std::make_shared<Allocation>(vk);
    a->shared_geometry=old.shared_geometry?old.shared_geometry:allocation_;
    const auto& base=*a->shared_geometry;
    a->set_layout=base.set_layout;a->layout=base.layout;a->select_layout=base.select_layout;
    a->pipeline=base.pipeline;a->select_pipeline=base.select_pipeline;a->shadow_sampler=base.shadow_sampler;
    a->surface_queries=base.surface_queries;a->shadow_only=base.shadow_only;a->shared_shadows=true;
    a->shared_part_materials=base.shared_part_materials;
    for(const auto& source:base.shadow_bounds) {
        matter::VkAccelerationStructureResource bound;
        bound.device=source.device;bound.handle=source.handle;bound.address=source.address;
        bound.size=source.size;bound.lifetime=source.lifetime;
        a->shadow_bounds.push_back(std::move(bound));
    }
    // Explicit read-only aliases retain the allocation; VkBufferResource stays
    // move-only so general callers cannot accidentally share mutable storage.
    const auto share=[](const matter::VkBufferResource& source) {
        matter::VkBufferResource result;
        result.device=source.device;result.buffer=source.buffer;result.memory=source.memory;
        result.size=source.size;result.address=source.address;result.mapped=source.mapped;
        result.allocation_size=source.allocation_size;result.non_coherent_atom_size=source.non_coherent_atom_size;
        result.memory_properties=source.memory_properties;result.lifetime=source.lifetime;
        return result;
    };
    a->shadow_object_addresses=share(base.shadow_object_addresses);
    a->bricks=share(base.bricks);a->cells=share(base.cells);a->instances=share(base.instances);a->roots=share(base.roots);a->levels=share(base.levels);
    a->surfaces=share(base.surfaces);a->texture_mips=share(base.texture_mips);a->texels=share(base.texels);a->texture_pages=share(base.texture_pages);
    a->children=share(base.children);a->dummy=share(base.dummy);a->command_template=share(base.command_template);
    a->timestamp_bits=base.timestamp_bits;a->timestamp_period=base.timestamp_period;
    a->bytes=base.bytes;a->draws=base.draws;a->brick_count=base.brick_count;
    uint64_t count=0;for(const auto& object:objects) count+=object.size();
    if(!count || count>0x1000000u) {error="shared placement count must be in 1..2^24";return {};}
    std::vector<VkAccelerationStructureInstanceKHR> roots;roots.reserve(size_t(count));
    std::vector<GpuInstance> placements;placements.reserve(size_t(count));
    for(size_t object=0;object<objects.size();++object) for(const auto& instance:objects[object]) {
        const auto& pose=instance.object_to_world;matter::Mat4f inverse;
        if(pose.m[12]!=0 || pose.m[13]!=0 || pose.m[14]!=0 || pose.m[15]!=1 || !mat4_inverse(pose,inverse) ||
           !std::isfinite(instance.roughness) || instance.roughness<0 || instance.roughness>1 || instance.material_index>=0x80000000u) {
            error="invalid shared placement transform or material";return {};
        }
        for(float v:pose.m) if(!std::isfinite(v)) {error="nonfinite shared placement";return {};}
        for(float v:inverse.m) if(!std::isfinite(v)) {error="nonfinite shared placement inverse";return {};}
        VkAccelerationStructureInstanceKHR root{};std::memcpy(root.transform.matrix,pose.m,12*sizeof(float));
        root.instanceCustomIndex=uint32_t(object);root.mask=0xff;root.accelerationStructureReference=a->shadow_bounds[object].address;
        roots.push_back(root);
        GpuInstance gpu{};gpu.grid_to_world=pack_glsl_mat4(pose);gpu.world_to_grid=pack_glsl_mat4(inverse);
        gpu.identity[1]=a->shared_part_materials[object]?1u:0u;gpu.identity[2]=instance.material_index;gpu.identity[3]=instance.instance_token;
        gpu.surface[0]=instance.roughness;placements.push_back(gpu);
    }
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    if(count>limits.maxInstanceCount || placements.size()*sizeof(GpuInstance)>properties.properties.limits.maxStorageBufferRange) {
        error="shared placement update exceeds device addressing limits";return {};
    }
    matter::VkBufferResource input;
    if(!matter::create_buffer(vk,roots.size()*sizeof(roots[0]),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,input,error) ||
       !matter::upload_buffer(vk,input,roots.data(),roots.size()*sizeof(roots[0]),0,error)) return {};
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=input.address;
    if(!a->build_acceleration(vk,geometry,uint32_t(count),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,a->shadow_tlas,{input.lifetime},error)) return {};
    a->root_count=uint32_t(count);
    const VkDevice device=vk.device();
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,(a->shadow_only?4u:16u)*3},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,3},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=3;pool.poolSizeCount=a->shadow_only?4u:2u;pool.pPoolSizes=sizes;
    if(!checked(vkCreateDescriptorPool(device,&pool,nullptr,&a->pool),"shared placement descriptor pool",error)) return {};
    if(!a->shadow_only) {
        const auto bytes=placements.size()*sizeof(placements[0]);
        if(!matter::create_buffer(vk,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,a->placements,error) || !matter::upload_buffer(vk,a->placements,placements.data(),bytes,0,error)) return {};
        a->bytes+=bytes;
    }
    for(auto& frame:a->frames) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=a->pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&a->set_layout;
        if(!checked(vkAllocateDescriptorSets(device,&allocate,&frame.set),"shared placement descriptor set",error)) return {};
        if(a->timestamp_bits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=a->shadow_only?2u:4u;
            if(!checked(vkCreateQueryPool(device,&query,nullptr,&frame.timestamps),"shared placement timestamps",error)) return {};
        }
        if(a->shadow_only) continue; // record_shadows binds its frame targets.
        if(!matter::create_buffer(vk,sizeof(GpuTemporal),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,frame.temporal,error)) return {};
        a->bytes+=sizeof(GpuTemporal);
        const matter::VkBufferResource* buffers[]={&a->bricks,&a->cells,&a->instances,&a->dummy,&a->roots,&a->dummy,
            &a->levels,&a->surfaces,&a->texture_mips,&a->texels,&a->texture_pages,&a->dummy,&a->children,&a->placements,&frame.temporal};
        for(uint32_t i=0;i<sparse_bindings;++i) {
            VkDescriptorBufferInfo info{buffers[i]->buffer,0,VK_WHOLE_SIZE};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet=frame.set;write.dstBinding=i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&info;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
        VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&a->shadow_tlas.handle;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=frame.set;write.dstBinding=15;
        write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;write.pNext=&acceleration;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        VkDescriptorBufferInfo info{a->shadow_object_addresses.buffer,0,VK_WHOLE_SIZE};write.dstBinding=16;
        write.pNext=nullptr;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    auto result=std::make_shared<VkSparseVoxelScene>();result->allocation_=std::move(a);return result;
}
