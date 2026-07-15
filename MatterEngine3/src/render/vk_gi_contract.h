#pragma once

#include "material_registry.h"

#include <stdint.h>

typedef struct VulkanGiSettings {
    uint32_t enabled;
    uint32_t max_bounces;
    uint32_t samples_per_pixel;
    uint32_t denoiser_iterations;
    float trace_scale;
    float diffuse_multiplier;
    float reflection_multiplier;
    float transmission_multiplier;
    float scattering_multiplier;
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

#ifdef __cplusplus
#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout<MaterialGpuRecord>::value,
              "MaterialGpuRecord must remain a standard-layout GPU record");
static_assert(sizeof(MaterialGpuRecord) == 144,
              "MaterialGpuRecord must remain exactly nine vec4 records");
static_assert(offsetof(MaterialGpuRecord, flags_misc) == 128,
              "MaterialGpuRecord flags must occupy the ninth vec4");
#endif
