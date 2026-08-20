#pragma once

// MatterEngine3/src/render/vk_gi_contract.h
//
// The CPU<->GPU contract for the ray-traced GI path. Everything here is either
// a knob the renderer sets per frame or a record whose byte layout a shader
// depends on; the static_asserts at the bottom are the enforcement, and they
// are the reason this is a header rather than fields on the renderer.
//
// Deliberately C-compatible (plain typedef structs, default member
// initializers hidden behind __cplusplus) so it can be included from either
// side. Consumed by MatterEngine3/src/render/vk_scene_renderer.{h,cpp}, and
// re-exported through matter/world_session.h, where RenderOptions::vulkan_gi
// carries VulkanGiSettings from the editor into the renderer.
//
// Two records have hard GPU-side layout requirements: MaterialGpuRecord (nine
// vec4s, defined in libs/MatterSurfaceLib/include/material_registry.h, its
// field offsets pinned below) and GpuRtPartRecord (three vec4s). Changing
// either means changing the matching GLSL in MatterEngine3/shaders_vk/ in the
// same commit.

#include "material_registry.h"

#include <stdint.h>

// Per-frame GI knobs, set by the editor/property system and read by the
// renderer; the defaults below are the "GI on, cheapest useful configuration"
// baseline. The multipliers are dimensionless scales on their respective
// contributions (1.0 = physical), max_reflection_roughness is the roughness
// above which reflections stop being traced, and trace_scale is the GI trace
// resolution as a fraction of the G-buffer (1.0 = full rate).
// denoiser_iterations 0 means no denoise pass.
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

// Per-frame ray budget accounting, one counter per ray kind, reported for the
// perf overlays. Counts, not times. `any_hit_layers` counts any-hit shader
// invocations (alpha-tested layers traversed) rather than rays cast, so it can
// far exceed the ray counts on foliage.
typedef struct VulkanGiCounters {
    uint64_t shadow_rays;
    uint64_t diffuse_rays;
    uint64_t specular_rays;
    uint64_t transmission_rays;
    uint64_t scattering_rays;
    uint64_t ao_rays;
    uint64_t any_hit_layers;
} VulkanGiCounters;

// Why a pixel's temporal history was thrown away this frame, as a bitmask —
// several reasons can fire at once. Used both by the shader that rejects and
// by the debug view that visualizes rejection, so the bit values are part of
// the GPU contract:
//   BOUNDS         reprojected outside the previous frame
//   DEPTH/NORMAL   surface moved or turned too far to be the same sample
//   MATERIAL       a different material landed on the pixel
//   INSTANCE       a different TLAS instance landed on the pixel
//   RESET          history discarded wholesale (see the `reset` push constant)
//   HIT_DISTANCE   the traced hit distance disagrees with the history's
enum VulkanGiTemporalRejectionBits {
    VULKAN_GI_REJECT_BOUNDS = 1u << 0,
    VULKAN_GI_REJECT_DEPTH = 1u << 1,
    VULKAN_GI_REJECT_NORMAL = 1u << 2,
    VULKAN_GI_REJECT_MATERIAL = 1u << 3,
    VULKAN_GI_REJECT_INSTANCE = 1u << 4,
    VULKAN_GI_REJECT_RESET = 1u << 5,
    VULKAN_GI_REJECT_HIT_DISTANCE = 1u << 6,
};

// Push constants for the GI temporal pass. Exactly two uvec4s (asserted
// below), so any new field has to displace an existing one.
//
// The two extents are in PIXELS and are not the same resolution: the temporal
// history is traced at trace_scale of the G-buffer, so the shader needs both
// to map between them. `reset` non-zero discards all history for this frame.
typedef struct VulkanGiTemporalConstants {
    uint32_t temporal_extent[2];   // GI/temporal history resolution, pixels
    uint32_t gbuffer_extent[2];    // full G-buffer resolution, pixels
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
    // WP-G (chart VT in the RT path): the transported VT slot (layer + 1;
    // vt::kVtNoSlot == 0 means "this BLAS's rung has no chart table") for the
    // exact (cluster, LOD) this TLAS instance traces. Resolved from the SAME
    // (part.vt_slots, VkSceneLod::chart_rung) mapping that feeds cull.comp's
    // vt_draw_slots table, so a ray hit and a raster fragment on the same
    // rung address the same indirection layer -- see
    // VkSceneRenderer::vt_slot_for_lod(). Occupies the former pad0; the
    // record is still exactly three vec4s and the shader-side stride guard
    // (vertex_stride != 72) is untouched.
    uint32_t vt_slot;
    uint32_t pad1, pad2, pad3;
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
//   (libs/MatterSurfaceLib/include/material_registry.h) from MaterialDef's
//   groundTilesetSlot ("detail slot", runtime override via
//   MaterialRegistrySetGroundTilesetSlot) and groundMacroSlot. The macro slot
//   has NO runtime override any more: the symmetric
//   MaterialRegistrySetGroundMacroSlot() was deleted as unreachable in
//   e7c19aae, so groundMacroSlot is whatever the material authored. Decoded on
//   the shader side by shaders_vk/tileset_common.glsl's tileset_detail_slot()/
//   tileset_macro_slot(). The frozen legacy 12-float table
//   (MaterialRegistryPackForGPU) does not carry flags_misc at all -- it has
//   only the detail slot, as a plain float, in slot [11].
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
