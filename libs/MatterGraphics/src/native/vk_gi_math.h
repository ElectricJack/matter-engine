#pragma once

// MatterEngine3/src/render/vk_gi_math.h
//
// CPU mirrors of the sampling and BRDF math the ray-traced GI shaders run:
// the PCG hash and per-pixel seed, the launch-grid -> source-image UV mapping,
// cosine-hemisphere sampling, Schlick Fresnel, the GGX reflection pdf, and the
// clearcoat lobe selection probability.
//
// These are deliberately Vulkan-free and side-effect-free so the identities
// that matter can be checked on the CPU: that the seed varies with pixel,
// frame and bounce; that the cosine pdf and the GGX pdf integrate the way the
// estimator assumes; that a UV maps into range. In the current tree the only
// callers are MatterEngine3/tests/vulkan_smoke_tests.cpp — the shipped GI path
// runs the GLSL versions in MatterEngine3/shaders_vk/, so changing a formula
// here without changing the shader silently breaks the parity these functions
// exist to assert.
//
// Conventions: all directions are unit vectors in world space; all cosines are
// expected already-clamped-ish and are re-clamped defensively; pdfs are with
// respect to solid angle.

#include <cstdint>

#include "matter/math_types.h"

namespace viewer {

// One cosine-hemisphere sample: a unit direction in the hemisphere around the
// shading normal, plus its solid-angle pdf (cos(theta)/pi). Default-constructed
// is not a valid sample — pdf 0 means "nothing sampled".
struct VulkanCosineSample {
    matter::Float3 direction{};
    float pdf = 0.0f;
};

// Normalized texture coordinates in [0,1], addressing the centre of a source
// texel. Both zero is also what the out-of-range guard returns.
struct VulkanGiUv {
    float x = 0.0f;
    float y = 0.0f;
};

// PCG-style integer hash: cheap, well distributed, and identical to the GLSL
// version so a CPU repro of a GI pixel draws the same numbers as the GPU.
uint32_t vulkan_gi_pcg_hash(uint32_t value) noexcept;
// Per-sample seed. Decorrelates across pixel, frame and bounce, so two pixels
// of one frame, one pixel across frames, and successive bounces of one path
// all get different sequences. `presented_frame_index` is the frame counter the
// temporal accumulation keys off — pass the same value the GI pass used, or
// the CPU and GPU sequences diverge.
uint32_t vulkan_gi_seed(uint32_t pixel_x, uint32_t pixel_y,
                        uint32_t presented_frame_index,
                        uint32_t bounce) noexcept;
// Maps a launch-grid pixel (raw_x, raw_y in a raw_width x raw_height dispatch)
// to the UV of the containing source texel's CENTRE, for the common case where
// GI traces at a lower resolution than the G-buffer it reads. Nearest, not
// bilinear: the result is snapped to a source texel so every launch in a
// footprint reads exactly the same sample. Returns {0,0} if any extent is zero.
VulkanGiUv vulkan_gi_source_uv(uint32_t raw_x, uint32_t raw_y,
                               uint32_t raw_width, uint32_t raw_height,
                               uint32_t source_width,
                               uint32_t source_height) noexcept;
// Cosine-weighted hemisphere sample about `normal`, from two uniform randoms
// in [0,1] (clamped internally). `normal` need not be normalized; a degenerate
// one falls back to +Y rather than producing NaNs.
VulkanCosineSample vulkan_cosine_sample(matter::Float3 normal, float u1,
                                        float u2) noexcept;
// Schlick's Fresnel approximation, per linear-RGB channel. `f0` is reflectance
// at normal incidence; `view_half_cosine` is dot(V, H) — the HALF vector, not
// the normal.
matter::Float3 vulkan_schlick_fresnel(matter::Float3 f0,
                                      float view_half_cosine) noexcept;
// Solid-angle pdf of a GGX/Trowbridge-Reitz half-vector sample already
// converted to the reflected direction (the 1/(4 dot(V,H)) Jacobian is
// included). Cosines are dot(N,H) and dot(V,H); `roughness` is perceptual, so
// alpha = roughness^2, floored so a mirror-smooth surface returns a large but
// finite pdf instead of dividing by zero.
float vulkan_ggx_reflection_pdf(float normal_half_cosine,
                                float view_half_cosine,
                                float roughness) noexcept;
// Probability of picking the clearcoat lobe instead of the base lobe for a
// given clearcoat weight in [0,1]: w/(1+w), so it never reaches 1 and the base
// lobe is always still sampled.
float vulkan_clearcoat_selection_probability(float clearcoat) noexcept;

}  // namespace viewer
