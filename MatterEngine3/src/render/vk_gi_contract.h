#pragma once

#include "material_registry.h"

#include <stdint.h>

typedef struct VulkanGiSettings {
#ifdef __cplusplus
    uint32_t enabled = 1;
    uint32_t max_bounces = 1;
    uint32_t samples_per_pixel = 1;
    uint32_t denoiser_iterations = 0;
    float trace_scale = 1.0f;
    float diffuse_multiplier = 1.0f;
    float reflection_multiplier = 1.0f;
    float max_reflection_roughness = 1.0f;
    float transmission_multiplier = 1.0f;
    float scattering_multiplier = 1.0f;
#else
    uint32_t enabled;
    uint32_t max_bounces;
    uint32_t samples_per_pixel;
    uint32_t denoiser_iterations;
    float trace_scale;
    float diffuse_multiplier;
    float reflection_multiplier;
    float max_reflection_roughness;
    float transmission_multiplier;
    float scattering_multiplier;
#endif
} VulkanGiSettings;

typedef struct VulkanGiCounters {
    uint64_t shadow_rays;
    uint64_t diffuse_rays;
    uint64_t specular_rays;
    uint64_t transmission_rays;
    uint64_t scattering_rays;
    uint64_t ao_rays;
    uint64_t any_hit_layers;
} VulkanGiCounters;

enum VulkanGiTemporalRejectionBits {
    VULKAN_GI_REJECT_BOUNDS = 1u << 0,
    VULKAN_GI_REJECT_DEPTH = 1u << 1,
    VULKAN_GI_REJECT_NORMAL = 1u << 2,
    VULKAN_GI_REJECT_MATERIAL = 1u << 3,
    VULKAN_GI_REJECT_INSTANCE = 1u << 4,
    VULKAN_GI_REJECT_RESET = 1u << 5,
    VULKAN_GI_REJECT_HIT_DISTANCE = 1u << 6,
};

typedef struct VulkanGiTemporalConstants {
    uint32_t temporal_extent[2];
    uint32_t gbuffer_extent[2];
    uint32_t reset;
    uint32_t attempt_token_lo;
    uint32_t presented_attempt_token_lo;
    uint32_t signal_mode;
} VulkanGiTemporalConstants;

#ifdef __cplusplus
typedef struct alignas(16) GpuRtPartRecord {
#else
typedef struct GpuRtPartRecord {
#endif
    uint64_t vertex_address;    // part-base VkRasterVertex buffer address
    uint64_t index_address;     // this BLAS's first index (base + first_index*4)
    uint32_t vertex_stride;     // 72
    uint32_t vertex_count;      // part unique-vertex count
    uint32_t primitive_count;   // index_count / 3 for this BLAS
    uint32_t valid;
    uint32_t pad0, pad1, pad2, pad3;
} GpuRtPartRecord;              // 48 bytes: "three vec4 records"

#ifdef __cplusplus
#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout<MaterialGpuRecord>::value,
              "MaterialGpuRecord must remain a standard-layout GPU record");
static_assert(sizeof(MaterialGpuRecord) == 144,
              "MaterialGpuRecord must remain exactly nine vec4 records");
static_assert(offsetof(MaterialGpuRecord, base_roughness) == 0,
              "MaterialGpuRecord base_roughness must be vec4 0");
static_assert(offsetof(MaterialGpuRecord, metal_opacity_spec_coat) == 16,
              "MaterialGpuRecord metal_opacity_spec_coat must be vec4 1");
static_assert(offsetof(MaterialGpuRecord, specular_tint_coat_roughness) == 32,
              "MaterialGpuRecord specular_tint_coat_roughness must be vec4 2");
static_assert(offsetof(MaterialGpuRecord, emission_strength) == 48,
              "MaterialGpuRecord emission_strength must be vec4 3");
static_assert(offsetof(MaterialGpuRecord, transmission) == 64,
              "MaterialGpuRecord transmission must be vec4 4");
static_assert(offsetof(MaterialGpuRecord, absorption_pad) == 80,
              "MaterialGpuRecord absorption_pad must be vec4 5");
static_assert(offsetof(MaterialGpuRecord, scattering) == 96,
              "MaterialGpuRecord scattering must be vec4 6");
static_assert(offsetof(MaterialGpuRecord, scattering_shape) == 112,
              "MaterialGpuRecord scattering_shape must be vec4 7");
static_assert(offsetof(MaterialGpuRecord, flags_misc) == 128,
              "MaterialGpuRecord flags_misc must be uvec4 8");
// flags_misc[0]: MaterialSurfaceFlags bitmask (unchanged).
// flags_misc[1]: schema v4 / Vulkan tileset (spec "Material schema", Phase 1
//   Task 8): packs the ground detail slot (low byte) and the ground macro
//   slot (next byte, Phase 3) as (slot + 1), so -1 (untextured / no macro)
//   encodes as 0 in that byte:
//     flags_misc[1] = uint32(detailSlot + 1) | (uint32(macroSlot + 1) << 8)
//   Packed by MaterialRegistryPackRtForGPU / MaterialPackDetailMacroSlots
//   (MatterSurfaceLib/include/material_registry.h) from MaterialDef's
//   groundTilesetSlot ("detail slot", runtime override via
//   MaterialRegistrySetGroundTilesetSlot) and groundMacroSlot (runtime
//   override via MaterialRegistrySetGroundMacroSlot). Decoded on the shader
//   side by shaders_vk/tileset_common.glsl's tileset_detail_slot()/
//   tileset_macro_slot(). The GL raster path never reads flags_misc at all
//   (MaterialRegistryPackForGPU's separate 12-float table is frozen and
//   carries only the detail slot, as a plain float, in slot [11]).
// flags_misc[2], flags_misc[3]: unused (reserved), always 0.
static_assert(std::is_standard_layout<GpuRtPartRecord>::value,
              "GpuRtPartRecord must remain a standard-layout GPU record");
static_assert(sizeof(GpuRtPartRecord) == 48,
              "GpuRtPartRecord must remain exactly three vec4 records");
static_assert(sizeof(VulkanGiTemporalConstants) == 32,
              "GI temporal push constants must remain two uvec4 records");

namespace matter {
using VulkanGiSettings = ::VulkanGiSettings;
using VulkanGiCounters = ::VulkanGiCounters;
}
#endif
