#pragma once

// MatterEngine3/include/matter/volumetric_quality.h
//
// Settings and sizing math for the froxel-based volumetric fog / cloud lighting
// pass. Two consumers share these types: the renderer
// (`src/render/vk_volumetrics.cpp`, which builds the froxel grid from them) and
// the editor's property panel (`MatterEditor/src/property_editor.cpp`, which
// binds them through matter::props and drives the quality presets).
//
// SPLIT ACROSS TWO HEADERS — read this before hunting for a definition. The
// three free functions declared at the bottom take a `CloudShadowSettings`,
// which lives in `matter/cloud_shadow_settings.h`. That header includes THIS
// one and defines all three inline, so:
//   * include volumetric_quality.h when you only need the settings struct or
//     the froxel sizing helpers;
//   * include cloud_shadow_settings.h when you actually call
//     enhanced_cloud_lighting / apply_volumetric_quality_preset /
//     identify_volumetric_quality_preset.
// The forward declaration below is what lets this header stay independent.
//
// Everything here is pure value math — no GPU objects, no allocation, no
// threading concerns. Lengths are metres; the froxel grid is measured in froxels
// (not pixels).

#include <cmath>
#include <cstdint>
#include <limits>

namespace matter {

// Defined in matter/cloud_shadow_settings.h; see the split note in the header.
struct CloudShadowSettings;

// Froxel-grid size selectors. These are INDEX enums: their values are positions
// in the lookup tables inside `resolve_froxel_grid`, not the multipliers or
// slice counts their names spell out. Both are explicitly `int32_t` because
// matter::props only binds 4-byte enums (see the static_assert in
// matter/props.h) — the editor exposes them as dropdowns.
enum class FroxelXyScale : int32_t { X0_5 = 0, X0_75, X1_0, X1_5, X2_0 };
enum class FroxelDepthSlices : int32_t { D64 = 0, D96, D128, D192, D256 };

// Resolved grid size in FROXELS (not pixels): width x height across the screen,
// depth along the view ray.
struct FroxelGridDimensions { uint32_t width, height, depth; };

// The volumetrics tunables, as one plain struct. This is the struct the props
// schema describes by byte offset and the renderer reads directly — it is a
// settings record, not renderer state, so copying it is free and comparing two
// of them is how `identify_volumetric_quality_preset` recognises a preset.
//
// The four `local_sun_march_*` / `multiple_scattering_*` / `powder_strength`
// fields are also the inputs to `enhanced_cloud_lighting`: any of them being
// active (or cloud shadows being enabled) switches the pass into its more
// expensive lighting path AND adds bytes per froxel — see
// `estimate_froxel_bytes`.
struct VulkanVolumetricsSettings {
    bool enabled = false;
    float temporal_blend = 0.85f;   // 0-1 weight on the reprojected history
    float phase_g = 0.3f;           // scattering anisotropy; 0 = isotropic
    float vol_debug_view = 0.0f;    // debug visualization selector
    FroxelXyScale froxel_xy_scale = FroxelXyScale::X1_0;
    FroxelDepthSlices froxel_depth_slices = FroxelDepthSlices::D128;
    int32_t local_sun_march_steps = 8;         // 0 disables the local sun march
    float local_sun_march_distance_m = 250.0f; // march length, metres
    int32_t multiple_scattering_orders = 2;    // >1 enables multi-scatter
    float multiple_scattering_strength = 0.55f;
    float powder_strength = 0.25f;             // 0 disables the powder term
};

// Named quality tiers. The first four are exact tuples of volumetrics + cloud
// shadow values that `apply_volumetric_quality_preset` writes.
//
// `Custom` is not a tuple and behaves asymmetrically, which is the thing to know
// here: applying it is a deliberate NO-OP (it leaves the current settings
// alone), and `identify_volumetric_quality_preset` returns it as the fallback
// when the live settings match none of the four. So "Custom" always means "these
// values are hand-tuned", never a preset you can restore.
enum class VolumetricQualityPreset : int32_t {
    CurrentCost = 0, Improved, High, Ultra, Custom
};

// The two size enums -> an actual froxel count. The XY grid is a 160x90 base
// scaled by the selected factor and rounded; depth comes straight from the slice
// table. An out-of-range enum value (a hand-edited JSON, a stale saved index)
// silently falls back to index 2 — the 1.0x / 128-slice default — rather than
// indexing past the tables.
inline FroxelGridDimensions resolve_froxel_grid(const VulkanVolumetricsSettings& settings) {
    constexpr float xy[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    constexpr uint32_t depth[] = {64, 96, 128, 192, 256};
    const int raw_xy = static_cast<int>(settings.froxel_xy_scale);
    const int raw_depth = static_cast<int>(settings.froxel_depth_slices);
    const int xy_index = raw_xy >= 0 && raw_xy < 5 ? raw_xy : 2;
    const int depth_index = raw_depth >= 0 && raw_depth < 5 ? raw_depth : 2;
    return {static_cast<uint32_t>(std::lround(160.0f * xy[xy_index])),
            static_cast<uint32_t>(std::lround(90.0f * xy[xy_index])), depth[depth_index]};
}

// Rough VRAM cost of the froxel volumes for a given grid: 32 bytes per froxel,
// plus 2 more when enhanced cloud lighting is on. An ESTIMATE for the UI's
// memory readout and for budget comparisons, not an allocation size. Every
// multiply saturates at UINT64_MAX, so an absurd grid reports "enormous" instead
// of wrapping to a small, plausible-looking number.
inline uint64_t estimate_froxel_bytes(FroxelGridDimensions dimensions, bool enhanced_clouds) {
    const auto saturating_multiply = [](uint64_t left, uint64_t right) {
        constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
        return left != 0 && right > maximum / left ? maximum : left * right;
    };
    const uint64_t voxels = saturating_multiply(
        saturating_multiply(static_cast<uint64_t>(dimensions.width), dimensions.height), dimensions.depth);
    return saturating_multiply(voxels, 4ull * 8ull + (enhanced_clouds ? 2ull : 0ull));
}

// ---------------------------------------------------------------------------
// Declared here, DEFINED in matter/cloud_shadow_settings.h (which includes this
// header). The declarations are `inline` to MATCH those definitions: a
// non-inline declaration followed by an inline definition made the symbol's
// linkage depend on whether the current translation unit had pulled in
// cloud_shadow_settings.h, so a TU that included only this header and called
// one of these got an undefined reference at link time — with nothing in the
// error pointing at the split. Include cloud_shadow_settings.h to call them.
//
//   enhanced_cloud_lighting            true when any of the local sun march,
//                                      multiple scattering, the powder term or
//                                      cloud shadows is active. Gates the
//                                      renderer's expensive lighting path and
//                                      the extra per-froxel bytes.
//   apply_volumetric_quality_preset    writes a preset's values into BOTH
//                                      structs; `Custom` writes nothing.
//   identify_volumetric_quality_preset the inverse: compares the live settings
//                                      against each preset and returns the match,
//                                      or `Custom`. Used to light up the right
//                                      button in the editor rather than to store
//                                      state.
// ---------------------------------------------------------------------------
inline bool enhanced_cloud_lighting(const VulkanVolumetricsSettings&,
                                    const CloudShadowSettings&);
inline void apply_volumetric_quality_preset(VolumetricQualityPreset,
                                            VulkanVolumetricsSettings&,
                                            CloudShadowSettings&);
inline VolumetricQualityPreset identify_volumetric_quality_preset(
    const VulkanVolumetricsSettings&, const CloudShadowSettings&);

} // namespace matter
