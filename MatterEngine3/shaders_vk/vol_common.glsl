// vol_common.glsl -- shared constants and utilities for volumetric shaders.
// No layout/binding declarations; include after #version and #extension lines.

// View-space froxel coverage must be large enough for streamed landscapes.
// Keep shadow rays separate: tracing every froxel several kilometres through
// a dense terrain TLAS is unnecessary and can trigger severe GPU stalls.
const float VOL_FROXEL_FAR = 3000.0;
const float VOL_SHADOW_FAR = 300.0;
const float VOL_NEAR = 0.1;

// Exponential slice-to-depth mapping: concentrates precision near the camera.
// slice in [0, depth_slices], depth in [VOL_NEAR, VOL_FROXEL_FAR].
float slice_to_depth(float slice_index, float depth_slices) {
    float t = slice_index / max(depth_slices, 1.0);
    return VOL_NEAR * pow(VOL_FROXEL_FAR / VOL_NEAR, t);
}

// Inverse: depth to normalized slice [0, 1].
float depth_to_slice_n(float depth, float depth_slices) {
    float clamped = clamp(depth, VOL_NEAR, VOL_FROXEL_FAR);
    return log(clamped / VOL_NEAR) /
           log(VOL_FROXEL_FAR / VOL_NEAR);
}

// Henyey-Greenstein phase function.
// cos_theta: dot(view_dir, light_dir), g: asymmetry parameter [-1, 1].
float hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

// Cloud-only dual lobe: a strong forward lobe preserves silver linings while
// the small backward lobe keeps the dark side from collapsing to black.
float cloud_phase(float mu, float anisotropy_scale) {
    return 0.8 * hg_phase(mu, 0.85 * anisotropy_scale) +
           0.2 * hg_phase(mu, -0.30 * anisotropy_scale);
}

// Must match C++ GpuCloudLayer in matter/cloud_layers.h (96 bytes std430).
// All-float, like GpuVolumeEmitter below and for the same reason: the struct
// alignment stays 4 and the std430 array stride is exactly sizeof, so the C++
// array and this one agree without invoking any padding rule.
//
// `octaves` and `seed` are integral values carried as floats; the shader
// converts with int()/uint(). Both are small and exactly representable.
struct GpuCloudLayer {
    float min_height;
    float max_height;
    float falloff_min;
    float falloff_max;

    float max_density;
    float noise_scale;
    float coverage;
    float lacunarity;

    float gain;
    float octaves;
    float seed;
    float pad0;

    float wind[3];
    float pad1;

    // weather_scale, weather_influence, detail_scale, detail_erosion.
    vec4 weather_scale_influence_detail_scale_detail_erosion;
    // shape_bias followed by three zero padding lanes.
    vec4 shape_bias_padding;
};

// Half-width of the coverage threshold's soft shoulder, in units of the
// normalized noise field. Mirrors kCloudCoverageEdge in cloud_layers.h.
const float CLOUD_COVERAGE_EDGE = 0.12;

// Must match C++ GpuVolumeEmitter in vk_emitter_gather.h (64 bytes std430).
// Note: the C++ field `length` is named `axial_len` here because GLSL
// reserves `.length` for the built-in length() method on member access.
struct GpuVolumeEmitter {
    float world_pos[3];
    float radius;
    float world_dir[3];
    float spread;
    float axial_len;
    float density;
    float rise;
    float turbulence;
    float color[3];
    float pad;
};
