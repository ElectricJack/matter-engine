matter::DlssMode VkSceneRenderer::active_dlss_mode() const {
    return dlss_bridge_->active_dlss_mode();
}

const std::string& VkSceneRenderer::dlss_reason() const {
    return dlss_bridge_->dlss_unavailable_reason();
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void VkSceneRenderer::set_test_dlss_bridge(matter::StreamlineBridge bridge) {
    test_dlss_bridge_override_ =
        std::make_unique<matter::StreamlineBridge>(std::move(bridge));
    dlss_bridge_ = test_dlss_bridge_override_.get();
}

bool VkSceneRenderer::test_uses_device_streamline_bridge() const {
    return vulkan_ && dlss_bridge_ == &vulkan_->streamline_bridge();
}

std::weak_ptr<void> VkSceneRenderer::test_dlss_output_lifetime(
    uint32_t frame_slot) const {
    return frame_slot < frames_.size()
               ? std::weak_ptr<void>(frames_[frame_slot].dlss_output.lifetime)
               : std::weak_ptr<void>{};
}

bool VkSceneRenderer::test_replace_dlss_output(uint32_t frame_slot,
                                               VkExtent2D extent,
                                               std::string& error) {
    if (frame_slot >= frames_.size()) {
        error = "test DLSS output frame slot is out of range";
        return false;
    }
    return ensure_dlss_output(frames_[frame_slot], extent, error);
}
#endif

VkExtent2D VkSceneRenderer::dlss_internal_extent(
    VkExtent2D output_extent) const {
    if (!dlss_bridge_->supports_dlss_mode(selected_dlss_mode_))
        return output_extent;
    matter::DlssOptimalSettings settings{};
    std::string ignored_error;
    if (dlss_bridge_->query_dlss_optimal_settings(
            {selected_dlss_mode_, output_extent, true, true}, settings,
            ignored_error))
        return settings.render_extent;
    return output_extent;
}

void VkSceneRenderer::set_dlss_mode(matter::DlssMode mode) {
    if (selected_dlss_mode_ == mode) return;
    selected_dlss_mode_ = mode;
    gi_history_reset_pending_ = true;
    local_direct_history_state_.reset_pending = true;
}
