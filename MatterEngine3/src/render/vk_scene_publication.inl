// Included inside namespace viewer, after renderer implementation.
struct ScenePublication::Impl {
    VkSceneRenderer& renderer;
    bool committed=false;
    std::vector<uint64_t> added;
    struct Lods {
        uint32_t slot;
        std::vector<VkSceneRenderer::GpuCluster> clusters;
        std::vector<std::vector<float>> thresholds;
        float mesh_span=0;
    };
    std::vector<Lods> lods;
    decltype(VkSceneRenderer::dirty_cluster_ranges_) pending_dirty;
    decltype(VkSceneRenderer::instance_staging_) instance_staging_;
    decltype(VkSceneRenderer::instance_part_slots_) instance_part_slots_;
    decltype(VkSceneRenderer::rt_instances_) rt_instances_;
    decltype(VkSceneRenderer::command_template_) command_template_;
    decltype(VkSceneRenderer::raster_command_enabled_) raster_command_enabled_;
    decltype(VkSceneRenderer::part_instance_counts_) part_instance_counts_;
    decltype(VkSceneRenderer::part_command_ranges_) part_command_ranges_;
    decltype(VkSceneRenderer::instance_input_snapshot_) instance_input_snapshot_;
    decltype(VkSceneRenderer::slot_of_snapshot_) slot_of_snapshot_;
    decltype(VkSceneRenderer::part_cluster_snapshot_) part_cluster_snapshot_;
    decltype(VkSceneRenderer::raster_draw_command_count_) raster_draw_command_count_;
    decltype(VkSceneRenderer::max_clusters_per_instance_) max_clusters_per_instance_;
    decltype(VkSceneRenderer::draw_transform_slots_) draw_transform_slots_;
    decltype(VkSceneRenderer::skin_transform_base_) skin_transform_base_;
    decltype(VkSceneRenderer::water_animation_transform_base_) water_animation_transform_base_;
    decltype(VkSceneRenderer::static_instance_count_) static_instance_count_;
    decltype(VkSceneRenderer::static_rt_instance_count_) static_rt_instance_count_;
    decltype(VkSceneRenderer::snapshot_slot_of_version_) snapshot_slot_of_version_;
    decltype(VkSceneRenderer::instance_snapshot_generation_) instance_snapshot_generation_;
    decltype(VkSceneRenderer::instance_generation_) instance_generation_;
    decltype(VkSceneRenderer::temporal_history_changed_) temporal_history_changed_;
    decltype(VkSceneRenderer::instance_snapshot_valid_) instance_snapshot_valid_;
    decltype(VkSceneRenderer::command_template_dirty_) command_template_dirty_;
    decltype(VkSceneRenderer::command_metadata_dirty_cluster_) command_metadata_dirty_cluster_;
    explicit Impl(VkSceneRenderer& r):renderer(r),
        instance_staging_(r.instance_staging_),
        instance_part_slots_(r.instance_part_slots_),
        rt_instances_(r.rt_instances_),
        command_template_(r.command_template_),
        raster_command_enabled_(r.raster_command_enabled_),
        part_instance_counts_(r.part_instance_counts_),
        part_command_ranges_(r.part_command_ranges_),
        instance_input_snapshot_(r.instance_input_snapshot_),
        slot_of_snapshot_(r.slot_of_snapshot_),
        part_cluster_snapshot_(r.part_cluster_snapshot_),
        raster_draw_command_count_(r.raster_draw_command_count_),
        max_clusters_per_instance_(r.max_clusters_per_instance_),
        draw_transform_slots_(r.draw_transform_slots_),
        skin_transform_base_(r.skin_transform_base_),
        water_animation_transform_base_(r.water_animation_transform_base_),
        static_instance_count_(r.static_instance_count_),
        static_rt_instance_count_(r.static_rt_instance_count_),
        snapshot_slot_of_version_(r.snapshot_slot_of_version_),
        instance_snapshot_generation_(r.instance_snapshot_generation_),
        instance_generation_(r.instance_generation_),
        temporal_history_changed_(r.temporal_history_changed_),
        instance_snapshot_valid_(r.instance_snapshot_valid_),
        command_template_dirty_(r.command_template_dirty_),
        command_metadata_dirty_cluster_(r.command_metadata_dirty_cluster_) {}
    ~Impl() {
        if(committed) return;
        // Ordinary geometry was only added. Its normal recycler retains GPU
        // ranges through the in-flight window; accepted parts were untouched.
        for(auto hash:added) renderer.release_part(hash);
        renderer.instance_staging_.swap(instance_staging_);
        renderer.instance_part_slots_.swap(instance_part_slots_);
        renderer.rt_instances_.swap(rt_instances_);
        renderer.command_template_.swap(command_template_);
        renderer.raster_command_enabled_.swap(raster_command_enabled_);
        renderer.part_instance_counts_.swap(part_instance_counts_);
        renderer.part_command_ranges_.swap(part_command_ranges_);
        renderer.instance_input_snapshot_.swap(instance_input_snapshot_);
        renderer.slot_of_snapshot_.swap(slot_of_snapshot_);
        renderer.part_cluster_snapshot_.swap(part_cluster_snapshot_);
        renderer.raster_draw_command_count_=raster_draw_command_count_;
        renderer.max_clusters_per_instance_=max_clusters_per_instance_;
        renderer.draw_transform_slots_=draw_transform_slots_;
        renderer.skin_transform_base_=skin_transform_base_;
        renderer.water_animation_transform_base_=water_animation_transform_base_;
        renderer.static_instance_count_=static_instance_count_;
        renderer.static_rt_instance_count_=static_rt_instance_count_;
        renderer.snapshot_slot_of_version_=snapshot_slot_of_version_;
        renderer.instance_snapshot_generation_=instance_snapshot_generation_;
        renderer.instance_generation_=instance_generation_;
        renderer.temporal_history_changed_=temporal_history_changed_;
        renderer.instance_snapshot_valid_=instance_snapshot_valid_;
        renderer.command_template_dirty_=command_template_dirty_;
        renderer.command_metadata_dirty_cluster_=command_metadata_dirty_cluster_;
        if(!added.empty()) {
            renderer.instance_snapshot_valid_=false;
            renderer.command_template_dirty_=true;
            renderer.command_metadata_dirty_cluster_=std::min(
                renderer.command_metadata_dirty_cluster_,renderer.cluster_staging_.size());
        }
    }
};
ScenePublication::ScenePublication(VkSceneRenderer& r):impl_(std::make_unique<Impl>(r)) {}
ScenePublication::~ScenePublication()=default;
void ScenePublication::added_part(uint64_t hash) {impl_->added.push_back(hash);}
bool ScenePublication::prepare_lods(const VkScenePart& source,std::string& error) {
    auto& r=impl_->renderer;
    const auto found=r.slot_of_.find(source.part_hash);
    if(found==r.slot_of_.end()) {error="LOD candidate has no registered ordinary part";return false;}
    const auto& record=r.parts_[found->second];
    if(source.clusters.size()!=record.cluster_count) {
        error="LOD candidate changed ordinary part topology";return false;
    }
    Impl::Lods next;next.slot=uint32_t(found->second);
    bool open=false,any_mesh=false;
    for(size_t i=0;i<source.clusters.size();++i) {
        const auto index=record.cluster_start+i;
        const auto& src=source.clusters[i];const auto& old=r.cluster_lods_[index];
        if(src.lods.size()!=old.size()) {error="LOD candidate changed ordinary rung topology";return false;}
        auto gpu=r.cluster_staging_[index];std::vector<float> thresholds;
        for(size_t level=0;level<src.lods.size();++level) {
            if(src.lods[level].index_count!=old[level].index_count ||
               src.lods[level].first_index+(source.indices.empty()?0:uint64_t(record.index_start))!=old[level].first_index) {
                error="LOD candidate changed ordinary index ownership";return false;
            }
            thresholds.push_back(src.lods[level].threshold);
            gpu.switch_distances[level]=lod::normalized_switch_distance(src.lods[level].threshold);
        }
        const auto mesh_lods=record.rt_cluster_mesh_lods[i];
        if(mesh_lods) {
            any_mesh=true;
            const float distance=gpu.switch_distances[mesh_lods-1];
            open=open || std::isinf(distance);
            next.mesh_span=std::max(next.mesh_span,distance*gpu.radius);
        }
        next.clusters.push_back(gpu);next.thresholds.push_back(std::move(thresholds));
    }
    if(open || (any_mesh && !(next.mesh_span>0))) next.mesh_span=std::numeric_limits<float>::infinity();
    impl_->lods.push_back(std::move(next));return true;
}
bool ScenePublication::prepare_instances(const std::vector<VkSceneInstance>& instances,std::string& error) {
    auto& r=impl_->renderer;
    if(!r.update_instances(instances,error)) return false;
    // Build all range metadata before the GPU pair can be published.
    impl_->pending_dirty=r.dirty_cluster_ranges_;
    for(const auto& update:impl_->lods) {
        const auto& record=r.parts_[update.slot];
        impl_->pending_dirty.push_back({record.cluster_start,record.cluster_count});
    }
    return true;
}
void ScenePublication::commit() noexcept {
    auto& r=impl_->renderer;
    for(const auto& update:impl_->lods) {
        auto& record=r.parts_[update.slot];
        for(size_t i=0;i<update.clusters.size();++i) {
            const auto index=record.cluster_start+i;
            r.cluster_staging_[index]=update.clusters[i];
            for(size_t level=0;level<update.thresholds[i].size();++level)
                r.cluster_lods_[index][level].threshold=update.thresholds[i][level];
        }
        record.rt_mesh_span=update.mesh_span;
    }
    if(!impl_->lods.empty()) {
        r.dirty_cluster_ranges_.swap(impl_->pending_dirty);
        r.mark_static_append();++r.static_generation_;++r.rt_geometry_epoch_;
    }
    impl_->committed=true;
}
