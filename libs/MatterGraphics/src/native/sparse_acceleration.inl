bool VkSparseVoxelScene::Allocation::build_shared_roots(matter::VulkanDevice& vk,
    const std::vector<SparseShadowObject>& objects,std::string& error) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    uint64_t total_roots=0;for(const auto& object:objects) total_roots+=object.instances.size();
    if(total_roots>limits.maxInstanceCount) {error="shared shadow root count exceeds device limit";return false;}
    const auto input_buffer=[&](const auto& data,matter::VkBufferResource& buffer,VkBufferUsageFlags usage) {
        const auto size=data.size()*sizeof(data[0]);
        return matter::create_buffer(vk,size,usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffer,error) &&
            matter::upload_buffer(vk,buffer,data.data(),size,0,error);
    };
    constexpr VkBufferUsageFlags input_usage=VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    std::vector<VkAccelerationStructureInstanceKHR> forest;forest.reserve(size_t(total_roots));
    std::vector<std::array<uint32_t,4>> addresses;
    uint32_t level=0,part_offset=0;
    shadow_bounds.resize(objects.size());shadow_objects.resize(objects.size());
    shared_part_materials.clear();for(const auto& object:objects) shared_part_materials.push_back(object.use_part_materials);
    for(size_t index=0;index<objects.size();++index) {
        const auto& object=objects[index];
        std::vector<VkAccelerationStructureInstanceKHR> branches;
        double minimum[3]={1e30,1e30,1e30},maximum[3]={-1e30,-1e30,-1e30};
        std::vector<std::shared_ptr<void>> retained;
        for(const auto& part:object.parts) {
            double lo[3]={1e30,1e30,1e30},hi[3]={-1e30,-1e30,-1e30};
            auto grid=mat4_identity();
            if(part.surface) {
                for(const auto& triangle:part.surface->triangles) for(const auto& vertex:triangle.vertices) {
                    const float p[]={vertex.position.x,vertex.position.y,vertex.position.z};
                    for(int k=0;k<3;++k) {lo[k]=std::min(lo[k],double(p[k]));hi[k]=std::max(hi[k],double(p[k]));}
                }
            } else {
            const auto& asset=*part.asset;
            const double origin[]={asset.origin.x,asset.origin.y,asset.origin.z};
            for(const auto& brick:asset.bricks) for(uint32_t bit=0;bit<64;++bit) if(brick.mask&(uint64_t(1)<<bit)) {
                const int local[]={int(bit%4),int(bit/4%4),int(bit/16)};
                for(int k=0;k<3;++k) {
                    const double value=origin[k]+(int64_t(brick.coord[k])*4+local[k])*double(asset.cell_size);
                    lo[k]=std::min(lo[k],value);hi[k]=std::max(hi[k],value+asset.cell_size);
                }
            }
            grid.m[0]=grid.m[5]=grid.m[10]=asset.cell_size;
            grid.m[3]=asset.origin.x;grid.m[7]=asset.origin.y;grid.m[11]=asset.origin.z;
            }
            for(const auto& placement:part.instances) {
                const auto transform=mat4_mul(placement.object_to_world,grid);
                VkAccelerationStructureInstanceKHR branch{};
                std::memcpy(branch.transform.matrix,transform.m,12*sizeof(float));
                branch.instanceCustomIndex=level;branch.mask=0xff;branch.accelerationStructureReference=shadow_blas[level].address;
                branches.push_back(branch);
                for(int corner=0;corner<8;++corner) for(int k=0;k<3;++k) {
                    double value=placement.object_to_world.m[k*4+3];
                    for(int a=0;a<3;++a) value+=placement.object_to_world.m[k*4+a]*((corner&(1<<a))?hi[a]:lo[a]);
                    minimum[k]=std::min(minimum[k],value);maximum[k]=std::max(maximum[k],value);
                }
            }
            retained.push_back(shadow_blas[level].lifetime);++level;
        }
        if(branches.size()>limits.maxInstanceCount) {error="shared shadow assembly exceeds device instance limit";return false;}
        if(objects.size()==1) shadow_objects[index]=std::move(shadow_tlas); // Reuse the already built local assembly.
        else {
            matter::VkBufferResource input;if(!input_buffer(branches,input,input_usage)) return false;
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
            geometry.geometry.instances.data.deviceAddress=input.address;retained.push_back(input.lifetime);
            if(!build_acceleration(vk,geometry,uint32_t(branches.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                shadow_objects[index],std::move(retained),error)) return false;
        }
        const uint64_t address=shadow_objects[index].address;
        addresses.push_back({uint32_t(address),uint32_t(address>>32),part_offset,0});
        part_offset+=uint32_t(branches.size());
        // Expand rounding conservatively so the outer acceleration structure
        // cannot hide a valid inner hit at a transformed asset boundary.
        float low[3]{},high[3]{};
        for(int k=0;k<3;++k) {
            const double pad=16*std::numeric_limits<float>::epsilon()*std::max({1.0,std::abs(minimum[k]),std::abs(maximum[k])});
            low[k]=float(minimum[k]-pad);high[k]=float(maximum[k]+pad);
            if(!std::isfinite(low[k]) || !std::isfinite(high[k]) || !(high[k]>low[k])) {error="shared shadow object bounds overflow";return false;}
        }
        const VkAabbPositionsKHR box{low[0],low[1],low[2],high[0],high[1],high[2]};
        matter::VkBufferResource bounds;if(!input_buffer(std::vector<VkAabbPositionsKHR>{box},bounds,input_usage)) return false;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_AABBS_KHR;
        geometry.geometry.aabbs={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
        geometry.geometry.aabbs.data.deviceAddress=bounds.address;geometry.geometry.aabbs.stride=sizeof(box);
        if(!build_acceleration(vk,geometry,1,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,shadow_bounds[index],{bounds.lifetime},error)) return false;
        for(const auto& placement:object.instances) {
            VkAccelerationStructureInstanceKHR root{};
            std::memcpy(root.transform.matrix,placement.object_to_world.m,12*sizeof(float));
            root.instanceCustomIndex=uint32_t(index);root.mask=0xff;root.accelerationStructureReference=shadow_bounds[index].address;
            forest.push_back(root);
        }
    }
    matter::VkBufferResource input;if(!input_buffer(forest,input,input_usage)) return false;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=input.address;
    std::vector<std::shared_ptr<void>> retained{input.lifetime};for(const auto& bound:shadow_bounds) retained.push_back(bound.lifetime);
    matter::VkAccelerationStructureResource outer;
    if(!build_acceleration(vk,geometry,uint32_t(forest.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,outer,std::move(retained),error)) return false;
    if(objects.size()>1) bytes-=shadow_tlas.size; // Combined setup TLAS is no longer needed.
    shadow_tlas=std::move(outer);
    bytes-=shadow_object_addresses.size;
    if(!input_buffer(addresses,shadow_object_addresses,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
    bytes+=shadow_object_addresses.size;shared_shadows=true;root_count=uint32_t(forest.size());
    return true;
}

bool VkSparseVoxelScene::Allocation::build_acceleration(matter::VulkanDevice& vk,
    VkAccelerationStructureGeometryKHR& geometry,uint32_t count,VkAccelerationStructureTypeKHR type,
    matter::VkAccelerationStructureResource& target,std::vector<std::shared_ptr<void>> retained,std::string& error) {
    const VkDevice device=vk.device();
    auto get_sizes=reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device,"vkGetAccelerationStructureBuildSizesKHR"));
    auto cmd_build=reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device,"vkCmdBuildAccelerationStructuresKHR"));
    if(!get_sizes || !cmd_build) {error="sparse shadows require acceleration structure build entry points";return false;}
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    const VkDeviceSize align=std::max(1u,limits.minAccelerationStructureScratchOffsetAlignment);
    struct Record {
        PFN_vkCmdBuildAccelerationStructuresKHR command;
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        VkAccelerationStructureBuildRangeInfoKHR range{};
    } record{cmd_build};
    record.info.type=type;record.info.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    record.info.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    record.info.geometryCount=1;record.info.pGeometries=&geometry;record.range.primitiveCount=count;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&record.info,&count,&sizes);
    matter::VkBufferResource scratch;
    if(!matter::create_acceleration_structure(vk,type,sizes.accelerationStructureSize,target,error) ||
       !matter::create_buffer(vk,sizes.buildScratchSize+align-1,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,scratch,error)) return false;
    record.info.dstAccelerationStructure=target.handle;
    record.info.scratchData.deviceAddress=(scratch.address+align-1)/align*align;
    retained.push_back(target.lifetime);retained.push_back(scratch.lifetime);
    if(!matter::submit_immediate(vk,[](VkCommandBuffer cmd,void* opaque) {
        auto& r=*static_cast<Record*>(opaque);
        VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        before.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT|VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT|VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        before.dstStageMask=VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.dstAccessMask=VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&before;
        vkCmdPipelineBarrier2(cmd,&dependency);
        const auto* range=&r.range;r.command(cmd,1,&r.info,&range);
        before.srcStageMask=VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.srcAccessMask=VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        before.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        vkCmdPipelineBarrier2(cmd,&dependency);
    },&record,error,matter::ImmediateSubmitPhase::compute_dispatch,std::move(retained))) return false;
    bytes+=target.size;return true;
}

