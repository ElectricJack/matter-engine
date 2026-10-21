// The whole ray-tracing lane for one frame: BLAS selection and builds
// (build_ray_geometry), the TLAS instance list and its build or cache reuse
// (emit_ray_instances), and the trace plus denoisers
// (record_ray_trace_dispatch).
//
// Called from record_raster, AFTER the G-buffer -- the shadow and lighting
// raygen shaders reconstruct their ray origins from this frame's depth and
// identity attachments, so it cannot run earlier.
//
// Returns true having cleared visibility to fully-lit and the raw signals to
// zero when ray tracing is off, unavailable, or there is nothing to trace: a
// missing occluder reads as a soft lighting omission, which is the failure
// direction this lane always chooses. LOD selection here uses the CULL eye,
// not the live one, or the traced surface and the drawn surface disagree.
bool VkSceneRenderer::record_ray_traced_shadows(
    const matter::VulkanFrame& frame, const FrameMatrices& matrices,
    matter::Float3 camera_eye, float pixel_budget,
    VkExtent2D trace_extent, std::string& error) {
    if (!record_primary_light_cull(frame, matrices, error)) return false;
    auto clear_visibility = [&]() {
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        clear_color_image_for_use(frame.command_buffer, visibility_, one,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    auto clear_raw_diffuse = [&]() {
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        for (auto* image : {&raw_diffuse_, &raw_specular_,
                            &raw_specular_aux_, &raw_transmission_,
                            &raw_transmission_aux_, &raw_local_direct_}) {
            clear_color_image_for_use(
                frame.command_buffer, *image, zero,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        // Pre-record eligibility can find scene instances even when distance
        // or LOD selection leaves no traceable geometry. In that case the
        // volume pass will not initialize its output. Turning off vol_enabled
        // alone does not remove this statically used sampled descriptor: bind
        // the initialized neutral volume before the composite set's first use.
        // This frame slot has retired; no recorded pass has bound this set yet.
        const VkDescriptorImageInfo neutral_volume{
            vol_linear_sampler_, vol_dummy_3d_.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frames_[frame.frame_slot].composite_descriptor_set;
        write.dstBinding = 9;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &neutral_volume;
        update_descriptor_sets(vulkan_->device(), 1, &write, 0, nullptr);
    };
    const bool native_trace_enabled =
        ray_tracing_settings_.enabled && vulkan_->ray_tracing_available()
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        && !test_force_rt_unavailable_
#endif
        ;
    if (!native_trace_enabled || rt_instances_.empty()) {
        local_direct_history_state_.reset_pending = true;
        local_direct_history_state_.filtered_valid = false;
        clear_visibility();
        clear_raw_diffuse();
        return publish_local_direct_owner(
            frames_[frame.frame_slot], LocalDirectOwner::Raster, 0, error);
    }
    const auto& rt_properties = vulkan_->ray_tracing_properties();
    const uint64_t dispatch_invocations =
        static_cast<uint64_t>(trace_extent.width) * trace_extent.height;
    if (dispatch_invocations >
        rt_properties.max_ray_dispatch_invocation_count) {
        error = "ray tracing dispatch exceeds maxRayDispatchInvocationCount";
        return false;
    }
    if (frame.frame_slot >= frames_.size() ||
        frame.frame_slot >= rt_descriptor_sets_.size()) {
        error = "ray tracing requires prepared per-frame resources";
        return false;
    }
    const auto get_sizes = reinterpret_cast<
        PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(
        vulkan_->device(), "vkGetAccelerationStructureBuildSizesKHR"));
    const auto cmd_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(vulkan_->device(),
                            "vkCmdBuildAccelerationStructuresKHR"));
    const auto cmd_trace = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        vkGetDeviceProcAddr(vulkan_->device(), "vkCmdTraceRaysKHR"));
    if (!get_sizes || !cmd_build || !cmd_trace) {
        error = "native ray tracing command entry points are unavailable";
        return false;
    }
    std::vector<RtBuildSel> selected_geometry;
    std::vector<RtBlasPending> pending;
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        1, vulkan_->ray_tracing_properties()
               .min_acceleration_structure_scratch_offset_alignment);
    {
        PROFILE_SCOPE("rt.geometry");
        // The CULL eye, not the live one: this selection must land on the
        // same rung cull.comp draws, or the TLAS holds a different surface
        // than the G-buffer the shadow rays start from. With the cull camera
        // frozen the two eyes part company, terrain gets traced coarse while
        // drawn fine (or vice versa), and every region where the traced rung
        // sits proud of the drawn one goes black in sun visibility -- the
        // scattered coarse-triangle splotches of issue eb0b1070.
        if (!build_ray_geometry(frame, lod_selection_eye(camera_eye),
                                pixel_budget, get_sizes, cmd_build,
                                selected_geometry, pending, error))
            return false;
    }
    bool instances_empty = false;
    {
        // The per-frame TLAS instance rebuild. Sized by rt_instances_, so this
        // is the zone that grows with scene population rather than resolution.
        PROFILE_SCOPE("rt.instances");
        PROFILE_COUNT("rt.instance_count", rt_instances_.size());
        if (!emit_ray_instances(frame, get_sizes, cmd_build, scratch_alignment,
                                selected_geometry, pending,
                                instances_empty, error))
            return false;
    }
    if (instances_empty) {
        local_direct_history_state_.reset_pending = true;
        local_direct_history_state_.filtered_valid = false;
        clear_visibility();
        clear_raw_diffuse();
        return publish_local_direct_owner(
            frames_[frame.frame_slot], LocalDirectOwner::Raster, 0, error);
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    // The counter was already zeroed and seeded above; a failed record must
    // still leave it without a candidate.
    if (test_fail_next_rt_dispatch_record_) {
        test_fail_next_rt_dispatch_record_ = false;
        error = "injected RT dispatch record failure";
        return false;
    }
#endif
    {
        PROFILE_SCOPE("rt.dispatch");
        if (!record_ray_trace_dispatch(frame, matrices, trace_extent,
                                       cmd_trace, error))
            return false;
    }
    // Only a recorded production dispatch makes the counter a candidate
    // sample; finish_ray_tracing_frame promotes it if this serial submits.
    if (frame.serial != 0)
        frames_[frame.frame_slot].rt_counter_candidate_serial = frame.serial;
    return true;
}

// Picks the traced rung for every (RT instance, cluster) and records the BLAS
// builds needed to back them, filling `selected_geometry` (what the TLAS will
// reference) and `pending` (what is being built this frame).
//
// This is the RT mirror of cull.comp's selection and must agree with it term
// for term: same planning bounds, same radius, same average-basis scale, same
// lod_distance.h rule, and the same per-part max_draw_distance and lod_bias
// overrides. A divergence here does not merely cost performance -- it puts a
// different surface in the TLAS than the G-buffer the rays start from.
//
// Three exclusions, each argued at length inline: an instance whose nearest
// cluster is past its furthest mesh rung (the O(instances x clusters) early
// out), a cluster whose selected rung is the terminal impostor, and a cluster
// a skin raster draw owns. Builds are batched against a 64 MiB scratch budget,
// and any recorded build bumps rt_geometry_epoch_, retiring every cached TLAS.
bool VkSceneRenderer::build_ray_geometry(
    const matter::VulkanFrame& frame,
    matter::Float3 camera_eye, float pixel_budget,
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
    std::vector<RtBuildSel>& selected_geometry,
    std::vector<RtBlasPending>& pending,
    std::string& error, bool warmup_only) {
    FrameResources& selected = frames_[frame.frame_slot];
    VkDeviceSize scratch_size = 1;
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        1, vulkan_->ray_tracing_properties()
               .min_acceleration_structure_scratch_offset_alignment);
    // Same conservative all-LOD union the CPU skin lane and cull.comp plan
    // against. Fetched once: gpu_records() materializes a vector.
    const std::vector<VkAnimationBoundsGpuRecord> rt_planning_bounds =
        animation_bounds_.gpu_records();
    // Instances rejected by the billboard-only early-out below. Reported as a
    // counter so the win is measurable and a regression (it silently stopping
    // firing) is visible rather than merely slow.
    uint64_t rt_instances_skipped = 0;
    // Times the per-instance rung pick only; stopped before the BLAS lookups
    // and builds that consume its selection.
    PROFILE_SCOPE_NAMED(rung_select_scope, "rt.rung_select");
    size_t rt_scanned = 0;
    for (const RtInstance& source : rt_instances_) {
        if (warmup_only) break;
        ++rt_scanned;
        // Same flat mirror update_instances() uses -- this WAS a per-instance
        // std::map descent (slot_of_.find()); part_slot_lookup answers the
        // identical question in one open-addressed probe. The per-frame cost of
        // this loop is the O(instances x clusters) re-derivation below, NOT this
        // lookup (see the sibling note at update_instances()).
        const int found_slot = part_slot_lookup(source.part_hash);
        if (found_slot < 0) continue;
        PartRecord& part = parts_[static_cast<size_t>(found_slot)];
        if (!part.triangle_resident) continue;
        // ---- Per-module draw overrides, the RT half -------------------------
        //
        // cull.comp reads this same table (by cluster.part_slot, which is the
        // parts_ index this lookup returns -- see where GpuCluster::part_slot
        // is filled) and applies both fields in its `override_lod_bias` block,
        // immediately before it computes `reach`. This lane read NEITHER, and
        // that divergence is what the impostor reports were made of: dragging
        // a part's LOD bias moved the RASTER onto the card while
        // the traced mirror stayed on the mesh rung, so the tree's real
        // geometry sat in the TLAS underneath its own billboard. Hence the
        // coloured shadows cast by "impostored" foliage, and hence an impostor
        // card catching its own tree's shadow -- the card's reconstructed
        // pixels are inside the still-traced volume, the same pathology the
        // 48.5 %-lit measurement below describes.
        //
        // ORDERING. The table is rebuilt in upload_scene_buffers() (the
        // `part_draw_overrides_dirty_` branch), which runs from prepare_frame()
        // -- and matter_engine.cpp calls prepare_frame() before
        // record_cull_and_render(), which is what eventually records this
        // function. So the table read here is this frame's, the same bytes the
        // cull dispatch was handed. Nothing between those two points can set
        // the dirty flag; a set_draw_overrides() from the editor lands before
        // the next prepare_frame().
        //
        // The bound is a real guard, not a formality: the table is ONE neutral
        // entry whenever no module overrides anything, so almost every part
        // slot legitimately falls outside it and takes the neutral path.
        matter::PartDrawOverrideGpu part_override{};
        if (static_cast<size_t>(found_slot) < part_draw_override_table_.size())
            part_override = part_draw_override_table_[found_slot];
        matter::Mat4f object_to_world{};
        std::memcpy(object_to_world.m, source.transform,
                    sizeof(object_to_world.m));
        // ---- INSTANCE-LEVEL EARLY-OUT -----------------------------------
        //
        // Every cluster whose selected rung is the terminal billboard already
        // contributes nothing (the `lod_index >= mesh_lods` skip below). But
        // that verdict was reached only AFTER the scattered cluster_staging_
        // fetch, the planning-bounds build and the full LOD selection -- per
        // cluster, per instance, per frame. With ~88k vegetation instances a
        // capture measured build_ray_geometry at 5.8 ms of render-thread time
        // doing overwhelmingly that.
        //
        // If the NEAREST point any of this part's clusters can occupy is still
        // beyond the FURTHEST distance at which any of them keeps a mesh rung,
        // then every cluster is billboard-only and the whole instance can be
        // rejected before touching cluster data at all.
        //
        // Conservative on both terms, so it can never drop a caster:
        //   - rt_mesh_span takes the MAX over clusters, and the last mesh
        //     rung's switch distance, so it over-estimates the mesh range.
        //   - the distance bound subtracts the centre extent scaled by the
        //     LARGEST basis length, not the average, so it under-estimates how
        //     far the closest cluster is.
        // INFINITY (an open rung anywhere in the part) skips the test.
        //
        // Same rule, not a second one: the reach goes through lod::reach with
        // the same average-basis scale and the same `!= 1` lod_bias guard
        // select_cluster_lod_view uses.
        //
        // STATIC INSTANCES ONLY. An animated instance resolves its rung from
        // the animation bounds UNION (planning_radius below), which is larger
        // than the static cluster radius rt_mesh_span was folded from -- often
        // much larger, since the union covers skin and every rigid segment. The
        // early-out would then under-estimate its reach and skip a mesh that
        // does still trace, which is a skinned character silently losing its
        // shadow. The vegetation this optimisation exists for is all static, so
        // excluding animated instances costs nothing measurable.
        if (source.animation_instance_slot == UINT32_MAX &&
            std::isfinite(part.rt_mesh_span)) {
            const auto basis_len = [&](int c0, int c1, int c2) {
                const float a = object_to_world.m[c0];
                const float b = object_to_world.m[c1];
                const float c = object_to_world.m[c2];
                return std::sqrt(a * a + b * b + c * c);
            };
            const float lx = basis_len(0, 4, 8);
            const float ly = basis_len(1, 5, 9);
            const float lz = basis_len(2, 6, 10);
            const float scale = (lx + ly + lz) / 3.0f;
            const float dx = object_to_world.m[12] - camera_eye.x;
            const float dy = object_to_world.m[13] - camera_eye.y;
            const float dz = object_to_world.m[14] - camera_eye.z;
            const float origin_distance =
                std::sqrt(dx * dx + dy * dy + dz * dz);
            const float nearest_cluster =
                origin_distance -
                part.rt_center_extent * std::max(lx, std::max(ly, lz));
            float span_reach =
                lod::reach(part.rt_mesh_span, scale, pixel_budget);
            if (part_override.lod_bias != 1.0f)
                span_reach *= part_override.lod_bias;
            if (nearest_cluster > span_reach) {
                ++rt_instances_skipped;
                continue;
            }
        }
        for (uint32_t cluster_index = 0;
             cluster_index < part.cluster_count; ++cluster_index) {
            // The raster lanes already treat an accepted skin draw as the sole
            // owner of its (instance, generation, cluster): cull.comp drops the
            // static bind-pose draw the moment the bounds record carries the
            // skin-raster flag. Apply the same exclusion here. This BLAS is
            // immutable bind-pose geometry (see the build contract below), so
            // tracing it under a compute-skinned gbuffer buries posed pixels
            // inside the bind-pose silhouette -- their GI and sun rays self-hit
            // immediately and carve hard-edged dark patches across the animated
            // surface. Until a deforming-BLAS phase exists, a skinned cluster
            // contributes no traced geometry at all: a missing occluder reads
            // as a soft lighting omission, a wrong-pose occluder reads as
            // geometry. Skin fallbacks need no special case -- a BindPose
            // fallback publishes no draw, so the raster mesh IS the bind pose
            // and the traced copy aligns with it again.
            if (source.animation_instance_slot != UINT32_MAX &&
                animation_skin_raster_owns_cluster(
                    selected.ready_skin_raster_draws,
                    source.animation_instance_slot,
                    source.animation_instance_generation, cluster_index))
                continue;
            const uint32_t global_cluster =
                part.cluster_start + cluster_index;
            const GpuCluster& gpu_cluster = cluster_staging_[global_cluster];
            // A deforming instance's static cluster AABB is the part's whole
            // bind-pose extent (for a partitioned animated part, an
            // origin-centered box covering skin AND every rigid segment), so
            // its radius runs well above the animated skin's. Selecting from it
            // holds the traced rung one level finer than the rasterized one
            // across a wide band of distances, and both surfaces are on screen
            // at once: the raster rung shades the gbuffer while the finer
            // traced rung supplies GI and shadow rays, so the two silhouettes
            // interpenetrate and the mesh reads as two overlapping copies.
            // Resolving the same union the raster lanes use keeps the lanes on
            // the same rung at every distance.
            VkAnimationBoundsAabb planning_bounds{
                {gpu_cluster.aabb_min[0], gpu_cluster.aabb_min[1],
                 gpu_cluster.aabb_min[2]},
                {gpu_cluster.aabb_max[0], gpu_cluster.aabb_max[1],
                 gpu_cluster.aabb_max[2]}};
            float planning_radius = gpu_cluster.radius;
            if (source.animation_instance_slot != UINT32_MAX &&
                resolve_animation_cluster_union(
                    rt_planning_bounds, source.animation_instance_slot,
                    source.animation_instance_generation, cluster_index,
                    planning_bounds)) {
                const float dx = planning_bounds.max[0] - planning_bounds.min[0];
                const float dy = planning_bounds.max[1] - planning_bounds.min[1];
                const float dz = planning_bounds.max[2] - planning_bounds.min[2];
                planning_radius =
                    0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            // max_draw_distance: reject before any LOD work, on the same
            // distance and against the same bound cull.comp uses, computed
            // from the same planning bounds the rung choice uses below. (No
            // stat is bumped here; cull.comp counts its rejection into
            // frustum_culled, which is a RASTER draw statistic and would be
            // double-counted if this lane touched it.)
            //
            // DECISION -- a part past its draw distance casts NO shadow, and
            // that is deliberate. The alternative (keep tracing it so it can
            // still cast) was rejected on three grounds:
            //
            //   1. `hide`, the sibling control in the same struct, is already
            //      enforced on BOTH lanes -- draw_overrides.h spells out that a
            //      GPU-cull-only hide "would leave the module visible in
            //      reflections". max_draw_distance is "hide past N metres"; it
            //      would be incoherent for the distance form of the same idea
            //      to keep a traced copy the instant form removes.
            //   2. A shadow with no visible caster reads as a bug, loudly. The
            //      dial's typical use is scatter (grass, undergrowth) with a
            //      cutoff well inside the shadow-casting range, so the artefact
            //      would be a carpet of shadows over bare ground.
            //   3. It is a PERFORMANCE dial. Keeping the BLAS build and the
            //      TLAS instance alive past the cutoff spends most of what the
            //      dial was set to save.
            //
            // What this gives up is the long shadow of a tall object dropped by
            // a short cutoff. If that ever matters the fix is a separate
            // `shadow_draw_distance` field, not a silent divergence between the
            // lanes -- which is exactly the failure this whole change repairs.
            const float distance_to_eye = vk_scene_detail::cluster_distance_to_eye(
                {planning_bounds.min[0], planning_bounds.min[1],
                 planning_bounds.min[2]},
                {planning_bounds.max[0], planning_bounds.max[1],
                 planning_bounds.max[2]},
                object_to_world, camera_eye);
            if (part_override.max_draw_distance > 0.0f &&
                distance_to_eye > part_override.max_draw_distance)
                continue;
            // lod_bias rides through to the reach, exactly where cull.comp
            // applies it (`if (override_lod_bias != 1.0) reach *= ...`).
            // Everything else about this call already matched the shader: same
            // planning bounds, same radius, same scale-from-basis, same
            // switch-distance loop via lod_distance.h. Passing the bias is the
            // last term that differed.
            const uint32_t lod_index = vk_scene_detail::select_cluster_lod_view(
                {planning_bounds.min[0], planning_bounds.min[1],
                 planning_bounds.min[2]},
                {planning_bounds.max[0], planning_bounds.max[1],
                 planning_bounds.max[2]},
                planning_radius, gpu_cluster.switch_distances,
                gpu_cluster.lod_count, object_to_world, camera_eye,
                pixel_budget, part_override.lod_bias);
            // M2.5: a cluster at its terminal IMPOSTOR rung contributes no
            // traced geometry at all.
            //
            // The billboard is oriented in the VERTEX stage, so its BLAS holds
            // the quad unrotated: raster shows a camera-facing card and a ray
            // hits an axis-fixed rectangle. That is the rectangular shadow in
            // the report, and alpha testing cannot repair it -- the light's
            // silhouette of an object is not the camera's, so no single card
            // is right for both.
            //
            // The obvious repair is to CLAMP the traced rung down to the last
            // mesh rung instead of dropping the cluster, and that was tried
            // and measured first. It fixes the silhouette and it is strictly
            // worse to look at, because the card's DEPTH is wrong: the quad is
            // a camera-facing plane through the object's CENTRE, so every one
            // of its pixels reconstructs a world position INSIDE the mesh that
            // would now be traced, and rt_shadow.rgen's sun ray leaves that
            // origin already buried. Measured on RockGallery (one rock, one
            // pose, sun 45 deg elevation), mean sun visibility over the card:
            //
            //   traced quad (the defect)   48.5 % lit   mean luminance 0.107
            //   clamped to the mesh rung    2.6 % lit   mean luminance 0.018
            //   the mesh rung it replaces  100.0 % lit  mean luminance 0.185
            //
            // So the clamp trades a wrong shadow SHAPE for a black object.
            // Dropping the cluster instead is also what the tier was designed
            // for: raster.vert records that "an impostor's occlusion comes
            // from the atlas, so it is free" -- the baked AO in the shade
            // layer IS this rung's occlusion, and a traced self-shadow on top
            // of it is double-counting whichever way it comes out.
            //
            // What this gives up is the shadow the object CASTS on others
            // while it is at this rung. That rung is selected at ~10 px on
            // screen (impostor_bake.h's view-count derivation), so the lost
            // shadow is about that big -- and it was the wrong shape anyway.
            // Recovering it needs the traced mesh back plus a per-pixel signal
            // that lets rt_shadow/rt_lighting lift an impostor ray origin
            // clear of the depicted volume, the way pom_lift already does for
            // parallaxed ground. That is the follow-up, not this fix.
            //
            // Fail closed on a cluster with no mesh rung at all: same rule, no
            // geometry. A missing occluder reads as a soft lighting omission;
            // a wrong-shape occluder reads as geometry (the same argument the
            // skinned exclusion above makes).
            const uint32_t mesh_lods =
                cluster_index < part.rt_cluster_mesh_lods.size()
                    ? part.rt_cluster_mesh_lods[cluster_index]
                    : 0u;
            if (lod_index >= mesh_lods) continue;
            uint32_t record_index = 0;
            if (!vk_scene_detail::dense_rt_lod_index(
                    part.rt_cluster_lod_offsets, cluster_index, lod_index,
                    record_index))
                continue;
            RtLodRecord& record = part.rt_lods[record_index];
            if (record.primitive_count != 0) {
                const bool opaque = rt_material_ids_are_opaque(
                    material_staging_, record.material_ids);
                selected_geometry.push_back(
                    {&part, &record, &source, opaque});
            }
        }
    }
    rung_select_scope.stop();
    PROFILE_COUNT("instances.rt_scanned", rt_scanned);
    // Warm pages build through the same triangle BLAS path, but never enter
    // the TLAS until the owner publishes a complete replacement cut.
    const size_t visible_selection_count = selected_geometry.size();
    if (warmup_only) for (auto it = geometry_page_warmups_.begin(); it != geometry_page_warmups_.end();) {
        const auto found = slot_of_.find(*it);
        if (found == slot_of_.end() || geometry_page_render_ready(*it)) {
            it = geometry_page_warmups_.erase(it); continue;
        }
        auto& part = parts_[static_cast<size_t>(found->second)];
        if (!part.triangle_resident) { ++it; continue; }
        if (part.rt_lods.size() == 1) {
            auto& lod = part.rt_lods.front();
            if(lod.candidate_serial!=0){++it;continue;}
            selected_geometry.push_back({&part, &lod, nullptr,
                rt_material_ids_are_opaque(material_staging_, lod.material_ids)});
        }
        ++it;
    }
    // Start/collect every eligible lookup before GPU-work budgets. A cache
    // miss or slow read at the front must not serialize the rest of the queue.
    for (const RtBuildSel& selected_lod : selected_geometry) {
        auto& part = *selected_lod.part;
        auto& lod = *selected_lod.lod;
        if (!part.vertex_count || !part.index_count ||
            !vertices_.address || !indices_.address || lod.candidate_serial != 0 ||
            (lod.built && lod.geometry_opaque == selected_lod.opaque)) continue;
        if (blas_cache_ && blas_cache_->available() &&
            !part.blas_cache_directory.empty() && part.rt_lods.size() == 1) {
            const auto key = blas_cache_->key(part.blas_cache_content, selected_lod.opaque);
            if (key != lod.cache_key) {
                if (lod.cache_ticket) blas_cache_->disk.cancel(lod.cache_ticket);
                lod.cache_key = key; lod.cache_ticket = 0; lod.cache_checked = false;
                lod.cached_blas.reset();
            }
            if (!lod.cache_checked && blas_cache_->disk.poll(
                    part.blas_cache_directory, key, lod.cache_ticket, lod.cached_blas)) {
                lod.cache_checked = true;
                if (!lod.cached_blas) ++blas_cache_->misses;
            }
        }
    }
    uint64_t warm_build_bytes = 0, warm_restore_bytes = 0;
    double warm_build_ms = 0, warm_restore_ms = 0;
    for (const RtBuildSel& selected_lod : selected_geometry) {
        PartRecord& part = *selected_lod.part;
        RtLodRecord& lod = *selected_lod.lod;
        if (!part.vertex_count || !part.index_count ||
            !vertices_.address || !indices_.address ||
            lod.candidate_serial != 0 ||
            (lod.built && lod.geometry_opaque == selected_lod.opaque) ||
            std::any_of(pending.begin(), pending.end(),
                        [&lod](const RtBlasPending& item) { return item.lod == &lod; }))
            continue;
        const bool cache_enabled = blas_cache_ && blas_cache_->available() &&
            !part.blas_cache_directory.empty() && part.rt_lods.size() == 1;
        if (cache_enabled && !lod.cache_checked) continue;
        const bool restoring = bool(lod.cached_blas);
        const uint64_t bytes = restoring ? lod.cached_blas->size() :
            uint64_t(part.vertex_count)*sizeof(VkRasterVertex) +
            uint64_t(lod.primitive_count)*3*sizeof(uint32_t);
        auto& used_bytes = restoring ? warm_restore_bytes : warm_build_bytes;
        auto& used_ms = restoring ? warm_restore_ms : warm_build_ms;
        const uint64_t byte_budget = restoring ? (32ull << 20) : (4ull << 20);
        if (warmup_only && used_bytes &&
            (used_ms >= 4.0 || bytes > byte_budget - std::min(used_bytes, byte_budget)))
            continue;
        const auto work_start = std::chrono::steady_clock::now();
        auto cached_blas = std::move(lod.cached_blas);
        RtBlasPending item{};
        item.part = &part;
        item.lod = &lod;
        auto& triangles = item.geometry.geometry.triangles;
        triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = vertices_.address +
            VkDeviceSize{part.vertex_start} * sizeof(VkRasterVertex);
        triangles.vertexStride = sizeof(VkRasterVertex);
        triangles.maxVertex = part.vertex_count - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        // The LOD index is part-local; add the part's shared index range.
        triangles.indexData.deviceAddress =
            indices_.address +
            (VkDeviceSize{part.index_start} + lod.first_index) * sizeof(uint32_t);
        item.geometry.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        item.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        item.geometry.flags = selected_lod.opaque
                                  ? VK_GEOMETRY_OPAQUE_BIT_KHR
                                  : 0;
        item.build.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        item.build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        item.build.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        item.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        // C4's ray-tracing contract is intentionally conservative: the
        // compute-skinned raster stream never enters an RT BLAS. The immutable
        // part bind pose remains build-once until a later deforming-RT phase.
        // The selection loop above enforces the complement: while a skin draw
        // owns a cluster, its bind-pose BLAS stays out of the TLAS entirely,
        // so build-once geometry only ever traces where it is also rasterized.
        assert(skinned_rt_uses_bind_pose_blas());
        assert((item.build.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) == 0);
        assert(item.build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
        item.build.geometryCount = 1;
        item.build.pGeometries = &item.geometry;
        item.range.primitiveCount = lod.primitive_count;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        get_sizes(vulkan_->device(),
                  VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                  &item.build, &item.range.primitiveCount, &sizes);
        if (lod.built) {
            auto replacement = std::make_shared<
                matter::VkAccelerationStructureResource>();
            if (!matter::create_acceleration_structure(
                    *vulkan_, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                    sizes.accelerationStructureSize, *replacement, error))
                return false;
            lod.candidate = std::move(replacement);
            item.target = lod.candidate;
        } else {
            if (!lod.blas) {
                lod.blas = std::make_shared<
                    matter::VkAccelerationStructureResource>();
                if (!matter::create_acceleration_structure(
                        *vulkan_,
                        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                        sizes.accelerationStructureSize, *lod.blas, error))
                    return false;
            }
            item.target = lod.blas;
        }
        lod.candidate_opaque = selected_lod.opaque;
        item.build.dstAccelerationStructure = item.target->handle;
        item.scratch_size =
            (sizes.buildScratchSize + scratch_alignment - 1) /
            scratch_alignment * scratch_alignment;
        if (cached_blas) {
            item.restored = blas_cache_->restore(frame, cached_blas, *item.target, sizes.accelerationStructureSize, error);
            if (!item.restored) error.clear(); // optional derived cache: rebuild on any miss
            else item.scratch_size = 0;
        }
        used_bytes += bytes;
        used_ms += std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-work_start).count();
        pending.push_back(item);
    }
    PROFILE_COUNT("rt.warm_build_input_bytes", warm_build_bytes);
    PROFILE_COUNT("rt.warm_restore_bytes", warm_restore_bytes);
    selected_geometry.resize(visible_selection_count);
    // Builds within a batch get disjoint scratch regions so the GPU can
    // overlap them; the budget chunks first-load spikes into several batches
    // instead of growing the scratch buffer without bound.
    constexpr VkDeviceSize kBlasScratchBudget = 64ull << 20;
    std::vector<size_t> batch_ends;
    VkDeviceSize batch_offset = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (batch_offset > 0 &&
            batch_offset + pending[i].scratch_size > kBlasScratchBudget) {
            batch_ends.push_back(i);
            batch_offset = 0;
        }
        pending[i].scratch_offset = batch_offset;
        batch_offset += pending[i].scratch_size;
        scratch_size = std::max(scratch_size, batch_offset);
    }
    batch_ends.push_back(pending.size());
    if (!ensure_build_buffer(
            selected.rt_scratch, scratch_size + scratch_alignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            error)) return false;
    const VkDeviceAddress blas_scratch_address =
        (selected.rt_scratch.address + scratch_alignment - 1) /
        scratch_alignment * scratch_alignment;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> batch_builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> batch_ranges;
    size_t batch_begin = 0;
    const bool has_blas_work = !pending.empty();
    // Any recorded BLAS build changes geometry an already-built TLAS may
    // reference (rebuilt in place, or relocated to a freshly created candidate
    // structure). Retire every slot's cached TLAS so none can reference stale
    // or superseded bottom-level data.
    if (has_blas_work) ++rt_geometry_epoch_;
    if (has_blas_work && !warmup_only)
        write_gpu_timestamp(frame.command_buffer, kGpuZoneBlas, false, selected);
    for (const size_t batch_end : batch_ends) {
        if (batch_end == batch_begin) continue;
        batch_builds.clear();
        batch_ranges.clear();
        for (size_t i = batch_begin; i < batch_end; ++i) {
            RtBlasPending& item = pending[i];
            if (item.restored) continue;
            item.build.pGeometries = &item.geometry;
            item.build.scratchData.deviceAddress =
                blas_scratch_address + item.scratch_offset;
            batch_builds.push_back(item.build);
            batch_ranges.push_back(&item.range);
        }
        if (!batch_builds.empty()) cmd_build(frame.command_buffer,
                  static_cast<uint32_t>(batch_builds.size()),
                  batch_builds.data(), batch_ranges.data());
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
        batch_begin = batch_end;
    }
    if (has_blas_work && !warmup_only)
        write_gpu_timestamp(frame.command_buffer, kGpuZoneBlas, true, selected);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    test_last_rt_blas_build_count_ += static_cast<uint32_t>(std::count_if(pending.begin(), pending.end(), [](const RtBlasPending& item) { return !item.restored; }));
#endif
    if (!pending.empty()) {
        std::vector<std::shared_ptr<void>> retained{selected.rt_scratch.lifetime};
        for (auto& item : pending) {
            item.lod->candidate_serial = frame.serial;
            if (!item.restored && blas_cache_ && !item.part->blas_cache_directory.empty())
                blas_cache_->capture(frame, item.part->blas_cache_directory, item.lod->cache_key, item.target, item.part->geometry_budget_claim);
            retained.push_back(item.target->lifetime);
            retained.push_back(vertices_.lifetime);
            retained.push_back(indices_.lifetime);
        }
        if (!vulkan_->retain_for_frame(frame, std::move(retained), error)) return false;
    }
    PROFILE_COUNT("rt.instances_skipped", rt_instances_skipped);
    return true;
}

// The RT hit shader counts every surface lookup it rejects (a traced
// rt_parts record that is stale or unwritten), but the only other reader is a
// test-only readback compiled out of shipping builds. A rejection is exactly
// the signal that says whether the stale-rt_parts-tail clear below works.
//
// Called once per frame from record_cull_and_render, after begin_frame has
// waited on this slot's fence, so a sample here is RETIRED: its submission has completed. The
// counter bytes are consumed only when they carry a sample -- written by a
// dispatch recorded in a frame that was actually submitted (see
// rt_counter_candidate_serial) -- and exactly once: the sample is cleared
// before it is read, so neither a duplicate call nor a later frame of a slot
// that stopped tracing re-reads the same bytes. Whether RT is enabled NOW is
// irrelevant: the last sample submitted before RT turned off is still
// consumed. Every consumed sample is accumulated, equal consecutive counts
// included; only the warning is suppressed while the count is unchanged.
void VkSceneRenderer::report_rt_trace_counters(uint32_t frame_slot) {
    if (frame_slot >= frames_.size()) return;
    FrameResources& selected = frames_[frame_slot];
    if (primary_light_audit_ && selected.primary_light_audit_pending) {
        selected.primary_light_audit_pending = false;
        ++primary_light_audit_frames_;
        std::string diagnostic_error;
        // Inspect every retired audit frame, including short camera moves
        // between periodic status lines. Never throttle a correctness failure.
        if (matter::map_buffer(selected.rt_test_output, diagnostic_error) &&
            matter::invalidate_buffer(selected.rt_test_output, 56 * sizeof(uint32_t),
                8 * sizeof(uint32_t), diagnostic_error)) {
            const auto* words = static_cast<const uint32_t*>(selected.rt_test_output.mapped);
            if (words[58] != 0 || primary_light_audit_frames_ <= 3 ||
                primary_light_audit_frames_ % 120 == 0)
                MATTER_LOGI("primary-light-cull", "retired=%llu candidates=%u rejected=%u positive_rejected=%u kept=%u",
                    static_cast<unsigned long long>(primary_light_audit_frames_),
                    words[56], words[57], words[58], words[59]);
        } else {
            MATTER_LOGW("primary-light-cull", "audit readback failed: %s", diagnostic_error.c_str());
        }
    }
    if (rt_adaptive_diagnostics_ && selected.rt_adaptive_diagnostics_pending) {
        selected.rt_adaptive_diagnostics_pending = false;
        ++rt_adaptive_diagnostic_frames_;
        std::string diagnostic_error;
        // First few frames expose startup resets; then one sampled retired
        // frame per 120 avoids both logging spam and a history-size readback.
        if ((rt_adaptive_diagnostic_frames_ <= 3 ||
             rt_adaptive_diagnostic_frames_ % 120 == 0) &&
            matter::map_buffer(selected.rt_test_output, diagnostic_error) &&
            matter::invalidate_buffer(selected.rt_test_output,
                32 * sizeof(uint32_t), 24 * sizeof(uint32_t), diagnostic_error)) {
            const auto* w = static_cast<const uint32_t*>(selected.rt_test_output.mapped);
            const auto word64 = [&](uint32_t lo) {
                return static_cast<unsigned long long>(w[lo]) |
                       (static_cast<unsigned long long>(w[lo + 1]) << 32);
            };
            MATTER_LOGI("rt-adaptive",
                "retired=%llu one=%u four=%u reject gate=%u motion=%u reactive=%u "
                "bounds=%u history=%u identity=%u depth=%u normal=%u orm=%u "
                "moments=%u variance=%u cpu_gate=0x%x key=%016llx previous=%016llx "
                "token=%llu lights=%u",
                static_cast<unsigned long long>(rt_adaptive_diagnostic_frames_),
                w[32], w[44], w[33], w[34], w[35], w[36], w[37], w[38],
                w[39], w[40], w[41], w[42], w[43], w[48], word64(49), word64(51),
                word64(53), w[55]);
        }
    }
    const uint64_t sample_serial = selected.rt_counter_sample_serial;
    selected.rt_counter_sample_serial = 0;
    if (sample_serial == 0 ||
        selected.rt_error_counter.buffer == VK_NULL_HANDLE) return;
    std::string error;
    if (!matter::map_buffer(selected.rt_error_counter, error) ||
        !matter::invalidate_buffer(selected.rt_error_counter, 0,
                                   sizeof(GpuRtCounters), error)) {
        MATTER_LOGW("vk", "RT counter readback failed: %s\n", error.c_str());
        return;
    }
    const auto* gpu =
        static_cast<const GpuRtCounters*>(selected.rt_error_counter.mapped);
    const uint32_t rejected = gpu->invalid_part_records;
    ++rt_rejected_surface_lookup_samples_;
    rt_rejected_surface_lookups_total_ += rejected;
    if (pipeline_profile_enabled()) {
        MATTER_LOGI("rt-counter", "consumer=report_rt_trace_counters slot=%u "
                    "serial=%llu rejected=%u samples=%llu total=%llu",
                    frame_slot, static_cast<unsigned long long>(sample_serial),
                    rejected,
                    static_cast<unsigned long long>(rt_rejected_surface_lookup_samples_),
                    static_cast<unsigned long long>(rt_rejected_surface_lookups_total_));
    }
    const bool changed = rejected != rt_rejected_surface_lookups_last_logged_;
    rt_rejected_surface_lookups_last_logged_ = rejected;
    if (rejected == 0 || !changed) return;
    MATTER_LOGW("vk",
                 "RT rejected %u surface lookup(s) in the last retired RT "
                 "sample (%llu total) -- a rejected lookup traced a stale or "
                 "unwritten rt_parts record\n",
                 rejected,
                 static_cast<unsigned long long>(
                     rt_rejected_surface_lookups_total_));
    std::fflush(stderr);
}

// Turns the selected geometry into TLAS instances plus the parallel
// GpuRtPartRecord table the hit shaders resolve surfaces through, then either
// rebuilds this slot's TLAS or REUSES the cached one.
//
// Reuse requires a completed previous build for this slot, an unchanged
// geometry epoch, and byte-identical instance records -- an exact memcmp,
// which is what catches a cluster changing LOD. It matters: rebuilding every
// frame measured ~15 ms at 58k instances against ~1 ms of actual tracing.
//
// Sets `instances_empty` when there is nothing to trace (a normal outcome, not
// an error). The part table is cleared over its WHOLE allocation, not just the
// bytes this frame needs: rt_parts[] is an unsized SSBO array, so its length in
// the shader is the capacity, and a stale record from a denser frame is
// structurally valid while pointing at freed geometry.
bool VkSceneRenderer::emit_ray_instances(
    const matter::VulkanFrame& frame,
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
    VkDeviceSize scratch_alignment,
    const std::vector<RtBuildSel>& selected_geometry,
    const std::vector<RtBlasPending>& pending,
    bool& instances_empty,
    std::string& error) {
    instances_empty = false;
    FrameResources& selected = frames_[frame.frame_slot];
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<GpuRtPartRecord> part_records;
    instances.reserve(selected_geometry.size());
    part_records.reserve(selected_geometry.size());
    for (const RtBuildSel& selected_lod : selected_geometry) {
        const PartRecord& part = *selected_lod.part;
        const RtLodRecord& lod = *selected_lod.lod;
        const RtInstance& source = *selected_lod.source;
        const auto& traced_blas = lod.candidate ? lod.candidate : lod.blas;
        if (!traced_blas || !part.vertex_count || !part.index_count ||
            !vertices_.address || !indices_.address) continue;
        if (part_records.size() >= kTlasCustomIndexMax) {
            error = "RT geometry table exceeds TLAS custom-index capacity";
            return false;
        }
        VkAccelerationStructureInstanceKHR instance{};
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                instance.transform.matrix[row][col] =
                    source.transform[row * 4 + col];
        instance.instanceCustomIndex =
            static_cast<uint32_t>(part_records.size());
        instance.mask = selected_lod.opaque ? 0x01 : 0x02;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = traced_blas->address;
        instances.push_back(instance);
        GpuRtPartRecord record{};
        record.vertex_address = vertices_.address +
            VkDeviceSize{part.vertex_start} * sizeof(VkRasterVertex);
        record.index_address =
            indices_.address +
            (VkDeviceSize{part.index_start} + lod.first_index) * sizeof(uint32_t);
        record.vertex_stride = sizeof(viewer::VkRasterVertex);
        record.vertex_count = part.vertex_count;
        record.primitive_count = lod.primitive_count;
        record.valid = 1u;
        // WP-G: the VT slot for the exact rung this BLAS traces.
        // RtLodRecord::cluster_index is PART-LOCAL; cluster_lods_ (and the
        // raster vt_draw_slots table) are indexed globally, so rebase first.
        record.vt_slot =
            vt_slot_for_lod(part, part.cluster_start + lod.cluster_index,
                            lod.lod_index);
        if (source.vt_source_hash) {
            const int source_slot = part_slot_lookup(source.vt_source_hash);
            record.vt_slot = source_slot >= 0 && parts_[source_slot].live
                ? vt_slot_for_lod(parts_[source_slot], parts_[source_slot].cluster_start, 0)
                : 0;
        }
        record.water_binding_slot = part.water_binding_slot;
        record.water_generation = part.water_generation;
        part_records.push_back(record);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        const bool built_this_frame = std::any_of(
            pending.begin(), pending.end(), [&lod](const RtBlasPending& item) {
                return item.lod == &lod;
            });
        test_last_rt_geometry_records_.push_back(
            {part.hash, lod.cluster_index, lod.lod_index,
             instance.instanceCustomIndex, lod.first_index, lod.index_count,
             record.vertex_address, traced_blas->address,
             selected_lod.opaque, built_this_frame,
             record.water_binding_slot, record.water_generation});
#endif
    }
    if (instances.empty()) {
        instances_empty = true;
        return true;
    }
    const VkDeviceSize instance_bytes =
        instances.size() * sizeof(VkAccelerationStructureInstanceKHR);

    // The per-part geometry table is descriptor-bound and read by the RT
    // shaders every frame, so it is refreshed unconditionally -- before the
    // acceleration-structure work below, which may be skipped entirely. These
    // are plain host-visible writes with no command-buffer ordering against the
    // build, so hoisting them above it is behaviour-neutral.
    const VkDeviceSize part_bytes = std::max<VkDeviceSize>(
        sizeof(GpuRtPartRecord),
        part_records.size() * sizeof(GpuRtPartRecord));
    if (!ensure_buffer(selected.rt_parts, part_bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) ||
        !matter::map_buffer(selected.rt_parts, error) ||
        !matter::map_buffer(selected.rt_error_counter, error)) return false;
    // Clear the whole ALLOCATION, not just the bytes this frame needs.
    // ensure_buffer never shrinks and grows by a factor, so the buffer's
    // capacity is the session high-water mark -- and rt_parts[] is an unsized
    // SSBO array, so its length in the shader is that capacity. Clearing only
    // part_bytes leaves records from a denser frame readable in the tail, and
    // a stale record is structurally VALID (valid=1, vertex_stride=88, a real
    // primitive_count) while its vertex/index addresses point at part geometry
    // that has since been freed -- so load_rt_surface's guards pass and it
    // dereferences a dangling device address. Same trap as the cull.comp
    // lod_bias table. Zeroing the tail makes those guards effective: a stale
    // slot now reads valid==0, counts into invalid_part_records, and returns
    // an invalid surface instead of faulting the device.
    const VkDeviceSize part_clear_bytes = selected.rt_parts.size;
    std::memset(selected.rt_parts.mapped, 0,
                static_cast<size_t>(part_clear_bytes));
    if (!part_records.empty())
        std::memcpy(selected.rt_parts.mapped, part_records.data(),
                    part_records.size() * sizeof(GpuRtPartRecord));
    // Overwriting the counter retires whatever sample it held; a dispatch
    // recorded below becomes the new candidate.
    selected.rt_counter_sample_serial = 0;
    selected.rt_counter_candidate_serial = 0;
    std::memset(selected.rt_error_counter.mapped, 0, sizeof(GpuRtCounters));
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (test_rt_counter_seed_ != 0)
        static_cast<GpuRtCounters*>(selected.rt_error_counter.mapped)
            ->invalid_part_records = test_rt_counter_seed_;
    if (!matter::map_buffer(selected.rt_test_output, error)) return false;
    auto* test_words =
        static_cast<uint32_t*>(selected.rt_test_output.mapped);
    test_words[18] = 0u;
    test_words[19] = 0u;
    if (!matter::flush_buffer(selected.rt_test_output,
                              18 * sizeof(uint32_t),
                              2 * sizeof(uint32_t), error)) return false;
#endif
    if (!matter::flush_buffer(selected.rt_parts, 0, part_clear_bytes, error) ||
        !matter::flush_buffer(selected.rt_error_counter, 0,
                              sizeof(GpuRtCounters),
                              error)) return false;

    // Perf: rebuilding the TLAS from scratch every frame was by far the largest
    // GPU cost in dense streamed scenes (~15 ms at 58k instances) even though
    // the tracing that consumes it costs a fraction of that. An acceleration
    // structure is device resident and stays valid until either the records it
    // was built from or the bottom-level structures they reference change, so a
    // slot whose TLAS already encodes exactly these records can skip the
    // rebuild outright.
    //
    // Reuse requires ALL of:
    //   * a previously *completed* build for this slot -- rt_tlas_valid is only
    //     set from finish_ray_tracing_frame, on a frame that reported success;
    //   * an unchanged geometry epoch, bumped by every BLAS build, part release
    //     and reset(), i.e. every event that can change or free memory a cached
    //     TLAS points at; and
    //   * byte-identical instance records. Every field of
    //     VkAccelerationStructureInstanceKHR is assigned above from a
    //     value-initialised struct and the type is fully packed (each bitfield
    //     pair shares one 32-bit word), so memcmp is exact -- in particular it
    //     catches accelerationStructureReference changing when a cluster picks
    //     a different LOD.
    //
    // A slot only ever reuses its own TLAS, and a slot is not re-recorded until
    // its previous submission has retired, so the structure being read is
    // always fully built. Any miss falls through to the original full rebuild.
    const bool tlas_reusable =
        selected.rt_tlas_valid &&
        selected.rt_tlas.handle != VK_NULL_HANDLE &&
        selected.rt_tlas_geometry_epoch == rt_geometry_epoch_ &&
        selected.rt_tlas_instances.size() == instances.size() &&
        std::memcmp(selected.rt_tlas_instances.data(), instances.data(),
                    static_cast<size_t>(instance_bytes)) == 0;
    if (tlas_reusable) {
        // No acceleration-structure write is recorded this frame, so the
        // build->trace barrier below has nothing to order and is skipped too.
        ++rt_tlas_reuses_;
        return true;
    }
    ++rt_tlas_builds_;

    if (!ensure_build_buffer(selected.rt_instances, instance_bytes,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       error) ||
        !matter::map_buffer(selected.rt_instances, error)) return false;
    std::memcpy(selected.rt_instances.mapped, instances.data(),
                static_cast<size_t>(instance_bytes));
    if (!matter::flush_buffer(selected.rt_instances, 0, instance_bytes, error))
        return false;
    VkAccelerationStructureGeometryInstancesDataKHR instance_data{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instance_data.data.deviceAddress = selected.rt_instances.address;
    VkAccelerationStructureGeometryKHR tlas_geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.geometry.instances = instance_data;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // A TLAS is a shallow one-level structure, so FAST_TRACE's traversal-quality
    // benefit is marginal while its build cost is high. FAST_BUILD cuts the
    // per-frame rebuild ~2-3x with negligible trace impact (a StreamMountain
    // capture measured RT trace ~1ms against a 14.76ms TLAS rebuild). The BLAS
    // build (bottom-level, per-part) keeps PREFER_FAST_TRACE -- deep traversal is
    // where that flag actually pays off.
    tlas_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build.geometryCount = 1;
    tlas_build.pGeometries = &tlas_geometry;
    const uint32_t instance_count = static_cast<uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(vulkan_->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
              &tlas_build, &instance_count, &tlas_sizes);
    if (selected.rt_tlas.size < tlas_sizes.accelerationStructureSize) {
        selected.rt_tlas.reset();
        // The previous structure is gone; nothing cached about it survives.
        selected.rt_tlas_valid = false;
        if (!matter::create_acceleration_structure(
                *vulkan_, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                tlas_sizes.accelerationStructureSize, selected.rt_tlas,
                error)) return false;
    }
    if (!ensure_build_buffer(
            selected.rt_tlas_scratch,
            tlas_sizes.buildScratchSize + scratch_alignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            error)) return false;
    const VkDeviceAddress tlas_scratch_address =
        (selected.rt_tlas_scratch.address + scratch_alignment - 1) /
        scratch_alignment * scratch_alignment;
    tlas_build.dstAccelerationStructure = selected.rt_tlas.handle;
    tlas_build.scratchData.deviceAddress = tlas_scratch_address;
    VkAccelerationStructureBuildRangeInfoKHR tlas_range{};
    tlas_range.primitiveCount = instance_count;
    const VkAccelerationStructureBuildRangeInfoKHR* tlas_range_ptr = &tlas_range;
    write_gpu_timestamp(frame.command_buffer, kGpuZoneTlas, false, selected);
    cmd_build(frame.command_buffer, 1, &tlas_build, &tlas_range_ptr);
    VkMemoryBarrier2 as_to_ray{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    write_gpu_timestamp(frame.command_buffer, kGpuZoneTlas, true, selected);
    as_to_ray.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    as_to_ray.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    as_to_ray.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    as_to_ray.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo as_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    as_dependency.memoryBarrierCount = 1;
    as_dependency.pMemoryBarriers = &as_to_ray;
    vkCmdPipelineBarrier2(frame.command_buffer, &as_dependency);

    // Stage this build as the slot's cache candidate. Deliberately NOT marked
    // valid here: the build is only *recorded*, and a frame abandoned before
    // submission would leave the TLAS holding its previous contents.
    // finish_ray_tracing_frame promotes the candidate once the frame reports
    // success, and leaves rt_tlas_valid false on failure so the slot rebuilds.
    selected.rt_tlas_instances = std::move(instances);
    selected.rt_tlas_pending_epoch = rt_geometry_epoch_;
    selected.rt_tlas_pending_serial = frame.serial;
    selected.rt_tlas_valid = false;
    return true;
}

// Writes the RT descriptor set from current renderer state and records up to
// three traces plus the denoisers: the shadow raygen at the full trace extent,
// the full-resolution local-direct lighting raygen when local lights exist,
// then GI lighting at its diffuse and reflection/transmission extents, followed
// by record_gi_temporal + record_gi_atrous.
//
// The descriptor array is rebuilt every frame rather than patched, so a VT
// state change needs no separate "on change" write for the RT set -- and
// bindings 17-19 are built from exactly the same live/dummy state
// write_vt_descriptors_for_frame() uses, so a ray and a fragment always
// resolve against the identical pool, indirection and variant table. The one
// exception is the tileset group (15/16/28): it names only this slot's own
// tileset_image_infos and buffers, so it is written only when
// tileset_descriptor_revision_ says write_tileset_descriptors_for_frame()
// changed them since this slot's RT set last received them.
bool VkSceneRenderer::record_ray_trace_dispatch(
    const matter::VulkanFrame& frame,
    const FrameMatrices& matrices,
    VkExtent2D trace_extent,
    PFN_vkCmdTraceRaysKHR cmd_trace,
    std::string& error) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (test_rt_geometry_pipelines_only_ &&
        (gi_settings_.enabled || !local_light_publication_.records.empty())) {
        error = "geometry-only RT test fixture cannot dispatch GI or local lighting";
        return false;
    }
#endif
    FrameResources& selected = frames_[frame.frame_slot];
    // This target exists even without VT/extraction. Order color writes and
    // any extraction layout transition before primary RT storage reads.
    transition_for_use(frame.command_buffer, vt_feedback_, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    if (primary_light_audit_) {
        if (!matter::map_buffer(selected.rt_test_output, error)) return false;
        auto* words = static_cast<uint32_t*>(selected.rt_test_output.mapped);
        std::memset(words + 56, 0, 8 * sizeof(uint32_t));
        if (!matter::flush_buffer(selected.rt_test_output, 56 * sizeof(uint32_t),
                                  8 * sizeof(uint32_t), error)) return false;
        selected.primary_light_audit_pending = true;
    }
    transition_for_use(frame.command_buffer, visibility_, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    const VkClearColorValue gi_zero{{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_color_image_for_use(frame.command_buffer, raw_diffuse_, gi_zero,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    clear_color_image_for_use(frame.command_buffer, raw_local_direct_, gi_zero,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    for (auto* specular_image : {&raw_specular_, &raw_specular_aux_,
                                 &raw_transmission_, &raw_transmission_aux_}) {
        clear_color_image_for_use(
            frame.command_buffer, *specular_image, gi_zero,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    // Primary motion vectors cannot reveal an off-screen shadow/reflection
    // change. Both diffuse GI and rough reflection therefore compare actual
    // emitted TLAS records against their last successfully submitted scene.
    PROFILE_SCOPE_NAMED(tlas_hash_scope, "rt.tlas_hash");
    uint64_t key = 14695981039346656037ull;
    const auto hash_bytes = [&key](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            key ^= bytes[i];
            key *= 1099511628211ull;
        }
    };
    hash_bytes(selected.rt_tlas_instances.data(),
               selected.rt_tlas_instances.size() *
                   sizeof(VkAccelerationStructureInstanceKHR));
    hash_bytes(&rt_geometry_epoch_, sizeof(rt_geometry_epoch_));
    hash_bytes(&local_light_revision_, sizeof(local_light_revision_));
    hash_bytes(&material_shading_revision_, sizeof(material_shading_revision_));
    hash_bytes(&material_geometry_revision_, sizeof(material_geometry_revision_));
    hash_bytes(&ray_tracing_settings_.bias, sizeof(ray_tracing_settings_.bias));
    hash_bytes(&ray_tracing_settings_.max_normal_bias, sizeof(ray_tracing_settings_.max_normal_bias));
    hash_bytes(&gi_settings_.enabled, sizeof(gi_settings_.enabled));
    gi_candidate_scene_key_ = key;
    tlas_hash_scope.stop();
    // Validate history against this frame's emitted scene BEFORE selecting ray
    // counts. A changed off-screen blocker is invisible to primary velocity.
    const bool primary_history_valid = !temporal_frame_.reset &&
        !local_direct_history_state_.reset_pending &&
        local_direct_history_state_.presented_token != 0 &&
        key == local_direct_history_state_.presented_scene_key;
    if (rt_adaptive_diagnostics_) {
        if (!matter::map_buffer(selected.rt_test_output, error)) return false;
        auto* words = static_cast<uint32_t*>(selected.rt_test_output.mapped);
        std::memset(words + 32, 0, 24 * sizeof(uint32_t));
        // Snapshot the exact CPU gate for the retired frame's classification.
        words[48] = (temporal_frame_.reset ? 1u : 0u) |
            (local_direct_history_state_.reset_pending ? 2u : 0u) |
            (local_direct_history_state_.presented_token == 0 ? 4u : 0u) |
            (key != local_direct_history_state_.presented_scene_key ? 8u : 0u);
        words[49] = static_cast<uint32_t>(key);
        words[50] = static_cast<uint32_t>(key >> 32);
        words[51] = static_cast<uint32_t>(local_direct_history_state_.presented_scene_key);
        words[52] = static_cast<uint32_t>(local_direct_history_state_.presented_scene_key >> 32);
        words[53] = static_cast<uint32_t>(local_direct_history_state_.presented_token);
        words[54] = static_cast<uint32_t>(local_direct_history_state_.presented_token >> 32);
        words[55] = static_cast<uint32_t>(local_light_publication_.records.size());
        if (!matter::flush_buffer(selected.rt_test_output, 32 * sizeof(uint32_t),
                                  24 * sizeof(uint32_t), error)) return false;
    }
    matter::VkImageResource* primary_history_inputs[8]{};
    VkDescriptorImageInfo primary_history_infos[8]{};
    if (rt_primary_adaptive_) {
        GiHistorySet& previous =
            local_direct_history_[local_direct_history_state_.presented_index];
        matter::VkImageResource* inputs[] = {
            &velocity_, &reactivity_, &previous.history_length, &previous.depth,
            &previous.normal, &previous.identity, &previous.aux, &previous.moments};
        std::copy(std::begin(inputs), std::end(inputs), primary_history_inputs);
        for (uint32_t i = 0; i < 8; ++i) {
            auto& image = *primary_history_inputs[i];
            transition_for_use(frame.command_buffer, image,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
            primary_history_infos[i] = {composite_sampler_, image.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
    }
    VkWriteDescriptorSetAccelerationStructureKHR as_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &selected.rt_tlas.handle;
    VkDescriptorImageInfo depth_info{composite_sampler_, depth_.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo visibility_info{VK_NULL_HANDLE, visibility_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_diffuse_info{VK_NULL_HANDLE, raw_diffuse_.view,
                                           VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo albedo_info{composite_sampler_, albedo_.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo normal_info{composite_sampler_, normal_.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo orm_info{composite_sampler_, orm_.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo identity_info{composite_sampler_, material_instance_.view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo raw_specular_info{VK_NULL_HANDLE, raw_specular_.view,
                                            VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_specular_aux_info{VK_NULL_HANDLE,
                                                raw_specular_aux_.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_transmission_info{VK_NULL_HANDLE,
                                                raw_transmission_.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_transmission_aux_info{
        VK_NULL_HANDLE, raw_transmission_aux_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo raw_local_direct_info{
        VK_NULL_HANDLE, raw_local_direct_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo part_info{selected.rt_parts.buffer, 0,
                                     selected.rt_parts.size};
    VkDescriptorBufferInfo material_info{selected.materials.buffer, 0,
                                         selected.materials.size};
    VkDescriptorBufferInfo error_info{selected.rt_error_counter.buffer, 0,
                                      selected.rt_error_counter.size};
    VkDescriptorBufferInfo test_output_info{selected.rt_test_output.buffer, 0,
                                            selected.rt_test_output.size};
    // Phase 1 tileset Vulkan port (Task 6): bindings 15/16 (and 28 below)
    // mirror raster set 1's bindings 6/7/26, from the infos
    // write_tileset_descriptors_for_frame() captured for this slot. Skipped
    // below while tileset_descriptor_revision_ is unchanged.
    const auto& tileset_image_infos = selected.tileset_image_infos;
    VkDescriptorBufferInfo tileset_params_info{selected.tileset_params.buffer, 0,
                                               sizeof(TilesetParamsGpu)};
    // WP-G: bindings 17/18/19 mirror raster set 1's VT bindings 10/11/12,
    // built from exactly the same live/dummy state
    // write_vt_descriptors_for_frame() uses so a ray and a fragment resolve
    // against the identical pool, indirection and variant table.
    VkDescriptorImageInfo vt_pool_infos[vt::kVtChannelCount]{};
    const bool vt_live = vt_ && vt_->available();
    for (uint32_t c = 0; c < vt::kVtChannelCount; ++c) {
        vt_pool_infos[c].sampler =
            vt_live ? vt_->pool_sampler() : tileset_sampler_;
        vt_pool_infos[c].imageView =
            vt_live ? vt_->pool_view(c) : tileset_dummy_rgba8_.view;
        vt_pool_infos[c].imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // The indirection is a storage buffer since the buffer-indirection
    // redesign; the dummy storage buffer stands in until VT is live.
    VkDescriptorBufferInfo vt_indirection_info{
        vt_live ? vt_->indirection_buffer() : vt_dummy_storage_.buffer, 0,
        vt_live ? vt_->indirection_buffer_size() : VkDeviceSize{64}};
    VkDescriptorBufferInfo vt_variants_info{
        vt_live ? vt_->variant_buffer() : vt_dummy_storage_.buffer, 0,
        vt_live ? vt_->variant_buffer_size() : VkDeviceSize{64}};
    VkDescriptorBufferInfo vt_input_info{
        vt_live ? vt_->input_snapshot_buffer() : vt_dummy_storage_.buffer, 0,
        vt_live ? vt_->input_snapshot_buffer_size() : VkDeviceSize{64}};
    VkDescriptorBufferInfo vt_draw_info{selected.vt_draw_inputs.buffer, 0, selected.vt_draw_inputs.size};
    VkDescriptorImageInfo primary_input_info{
        VK_NULL_HANDLE, vt_feedback_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[25]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &as_write;
    writes[0].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (uint32_t i = 1; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 1
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = i == 1 ? &depth_info : &visibility_info;
    }
    const VkDescriptorBufferInfo* rt_buffers[] = {
        &part_info, &material_info, &error_info, &test_output_info};
    for (uint32_t i = 3; i < 7; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = rt_buffers[i - 3];
    }
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &raw_diffuse_info;
    VkDescriptorImageInfo* gi_inputs[] = {&albedo_info, &normal_info, &orm_info};
    for (uint32_t i = 8; i < 11; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = gi_inputs[i - 8];
    }
    VkDescriptorImageInfo* extra_infos[] = {
        &identity_info, &raw_specular_info, &raw_specular_aux_info,
        &raw_transmission_info};
    for (uint32_t i = 11; i < 15; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 11
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = extra_infos[i - 11];
    }
    writes[15].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[15].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[15].dstBinding = 15;
    writes[15].descriptorCount =
        kTilesetSourceDescriptors;
    writes[15].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[15].pImageInfo = tileset_image_infos.data();
    writes[16].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[16].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[16].dstBinding = 16;
    writes[16].descriptorCount = 1;
    writes[16].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[16].pBufferInfo = &tileset_params_info;
    for (uint32_t i = 17; i < 20; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[17].descriptorCount = vt::kVtChannelCount;
    writes[17].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[17].pImageInfo = vt_pool_infos;
    writes[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[18].pBufferInfo = &vt_indirection_info;
    writes[19].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[19].pBufferInfo = &vt_variants_info;
    // RT PBR Phase 1: binding 20 is the transmission aux storage image.
    writes[20].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[20].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[20].dstBinding = 20;
    writes[20].descriptorCount = 1;
    writes[20].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[20].pImageInfo = &raw_transmission_aux_info;
    writes[21].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[21].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[21].dstBinding = 26;
    writes[21].descriptorCount = 1;
    writes[21].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[21].pImageInfo = &raw_local_direct_info;
    for (uint32_t i = 22; i < 24; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i + 5u;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = i == 22 ? &vt_input_info : &vt_draw_info;
    }
    writes[24].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[24].dstSet = rt_descriptor_sets_[frame.frame_slot];
    writes[24].dstBinding = 29;
    writes[24].descriptorCount = 1;
    writes[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[24].pImageInfo = &primary_input_info;
    // This slot's RT set already holds the current tileset group unless
    // write_tileset_descriptors_for_frame() rebuilt some slot's infos since:
    // drop those three writes (432 samplers + 2 buffers) from the update.
    uint32_t write_count = 25;
    if (selected.rt_tileset_descriptor_revision == tileset_descriptor_revision_) {
        write_count = static_cast<uint32_t>(
            std::remove_if(std::begin(writes), std::end(writes),
                           [](const VkWriteDescriptorSet& write) {
                               return write.dstBinding == 15 ||
                                      write.dstBinding == 16 ||
                                      write.dstBinding == 28;
                           }) - std::begin(writes));
    }
    update_descriptor_sets(vulkan_->device(), write_count, writes, 0, nullptr);
    selected.rt_tileset_descriptor_revision = tileset_descriptor_revision_;
    if (rt_primary_adaptive_) {
        VkWriteDescriptorSet history_writes[8]{};
        for (uint32_t i = 0; i < 8; ++i) {
            auto& write = history_writes[i];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = rt_adaptive_descriptor_sets_[frame.frame_slot];
            write.dstBinding = i;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &primary_history_infos[i];
        }
        update_descriptor_sets(vulkan_->device(), 8, history_writes, 0, nullptr);
    }
    struct alignas(16) ShadowConstants {
        GpuMat4 clip_to_world;
        float to_sun_max_distance[4];
        float bias;
        uint32_t samples;
        uint32_t debug_view;
        // Ground-POM roof escape (mirrors rt_lighting.rgen's shading-origin
        // lift): world-space distance to lift the shadow ray origin along
        // the sun direction for materials that carry a ground tileset
        // detail slot. Sourced from the LIVE tileset POM settings so the
        // tuning UI's relief-cap slider keeps working (see
        // stage_tileset_params's identical pom_max_relief_m source).
        float pom_lift;
        // Sun angular size as a multiple of the shipped default, scaling the
        // radius of the cone the shadow rays are jittered inside. EXACTLY
        // 1.0f at the default diameter (a float divided by itself), so the
        // shader's `0.002 * sun_cone_scale` is bit-identical to the 0.002 it
        // used to hardcode. See matter/sun_angles.h.
        float sun_cone_scale;
        float max_normal_bias;
    } constants{};
    static_assert(offsetof(ShadowConstants,max_normal_bias)==100);
    constants.max_normal_bias=ray_tracing_settings_.max_normal_bias;
    constants.clip_to_world = pack_glsl_mat4(matrices.clip_to_world);
    constants.to_sun_max_distance[0] =
        atmosphere_replay_constants_.rt_to_sun.x;
    constants.to_sun_max_distance[1] =
        atmosphere_replay_constants_.rt_to_sun.y;
    constants.to_sun_max_distance[2] =
        atmosphere_replay_constants_.rt_to_sun.z;
    constants.to_sun_max_distance[3] = ray_tracing_settings_.max_distance;
    constants.bias = ray_tracing_settings_.bias;
    constants.samples = atmosphere_replay_constants_.rt_shadow_samples;
    constants.debug_view = ray_tracing_settings_.debug_view ? 1u : 0u;
    // With POM disabled the ground stays flat at the datum, so there is no
    // displaced roof to escape -- lifting by the relief cap would then push
    // the shadow origin past real contact occluders. Keep only the epsilon.
    constants.pom_lift = tileset_pom_settings_.enabled
                             ? tileset_pom_settings_.relief_cap_m + 0.02f
                             : 0.02f;
    constants.sun_cone_scale =
        atmosphere_replay_constants_.rt_shadow_sun_cone_scale;
    last_rt_samples_ = constants.samples;
    last_rt_debug_view_ = constants.debug_view != 0;
    vkCmdBindPipeline(frame.command_buffer,
                      VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_shadow_pipeline_.pipeline);
    const VkDescriptorSet rt_set = rt_descriptor_sets_[frame.frame_slot];
    const VkDescriptorSet rt_sets[] = {
        rt_set, frames_[frame.frame_slot].environment_descriptor_set,
        frames_[frame.frame_slot].local_light_descriptor_set};
    vkCmdBindDescriptorSets(frame.command_buffer,
                            VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            rt_pipeline_layout_, 0, 3, rt_sets, 0, nullptr);
    // Bind before the first trace: all statically referenced pipeline sets
    // stay valid even when the initial SBT selects the sun-shadow raygen.
    if (rt_primary_adaptive_)
        vkCmdBindDescriptorSets(frame.command_buffer,
            VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline_layout_,
            3, 1, &rt_adaptive_descriptor_sets_[frame.frame_slot], 0, nullptr);
    vkCmdPushConstants(frame.command_buffer, rt_pipeline_layout_,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(constants),
                       &constants);
    const VkStridedDeviceAddressRegionKHR callable{};
    FrameResources& rt_frame_slot = frames_[frame.frame_slot];
    write_gpu_timestamp(frame.command_buffer, kGpuZoneRt, false, rt_frame_slot);
    cmd_trace(frame.command_buffer, &rt_shadow_pipeline_.raygen,
              &rt_shadow_pipeline_.miss, &rt_shadow_pipeline_.hit, &callable,
              trace_extent.width, trace_extent.height, 1);
    // End sun shadows here; local-direct and GI traces have separate zones.
    write_gpu_timestamp(frame.command_buffer, kGpuZoneRt, true, rt_frame_slot);
    const bool trace_local_direct = !local_light_publication_.records.empty();
    if (gi_settings_.enabled || trace_local_direct) {
        const VkClearColorValue dispatch_zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        if (gi_settings_.enabled) {
            clear_color_image_for_use(
                frame.command_buffer, raw_diffuse_, dispatch_zero,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        struct alignas(16) GiConstants {
            GpuMat4 clip_to_world;
            float to_sun_intensity[4];
            float bias;
            float max_distance;
            uint32_t presented_frame_index;
            float max_reflection_roughness;
            float diffuse_multiplier;
            float reflection_multiplier;
            float emission_multiplier;
            // Sun size, in the three forms rt_lighting.rgen needs it. These
            // occupy what were pad0/pad1/pad2, so the block did not grow.
            // cos_edge/cos_core are the same CPU-computed disc thresholds
            // composite.frag gets (see VkSceneLighting); size_scale squares
            // into the reflection prefilter's solid angle.
            float sun_disc_cos_edge;
            float sun_disc_cos_core;
            float sun_size_scale;
            float water_animation_time_seconds;
            // Reuses the final 4-byte padding lane; the block remains 128 B.
            uint32_t shadow_samples;
        } gi{};
        static_assert(sizeof(GiConstants) == 128);
        gi.clip_to_world = pack_glsl_mat4(matrices.clip_to_world);
        gi.to_sun_intensity[0] = constants.to_sun_max_distance[0];
        gi.to_sun_intensity[1] = constants.to_sun_max_distance[1];
        gi.to_sun_intensity[2] = constants.to_sun_max_distance[2];
        gi.to_sun_intensity[3] = lighting_.sun_intensity;
        gi.bias = ray_tracing_settings_.bias;
        gi.max_distance = ray_tracing_settings_.max_distance;
        gi.presented_frame_index =
            static_cast<uint32_t>(temporal_frame_.presented_frame_index);
        gi.max_reflection_roughness =
            std::clamp(gi_settings_.max_reflection_roughness, 0.02f, 1.0f);
        gi.diffuse_multiplier = gi_settings_.diffuse_multiplier;
        gi.reflection_multiplier = gi_settings_.reflection_multiplier;
        gi.emission_multiplier =
            atmosphere_replay_constants_.rt_gi_emission_multiplier;
        gi.sun_disc_cos_edge =
            atmosphere_replay_constants_.rt_gi_sun_disc_cos_edge;
        gi.sun_disc_cos_core =
            atmosphere_replay_constants_.rt_gi_sun_disc_cos_core;
        gi.sun_size_scale =
            atmosphere_replay_constants_.rt_gi_sun_size_scale;
        gi.water_animation_time_seconds = water_animation_time_seconds_;
        const uint32_t local_shadow_samples =
            atmosphere_replay_constants_.rt_shadow_samples & 0xffffu;
        // The raygen needs both the dispatch kind and the actual scene GI
        // state. In particular, primary local-direct is a separate full-rate
        // dispatch even when GI is enabled, but glass weighting must match the
        // transmission lane that composite will consume later in the frame.
        constexpr uint32_t kGiDispatchBit = 0x80000000u;
        constexpr uint32_t kLocalDirectDispatchBit = 0x40000000u;
        constexpr uint32_t kSceneGiEnabledBit = 0x20000000u;
        const uint32_t scene_gi_state =
            gi_settings_.enabled ? kSceneGiEnabledBit : 0u;
        if (trace_local_direct) {
            write_gpu_timestamp(frame.command_buffer, kGpuZoneRtLocalDirect,
                                false, rt_frame_slot);
            // These multiplier lanes are unused by primary-only raygen. Carry
            // jitter delta in internal Y-down pixels without growing the 128B
            // push ABI; GI dispatches retain their original multiplier values.
            GiConstants primary_gi = gi;
            primary_gi.diffuse_multiplier =
                temporal_frame_.current_jittered.jitter_pixels[0] -
                temporal_frame_.previous_jittered.jitter_pixels[0];
            primary_gi.reflection_multiplier =
                temporal_frame_.current_jittered.jitter_pixels[1] -
                temporal_frame_.previous_jittered.jitter_pixels[1];
            primary_gi.shadow_samples = local_shadow_samples |
                                kLocalDirectDispatchBit | scene_gi_state |
                                (primary_history_valid ? 0x04000000u : 0u);
            vkCmdPushConstants(frame.command_buffer, rt_pipeline_layout_,
                               VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(primary_gi),
                               &primary_gi);
            const auto& primary = rt_separate_primary_
                ? rt_primary_pipeline_ : rt_lighting_pipeline_;
            vkCmdBindPipeline(frame.command_buffer,
                              VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, primary.pipeline);
            cmd_trace(frame.command_buffer, &primary.raygen, &primary.miss,
                      &primary.hit, &callable,
                      raster_extent_.width, raster_extent_.height, 1);
            write_gpu_timestamp(frame.command_buffer, kGpuZoneRtLocalDirect,
                                true, rt_frame_slot);
            ++last_rt_trace_dispatches_;
            if (!publish_local_direct_owner(selected,
                                            LocalDirectOwner::RayTraced,
                                            local_light_revision_, error)) {
                return false;
            }
            selected.local_direct_candidate_serial = frame.serial;
            selected.rt_adaptive_diagnostics_pending = rt_adaptive_diagnostics_;
        } else {
            if (!publish_local_direct_owner(selected, LocalDirectOwner::Raster,
                                            0, error)) {
                return false;
            }
        }
        if (gi_settings_.enabled) {
            write_gpu_timestamp(frame.command_buffer, kGpuZoneRtGi, false,
                                rt_frame_slot);
            constexpr uint32_t kDiffuseOnlyBit = 0x10000000u;
            constexpr uint32_t kReflectionOnlyBit = 0x08000000u;
            const auto trace_signal = [&](VkExtent2D extent, uint32_t mask) {
                gi.shadow_samples = local_shadow_samples |
                                    kGiDispatchBit | scene_gi_state | mask;
                vkCmdPushConstants(frame.command_buffer, rt_pipeline_layout_,
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(gi), &gi);
                const auto& signal = !rt_specialize_gi_ ? rt_lighting_pipeline_ :
                    (mask == kDiffuseOnlyBit ? rt_diffuse_pipeline_ : rt_reflection_pipeline_);
                vkCmdBindPipeline(frame.command_buffer,
                                  VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, signal.pipeline);
                cmd_trace(frame.command_buffer, &signal.raygen, &signal.miss,
                          &signal.hit, &callable, extent.width, extent.height, 1);
                ++last_rt_trace_dispatches_;
            };
            if (raw_diffuse_extent_.width == raw_reflection_extent_.width &&
                raw_diffuse_extent_.height == raw_reflection_extent_.height &&
                !lighting_detail_timers_enabled() && !rt_specialize_gi_) {
                // The reference option retains the original combined dispatch
                // at equal extents unless detailed profiling requests a split.
                trace_signal(raw_diffuse_extent_, 0u);
            } else {
                // These dispatches write disjoint signal images. Their shared
                // scene/material inputs are read-only; denoising follows both.
                // Distinct query pairs nest inside RtGi. Each pair is written
                // once per frame; reflection and transmission share one dispatch.
                write_gpu_timestamp(frame.command_buffer, kGpuZoneRtGiDiffuse,
                                    false, rt_frame_slot);
                trace_signal(raw_diffuse_extent_, kDiffuseOnlyBit);
                write_gpu_timestamp(frame.command_buffer, kGpuZoneRtGiDiffuse,
                                    true, rt_frame_slot);
                write_gpu_timestamp(frame.command_buffer,
                                    kGpuZoneRtGiReflectionTransmission,
                                    false, rt_frame_slot);
                trace_signal(raw_reflection_extent_, kReflectionOnlyBit);
                write_gpu_timestamp(frame.command_buffer,
                                    kGpuZoneRtGiReflectionTransmission,
                                    true, rt_frame_slot);
            }
            write_gpu_timestamp(frame.command_buffer, kGpuZoneRtGi, true,
                                rt_frame_slot);
        }
    }
    if (gi_settings_.enabled || trace_local_direct)
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDenoise, false,
                            rt_frame_slot);
    if (gi_settings_.enabled) {
        if (!record_gi_temporal(frame, error)) return false;
        if (!record_gi_atrous(frame, error)) return false;
    }
    if (trace_local_direct) {
        local_direct_history_state_.candidate_scene_key = key;
        if (!record_gi_temporal_signal(frame, 3u, error, true) ||
            !record_gi_atrous_signal(frame, 3u, error, true))
            return false;
    } else {
        local_direct_history_state_.reset_pending = true;
        local_direct_history_state_.filtered_valid = false;
    }
    if (gi_settings_.enabled || trace_local_direct)
        write_gpu_timestamp(frame.command_buffer, kGpuZoneDenoise, true,
                            rt_frame_slot);
    VkMemoryBarrier2 counters_to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    counters_to_host.srcStageMask =
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    counters_to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    counters_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    counters_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo counters_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    counters_dependency.memoryBarrierCount = 1;
    counters_dependency.pMemoryBarriers = &counters_to_host;
    vkCmdPipelineBarrier2(frame.command_buffer, &counters_dependency);
    last_rt_effective_ = true;
    ++last_rt_trace_dispatches_;
    last_rt_fallback_reason_.clear();
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
        gi_settings_.enabled ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                             : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        gi_settings_.enabled ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                             : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    transition_for_use(frame.command_buffer, raw_local_direct_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    matter::VkImageResource& composite_specular =
        gi_settings_.enabled && gi_filtered_valid_
            ? gi_spec_atrous_[gi_filtered_index_] : raw_specular_;
    transition_for_use(frame.command_buffer, composite_specular,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    // RT PBR Phase 1: the composite now reads the denoised transmission when
    // the GI chain ran (update_composite_descriptor picks the same image).
    matter::VkImageResource& composite_transmission =
        gi_settings_.enabled && gi_filtered_valid_
            ? gi_trans_atrous_[gi_filtered_index_] : raw_transmission_;
    transition_for_use(frame.command_buffer, composite_transmission,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    transition_for_use(frame.command_buffer, raw_transmission_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    std::vector<std::shared_ptr<void>> retained{visibility_.lifetime,
        raw_diffuse_.lifetime, raw_specular_.lifetime,
        raw_specular_aux_.lifetime, raw_transmission_.lifetime,
        raw_transmission_aux_.lifetime, raw_local_direct_.lifetime,
        composite_specular.lifetime, composite_transmission.lifetime,
        selected.rt_instances.lifetime, selected.rt_scratch.lifetime,
        selected.rt_tlas_scratch.lifetime,
        selected.rt_tlas.lifetime, selected.rt_parts.lifetime,
        selected.rt_error_counter.lifetime, selected.materials.lifetime,
        selected.rt_test_output.lifetime,
        rt_shadow_pipeline_.sbt.lifetime, rt_surface_test_pipeline_.sbt.lifetime,
        rt_lighting_pipeline_.sbt.lifetime, rt_primary_pipeline_.sbt.lifetime,
        rt_diffuse_pipeline_.sbt.lifetime, rt_reflection_pipeline_.sbt.lifetime,
        vertices_.lifetime, indices_.lifetime};
    if (rt_primary_adaptive_)
        for (const auto* image : primary_history_inputs)
            retained.push_back(image->lifetime);
    for (const auto& part : parts_) {
        for (const auto& lod : part.rt_lods) {
            if (lod.candidate)
                retained.push_back(lod.candidate->lifetime);
            else if (lod.blas)
                retained.push_back(lod.blas->lifetime);
        }
    }
    if (!vulkan_->retain_for_frame(frame, std::move(retained), error))
        return false;
    return true;
}
