// Stores this frame's temporal (motion-vector / history) state and records, in
// `temporal_history_changed_`, whether anything update_instances() actually
// READS has changed since its last snapshot. The flag is sticky until
// update_instances takes a fresh snapshot, so any number of calls between two
// update_instances() calls are covered.
void VkSceneRenderer::set_temporal_frame(const TemporalFrame& frame) {
    // Track, for update_instances()' unchanged-input fast path, whether the
    // history data it actually reads changed. That is exactly:
    //   * `reset` (gates the whole history lookup), and
    //   * per entry and IN ORDER (the id index keeps the first match, so order
    //     is load-bearing when ids repeat): instance_id, history_valid, and
    //     previous_object_to_world -- the last only when history_valid is set,
    //     because it is not read otherwise.
    // current_object_to_world is never read by update_instances().
    //
    // The flag is sticky: it is only cleared when update_instances() takes a
    // fresh snapshot, so any number of set_temporal_frame() calls between two
    // update_instances() calls are covered.
    if (!temporal_copy_fuse_enabled()) {
        if (!temporal_history_changed_) {
            matter::profile::Scope compare_scope(
                engine_prof::id(engine_prof::kSetTemporalCompare));
            bool same = temporal_frame_.reset == frame.reset;
            // When both frames are reset the history contributes nothing to the
            // output in either case, so the per-entry data cannot matter.
            if (same && !frame.reset) {
                same =
                    temporal_frame_.instances.size() == frame.instances.size();
                for (size_t index = 0; same && index < frame.instances.size();
                     ++index) {
                    const TemporalInstanceFrame& previous =
                        temporal_frame_.instances[index];
                    const TemporalInstanceFrame& next = frame.instances[index];
                    if (previous.instance_id != next.instance_id ||
                        previous.history_valid != next.history_valid ||
                        (next.history_valid &&
                         std::memcmp(
                             previous.previous_object_to_world.m,
                             next.previous_object_to_world.m,
                             sizeof(next.previous_object_to_world.m)) != 0))
                        same = false;
                }
            }
            if (!same) temporal_history_changed_ = true;
        }
        {
            matter::profile::Scope copy_scope(
                engine_prof::id(engine_prof::kSetTemporalCopy));
            temporal_frame_ = frame;
        }
        return;
    }
    // Perf: the compare above and the `temporal_frame_ = frame` below each walk
    // the same ~90k-entry, ~13 MB TemporalInstanceFrame array, so the pair cost
    // three full traversals per frame -- and vector's copy-assign allocates
    // EXACTLY the source size when it has to grow, which during a fill is every
    // frame (13 MB allocated, copied and freed each time). One pass does both,
    // and resize_geometric restores amortised growth. The comparison result and
    // the resulting contents are identical.
    const bool compare_needed = !temporal_history_changed_;
    const size_t count = frame.instances.size();
    // Exactly the branch structure of the legacy path above: the per-entry data
    // (and therefore a size mismatch) only matters when neither frame is a
    // reset, because on a reset the history contributes nothing either way.
    bool same = temporal_frame_.reset == frame.reset;
    const bool history_matters = compare_needed && same && !frame.reset;
    if (history_matters && temporal_frame_.instances.size() != count)
        same = false;
    const bool per_entry_compare = history_matters && same;
    {
        matter::profile::Scope copy_scope(engine_prof::id(engine_prof::kSetTemporalCopy));
        // Grow first so every destination entry exists. When the sizes differ
        // `same` is already settled, so the stale tail is never compared.
        vk_perf::resize_geometric(temporal_frame_.instances, count);
        for (size_t index = 0; index < count; ++index) {
            TemporalInstanceFrame& previous = temporal_frame_.instances[index];
            const TemporalInstanceFrame& next = frame.instances[index];
            if (per_entry_compare && same &&
                (previous.instance_id != next.instance_id ||
                 previous.history_valid != next.history_valid ||
                 (next.history_valid &&
                  std::memcmp(previous.previous_object_to_world.m,
                              next.previous_object_to_world.m,
                              sizeof(next.previous_object_to_world.m)) != 0)))
                same = false;
            previous = next;
        }
    }
    if (compare_needed && !same) temporal_history_changed_ = true;
    // Everything except the instance array, which the loop above already owns.
    temporal_frame_.current_unjittered = frame.current_unjittered;
    temporal_frame_.previous_unjittered = frame.previous_unjittered;
    temporal_frame_.current_jittered = frame.current_jittered;
    temporal_frame_.previous_jittered = frame.previous_jittered;
    temporal_frame_.internal_extent = frame.internal_extent;
    temporal_frame_.output_extent = frame.output_extent;
    temporal_frame_.jitter_pixels[0] = frame.jitter_pixels[0];
    temporal_frame_.jitter_pixels[1] = frame.jitter_pixels[1];
    temporal_frame_.reset = frame.reset;
    temporal_frame_.attempt_token = frame.attempt_token;
    temporal_frame_.presented_frame_index = frame.presented_frame_index;
}
