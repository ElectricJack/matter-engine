bool VkSceneRenderer::consume_dlss_history_reset() {
    const bool pending = dlss_history_reset_pending_;
    dlss_history_reset_pending_ = false;
    // Explicit RT/GI/light edits invalidate whole-frame lighting independently
    // of GI's candidate history. In particular, a previous GI frame's reset
    // flag must not swallow a new control edit before the next frame starts.
    // Bridge-owned transition notifications are coalesced at their source.
    return pending;
}
