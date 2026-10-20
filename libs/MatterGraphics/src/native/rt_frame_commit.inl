// The commit point for everything this renderer staged during `frame_serial`.
// Must be called exactly once per recorded frame, with `succeeded` reporting
// whether that frame was actually SUBMITTED -- a frame abandoned before
// submission leaves every staged structure holding its previous contents.
//
// On success it promotes: the GI candidate history to presented (and clears
// whichever reset flags that candidate consumed), each candidate BLAS to the
// live one, the slot's staged TLAS to reusable, and the slot's RT counter
// candidate to a sample the next retirement may consume. On failure it drops the
// candidates and leaves the slot to rebuild. Nothing here bumps the geometry
// epoch: the build that produced a candidate already did, and bumping again
// would retire the TLAS built alongside it, which is still perfectly valid.
std::array<uint64_t, 4> VkSceneRenderer::blas_cache_stats() const {
    return blas_cache_ ? std::array<uint64_t,4>{blas_cache_->hits, blas_cache_->misses, blas_cache_->captures, blas_cache_->rejected} : std::array<uint64_t,4>{};
}

void VkSceneRenderer::finish_ray_tracing_frame(uint64_t frame_serial,
                                               bool succeeded) {
    if (blas_cache_) blas_cache_->finish(frame_serial, succeeded);
    if (frame_serial == 0) return;
    for (auto& frame : frames_) if (frame.geometry_feedback_pending_serial == frame_serial) {
        frame.geometry_feedback_valid = succeeded;
        frame.geometry_feedback_serial = succeeded ? frame_serial : 0;
        frame.geometry_feedback_pending_serial = 0;
    }
    if(sparse_candidate_serial_==frame_serial) {
        if(succeeded) {sparse_presented_=sparse_candidate_;sparse_presented_index_=sparse_candidate_index_;}
        sparse_candidate_.reset();sparse_candidate_serial_=0;
    }
    auto& direct = local_direct_history_state_;
    if (direct.candidate_serial == frame_serial) {
        if (succeeded) {
            direct.presented_index = direct.candidate_index;
            direct.presented_token = direct.candidate_token;
            direct.presented_scene_key = direct.candidate_scene_key;
            direct.reset_pending = false;
        }
        direct.candidate_serial = 0;
        direct.candidate_token = 0;
        direct.filtered_valid = false;
    }
    if (gi_candidate_frame_serial_ == frame_serial) {
        if (succeeded) {
            gi_presented_history_index_ = gi_candidate_history_index_;
            gi_presented_attempt_token_ = gi_candidate_attempt_token_;
            gi_presented_scene_key_ = gi_candidate_scene_key_;
            if (gi_candidate_was_reset_)
                gi_history_reset_pending_ = false;
            if (gi_candidate_used_diffuse_reset_)
                gi_diffuse_history_reset_pending_ = false;
            if (gi_candidate_used_reflection_reset_)
                gi_reflection_history_reset_pending_ = false;
            if (gi_candidate_was_reset_) ++gi_history_reset_count_;
        }
        gi_candidate_frame_serial_ = 0;
        gi_candidate_attempt_token_ = 0;
    }
    for (auto& part : parts_) {
        bool finished_candidate = false;
        for (auto& lod : part.rt_lods) {
            if (lod.candidate_serial != frame_serial) continue;
            finished_candidate = true;
            if (lod.candidate) {
                if (succeeded) {
                    lod.blas = std::move(lod.candidate);
                    lod.built = true;
                    lod.geometry_opaque = lod.candidate_opaque;
                } else {
                    lod.candidate.reset();
                }
            } else {
                lod.built = succeeded;
                if (succeeded) lod.geometry_opaque = lod.candidate_opaque;
            }
            lod.candidate_serial = 0;
        }
        if (succeeded && finished_candidate) {
            part.rt_geometry_classification_dirty = std::any_of(
                part.rt_lods.begin(), part.rt_lods.end(),
                [&](const RtLodRecord& lod) {
                    return lod.built && lod.geometry_opaque !=
                        rt_material_ids_are_opaque(material_staging_,
                                                   lod.material_ids);
                });
        }
        // No epoch bump is needed for the promotion itself. Promoting drops the
        // shared_ptr to the BLAS a candidate replaced, but a candidate can only
        // exist because build_ray_geometry recorded a build for it earlier in
        // this same frame, and that already bumped the epoch — which retired
        // every cached TLAS describing the superseded structure. Bumping again
        // here would additionally retire the TLAS built alongside the
        // candidate, which is still perfectly valid (promotion moves ownership,
        // not the device address).
    }
    // Commit or discard this frame's staged TLAS. On success the slot's
    // structure now genuinely holds the staged records and may be reused; on
    // failure the frame was never submitted, so the structure's contents are
    // unknown and the slot must rebuild.
    for (FrameResources& slot : frames_) {
        // A counter written by this frame's dispatch becomes a sample only if
        // the frame was submitted; an unsubmitted frame's bytes say nothing.
        if (slot.rt_counter_candidate_serial == frame_serial) {
            if (succeeded) slot.rt_counter_sample_serial = frame_serial;
            slot.rt_counter_candidate_serial = 0;
        }
        if (slot.local_direct_candidate_serial == frame_serial) {
            if (succeeded) {
                local_direct_owner_ = slot.local_direct_owner;
                local_direct_lane_revision_ =
                    slot.local_direct_lane_revision;
            } else {
                local_direct_owner_ = LocalDirectOwner::Raster;
                local_direct_lane_revision_ = 0;
                slot.local_direct_owner = LocalDirectOwner::Raster;
                slot.local_direct_lane_revision = 0;
            }
            slot.local_direct_candidate_serial = 0;
        }
        if (slot.rt_tlas_pending_serial != frame_serial) continue;
        slot.rt_tlas_pending_serial = 0;
        if (!succeeded) continue;
        slot.rt_tlas_geometry_epoch = slot.rt_tlas_pending_epoch;
        slot.rt_tlas_valid = true;
    }
}
