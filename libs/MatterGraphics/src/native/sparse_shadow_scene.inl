bool VkSceneRenderer::set_sparse_shadow_casters(const std::vector<SparseVoxelBatch>& batches,std::string& error) {
    error.clear();if(fail_if_poisoned(error)) return false;
    std::shared_ptr<VkSparseVoxelScene> candidate;
    if(!batches.empty()) {candidate=VkSparseVoxelScene::create_shadow_casters(*vulkan_,batches,error);if(!candidate) return false;}
    if(candidate==sparse_shadows_) return true;
    sparse_shadows_=std::move(candidate);
    note_sparse_snapshot_changed();
    shared_forest_catalog_.clear();shared_forest_shadow_catalog_.clear();
    return true;
}

bool VkSceneRenderer::readback_sparse_shadow_ms(uint32_t slot,double& ms,std::string& error) const {
    if(!sparse_shadows_) {ms=0;error="no sparse shadow snapshot";return false;}
    return sparse_shadows_->readback_shadow_ms(*vulkan_,slot,ms,error);
}

bool VkSceneRenderer::set_shared_sparse_shadow_casters(const std::vector<SparseShadowObject>& objects,std::string& error) {
    error.clear();if(fail_if_poisoned(error)) return false;
    std::shared_ptr<VkSparseVoxelScene> candidate;
    if(!objects.empty()) {candidate=VkSparseVoxelScene::create_shared_shadow_casters(*vulkan_,objects,error);if(!candidate) return false;}
    if(candidate==sparse_shadows_) return true;
    sparse_shadows_=std::move(candidate);note_sparse_snapshot_changed();
    shared_forest_catalog_.clear();shared_forest_shadow_catalog_.clear();
    return true;
}
