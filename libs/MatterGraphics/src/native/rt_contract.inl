struct GpuRtCounters {
    uint32_t invalid_part_records;
    uint32_t any_hit_invocations;
    uint32_t any_hit_layers;
    uint32_t capped_rays;
};
static_assert(sizeof(GpuRtCounters) == 16);

struct alignas(16) VulkanGiAtrousConstants {
    uint32_t extent[2];
    uint32_t step_width;
    uint32_t signal_mode;
    uint32_t kernel_radius;
    float phi_luminance;
    float phi_depth;
    float normal_power;
    uint32_t pass_index;
    uint32_t pad[3];
};
static_assert(sizeof(VulkanGiAtrousConstants) == 48);

// Named constants for values that appear as literals in this file.
// kGiTestTemporalToken / kGiTestAtrousToken are sentinel serials injected by
// test-fixture helpers so the GI temporal and A-trous shaders can be exercised
// without a real swapchain frame.  The values are ASCII "TEMP" and "ATRO"
// respectively — chosen to be visually distinctive in a debugger.  They have
// no shader-side counterpart and do not need to match any GLSL literal.
constexpr uint32_t kGiTestTemporalToken = 0x54454d50u;  // ASCII "TEMP"
constexpr uint32_t kGiTestAtrousToken   = 0x4154524fu;  // ASCII "ATRO"
// GI temporal history length is stored as uint16_t; this cap prevents
// truncation when the CPU-side counter (uint32_t) exceeds the field width.
constexpr uint32_t kGiHistoryLengthMax  = 65535u;       // UINT16_MAX
// Vulkan instanceCustomIndex is a 24-bit field in the TLAS instance record;
// part_records must stay below this limit.  Must match the Vulkan spec
// (VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits).
constexpr uint32_t kTlasCustomIndexMax  = 1u << 24;

// "Opaque" here means a ray may stop at the first hit: the geometry gets
// VK_GEOMETRY_OPAQUE_BIT_KHR and TLAS instance mask 0x01, so no any-hit
// shader runs for it. Anything that can partially transmit -- alpha-tested
// cutouts, glass, thin-walled scatterers -- must land in the non-opaque layer
// (mask 0x02) instead, where rt_visibility.rahit attenuates rather than
// blocking. Getting this wrong is silent: the surface simply casts a solid
// shadow it should not have.
bool rt_material_is_opaque(const MaterialGpuRecord& material) {
    // Thin-walled scatterers must sit in the non-opaque TLAS layer even
    // though they author shadowOpacity = 1.0: a backlit blob's sun ray
    // starts inside its own geometry, and the opaque layer would
    // self-shadow it to black instead of attenuating (rt_visibility.rahit).
    const bool thin_scattering =
        (material.flags_misc[0] & MATERIAL_THIN_WALLED) != 0u &&
        material.scattering[3] > 0.0f;
    return material.metal_opacity_spec_coat[1] >= 1.0f &&
           material.scattering_shape[3] >= 1.0f &&
           (material.flags_misc[0] & MATERIAL_ALPHA_TESTED) == 0u &&
           material.transmission[0] <= 0.0f && !thin_scattering;
}

bool rt_material_ids_are_opaque(
    const std::vector<MaterialGpuRecord>& materials,
    const std::vector<uint32_t>& material_ids) {
    return std::all_of(material_ids.begin(), material_ids.end(),
                       [&](uint32_t material_id) {
                           return material_id < materials.size() &&
                                  rt_material_is_opaque(
                                      materials[material_id]);
                       });
}