bool VkSparseVoxelScene::Allocation::build_surface_queries(matter::VulkanDevice& vk,
    const std::vector<GpuSurfaceTriangle>& triangle_data,const std::vector<GpuInstance>& instance_data,
    const std::vector<GpuLevel>& level_data,std::string& error) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    if(level_data.size()>0x1000000u || instance_data.size()>limits.maxInstanceCount) {
        error="shared surface assembly exceeds ray-query addressing limits";return false;
    }
    std::vector<std::array<float,3>> positions;positions.reserve(triangle_data.size()*3);
    for(const auto& triangle:triangle_data) for(const auto& v:triangle.vertices)
        positions.push_back({v.position_uvx[0],v.position_uvx[1],v.position_uvx[2]});
    const auto input_buffer=[&](const auto& data,matter::VkBufferResource& target) {
        const auto size=data.size()*sizeof(data[0]);
        return matter::create_buffer(vk,size,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,target,error) && matter::upload_buffer(vk,target,data.data(),size,0,error);
    };
    matter::VkBufferResource vertices,placed;if(!input_buffer(positions,vertices)) return false;
    shadow_blas.resize(level_data.size());
    for(size_t i=0;i<level_data.size();++i) {
        const auto& level=level_data[i];
        if(level.flags[0]!=1 || !level.brick_count || level.brick_count>limits.maxPrimitiveCount) {
            error="unsupported shared surface primitive range";return false;
        }
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        bool opaque=true;for(uint32_t p=0;p<level.brick_count;++p)
            opaque&=triangle_data[level.first_brick+p].vertices[0].color_texture[3]<0;
        geometry.flags=opaque?VK_GEOMETRY_OPAQUE_BIT_KHR:0;
        auto& tri=geometry.geometry.triangles;tri={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        tri.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;tri.vertexStride=sizeof(positions[0]);
        tri.vertexData.deviceAddress=vertices.address+VkDeviceSize(level.first_brick)*3*sizeof(positions[0]);
        tri.maxVertex=level.brick_count*3-1;tri.indexType=VK_INDEX_TYPE_NONE_KHR;
        if(!build_acceleration(vk,geometry,level.brick_count,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            shadow_blas[i],{vertices.lifetime},error)) return false;
    }
    std::vector<VkAccelerationStructureInstanceKHR> roots;roots.reserve(instance_data.size());
    for(const auto& source:instance_data) {
        VkAccelerationStructureInstanceKHR root{};
        for(int row=0;row<3;++row) for(int col=0;col<4;++col)
            root.transform.matrix[row][col]=source.grid_to_world.elements[col*4+row];
        root.instanceCustomIndex=source.identity[0];root.mask=0xff;
        root.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        root.accelerationStructureReference=shadow_blas[source.identity[0]].address;roots.push_back(root);
    }
    if(!input_buffer(roots,placed)) return false;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=placed.address;
    std::vector<std::shared_ptr<void>> retained{placed.lifetime};for(const auto& blas:shadow_blas) retained.push_back(blas.lifetime);
    return build_acceleration(vk,geometry,uint32_t(roots.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        shadow_tlas,std::move(retained),error);
}

bool VkSparseVoxelScene::Allocation::build_shadows(matter::VulkanDevice& vk,
    const std::vector<GpuBrick>& brick_data,const std::vector<GpuInstance>& instance_data,
    const std::vector<GpuLevel>& level_data,std::string& error) {
    const VkDevice device=vk.device();
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    if(level_data.size()>0x1000000u || instance_data.size()>limits.maxInstanceCount) {
        error="sparse shadow instance/prototype count exceeds ray-query limits";return false;
    }
    std::vector<VkAabbPositionsKHR> boxes;boxes.reserve(brick_data.size());
    for(const auto& brick:brick_data) {
        const uint32_t b=uint32_t(brick.coord[3]);
        boxes.push_back({float(brick.coord[0]*4+int(b&3)),float(brick.coord[1]*4+int((b>>2)&3)),float(brick.coord[2]*4+int((b>>4)&3)),
            float(brick.coord[0]*4+int((b>>6)&3)+1),float(brick.coord[1]*4+int((b>>8)&3)+1),float(brick.coord[2]*4+int((b>>10)&3)+1)});
    }
    const auto input_buffer=[&](const auto& data,matter::VkBufferResource& buffer) {
        const auto size=data.size()*sizeof(data[0]);
        return matter::create_buffer(vk,size,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffer,error) &&
            matter::upload_buffer(vk,buffer,data.data(),size,0,error);
    };
    matter::VkBufferResource aabbs,placed;
    if(!input_buffer(boxes,aabbs)) return false;
    shadow_blas.resize(level_data.size());
    for(size_t i=0;i<level_data.size();++i) {
        const auto& level=level_data[i];
        if(!level.brick_count || level.brick_count>limits.maxPrimitiveCount || level.flags[0]!=0) {
            error="invalid sparse shadow brick geometry count/type";return false;
        }
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_AABBS_KHR;
        geometry.geometry.aabbs={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
        geometry.geometry.aabbs.data.deviceAddress=aabbs.address+VkDeviceSize(level.first_brick)*sizeof(VkAabbPositionsKHR);
        geometry.geometry.aabbs.stride=sizeof(VkAabbPositionsKHR);
        if(!build_acceleration(vk,geometry,level.brick_count,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,shadow_blas[i],{aabbs.lifetime},error)) return false;
    }
    std::vector<VkAccelerationStructureInstanceKHR> roots;roots.reserve(instance_data.size());
    for(const auto& source:instance_data) {
        const uint32_t level=source.identity[0];VkAccelerationStructureInstanceKHR root{};
        for(int row=0;row<3;++row) for(int col=0;col<4;++col)
            root.transform.matrix[row][col]=source.grid_to_world.elements[col*4+row];
        root.instanceCustomIndex=level;root.mask=0xff;
        root.accelerationStructureReference=shadow_blas[level].address;roots.push_back(root);
    }
    if(!input_buffer(roots,placed)) return false;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=placed.address;
    std::vector<std::shared_ptr<void>> retained{placed.lifetime};
    for(const auto& blas:shadow_blas) retained.push_back(blas.lifetime);
    if(!build_acceleration(vk,geometry,uint32_t(roots.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,shadow_tlas,std::move(retained),error)) return false;

    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding bindings[7]{};
    for(uint32_t i=0;i<7;++i) bindings[i]={i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set_info.bindingCount=7;set_info.pBindings=bindings;
    if(!checked(vkCreateDescriptorSetLayout(device,&set_info,nullptr,&set_layout),"sparse shadow set layout",error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ShadowPush)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount=1;layout_info.pSetLayouts=&set_layout;layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&push;
    if(!checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&layout),"sparse shadow pipeline layout",error)) return false;
    const auto spirv=matter::find_spirv("sparse_voxel_shadow.comp.spv");
    if(!spirv.words || !spirv.word_count) {error="missing sparse voxel shadow shader";return false;}
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=spirv.word_count*4;shader.pCode=spirv.words;
    if(!checked(vkCreateShaderModule(device,&shader,nullptr,&select_shader),"sparse shadow shader",error)) return false;
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline_info.layout=layout;
    pipeline_info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module=select_shader;pipeline_info.stage.pName="main";
    if(!checked(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&select_pipeline),"sparse shadow pipeline",error)) return false;
    vkDestroyShaderModule(device,select_shader,nullptr);select_shader=VK_NULL_HANDLE;
    std::vector<std::array<uint32_t,4>> no_objects(1);
    if(!matter::create_buffer(vk,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,shadow_object_addresses,error) ||
       !matter::upload_buffer(vk,shadow_object_addresses,no_objects.data(),16,0,error)) return false;
    bytes+=16;
    VkDescriptorPoolSize sizes[]={{types[0],3},{types[1],3},{types[2],3},{types[3],12}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool_info.maxSets=3;pool_info.poolSizeCount=4;pool_info.pPoolSizes=sizes;
    if(!checked(vkCreateDescriptorPool(device,&pool_info,nullptr,&pool),"sparse shadow descriptor pool",error)) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(!checked(vkCreateSampler(device,&sampler,nullptr,&shadow_sampler),"sparse shadow depth sampler",error)) return false;
    for(auto& frame:frames) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&set_layout;
        if(!checked(vkAllocateDescriptorSets(device,&allocate,&frame.set),"sparse shadow descriptor set",error)) return false;
        if(timestamp_bits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=2;
            if(!checked(vkCreateQueryPool(device,&query,nullptr,&frame.timestamps),"sparse shadow timestamps",error)) return false;
        }
    }
    return true;
}
