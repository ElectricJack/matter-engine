void VkSceneRenderer::set_ray_tracing_settings(
    const matter::VulkanRayTracingSettings& settings) {
    const float normal_bias=std::isfinite(settings.max_normal_bias)?std::max(0.0f,settings.max_normal_bias):.5f;
    if (ray_tracing_settings_.enabled != settings.enabled || ray_tracing_settings_.max_normal_bias!=normal_bias) {
        gi_history_reset_pending_ = true;
        local_direct_history_state_.reset_pending = true;
        // RT enablement also changes who owns primary local direct (raster or
        // traced). Whole-frame temporal upscaling must not reuse the previous
        // owner's lighting, even though the GI histories have their own reset.
        dlss_history_reset_pending_ = true;
        temporal_history_changed_ = true;
    }
    ray_tracing_settings_ = settings;
    ray_tracing_settings_.max_normal_bias=normal_bias;
    ray_tracing_settings_.samples =
        std::max(1u, std::min(settings.samples, 16u));
}

void VkSceneRenderer::set_gi_settings(
    const matter::VulkanGiSettings& settings) {
    matter::VulkanGiSettings normalized = settings;
    normalized.max_bounces = 1u;
    normalized.samples_per_pixel = 1u;
    normalized.trace_scale = std::isfinite(settings.trace_scale)
        ? std::clamp(settings.trace_scale, 0.125f, 1.0f) : 1.0f;
    normalized.reflection_trace_scale = std::isfinite(settings.reflection_trace_scale)
        ? std::clamp(settings.reflection_trace_scale, 0.125f, 1.0f) : 1.0f;
    if (std::memcmp(&gi_settings_, &normalized, sizeof(normalized)) == 0)
        return;
    gi_settings_ = normalized;
    gi_history_reset_pending_ = true;
    local_direct_history_state_.reset_pending = true;
    gi_diffuse_history_reset_pending_ = true;
    gi_reflection_history_reset_pending_ = true;
    gi_filtered_valid_ = false;
    dlss_history_reset_pending_ = true;
    temporal_history_changed_ = true;
}
