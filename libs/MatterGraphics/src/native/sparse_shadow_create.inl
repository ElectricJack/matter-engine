std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_shadow_casters(
    matter::VulkanDevice& vk,const std::vector<SparseVoxelBatch>& batches,std::string& error) {
    if(!vk.ray_tracing_available()) {error=vk.ray_tracing_unavailable_reason();return {};}
    if(batches.empty()) {error="empty sparse shadow caster snapshot";return {};}
    for(const auto& batch:batches) if(!batch.asset || batch.asset->bricks.empty() || batch.instances.empty() ||
        batch.surface || !batch.coarser.empty() || !batch.coarser_surfaces.empty()) {
        error="shadow casters require one nonempty fixed voxel asset and placements per batch";return {};
    }
    return create_impl(vk,batches,error,nullptr,nullptr,nullptr,true);
}

std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_shared_shadow_casters(
    matter::VulkanDevice& vk,const std::vector<SparseShadowObject>& objects,std::string& error) {
    error.clear();
    if(!vk.ray_tracing_available()) {error=vk.ray_tracing_unavailable_reason();return {};}
    if(objects.empty() || objects.size()>0x1000000u) {error="invalid shared shadow object count";return {};}
    // Direct callers receive the same full outer-placement/catalog admission
    // as paired forest publication, before local geometry can be uploaded.
    if(!preflight_shared_forest(vk,{},objects,error)) return {};
    std::vector<SparseVoxelBatch> parts;
    uint64_t count=0;
    for(const auto& object:objects) {
        if(object.parts.empty()) {error="shared shadow objects require parts";return {};}
        count+=object.instances.size();
        if(count>0x1000000u) {error="shared shadow placement count exceeds addressing limit";return {};}
        for(const auto& instance:object.instances) {
            const auto& pose=instance.object_to_world;matter::Mat4f inverse;
            if(pose.m[12]!=0 || pose.m[13]!=0 || pose.m[14]!=0 || pose.m[15]!=1 || !mat4_inverse(pose,inverse)) {
                error="shared shadow placement requires an invertible affine transform";return {};
            }
            for(float v:pose.m) if(!std::isfinite(v)) {error="shared shadow placement transform is nonfinite";return {};}
            for(float v:inverse.m) if(!std::isfinite(v)) {error="shared shadow placement inverse is nonfinite";return {};}
        }
        parts.insert(parts.end(),object.parts.begin(),object.parts.end());
    }
    if(!count) {error="shared shadows need at least one world placement";return {};}
    auto result=create_shadow_casters(vk,parts,error);
    if(!result || !result->allocation_->build_shared_roots(vk,objects,error)) return {};
    return result;
}
