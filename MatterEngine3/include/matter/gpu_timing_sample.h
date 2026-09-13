#pragma once
#include <array>
#include <cstdint>

namespace matter {
// Append-only query indices, matching VkSceneRenderer. Unwritten/unavailable
// queries are NOT zero-duration measurements. Atmosphere uses a separate pool.
inline constexpr std::array<const char*, 25> kGpuTimingNames{{
    "total", "cull", "gbuffer", "blas", "tlas", "rt_sun_shadow",
    "denoise", "dlss", "composite", "volumetrics", "vt", "rt_gi",
    "atmosphere", "cloud_shadows", "vol_density", "vol_scatter",
    "vol_integrate", "water_decode", "water_forward", "water_direct_draw",
    "rt_local_direct", "hdr_lighting", "rt_gi_diffuse",
    "rt_gi_reflection_transmission", "primary_light_cull"}};
static_assert(kGpuTimingNames.size() <= 32, "GPU timing validity mask capacity");
// composite: final swapchain display transform; hdr_lighting: HDR reconstruction.
// rt_gi is the aggregate of all GI dispatches. Its two child zones are written
// only when diffuse and reflection/transmission execute at separate extents.
// Equal extents use one inseparable dispatch (rt_gi only); absent child queries
// stay unavailable, never synthetic zero measurements. Do not sum children
// with rt_gi. Reflection and transmission share a dispatch and cannot be split.
struct GpuTimingSample {
    uint64_t sequence = 0; // changes only on a fresh query-pool readback
    uint32_t valid_mask = 0; // both timestamps written and available
    std::array<float, kGpuTimingNames.size()> milliseconds{};
};
} // namespace matter
