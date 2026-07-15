#pragma once
#include "matter/world_session.h"

namespace viewer {
matter::VulkanLightingOverrides sanitize_vulkan_lighting_overrides(
    const matter::VulkanLightingOverrides& value) noexcept;
float vulkan_exposure_scale(float exposure_ev) noexcept;
bool vulkan_source_lighting_changed(
    const matter::VulkanLightingOverrides& a,
    const matter::VulkanLightingOverrides& b) noexcept;
}
