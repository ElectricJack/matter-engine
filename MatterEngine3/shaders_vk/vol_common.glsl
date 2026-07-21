// vol_common.glsl -- shared constants and utilities for volumetric shaders.
// No layout/binding declarations; include after #version and #extension lines.

const uint VOL_W = 160;
const uint VOL_H = 90;
const uint VOL_D = 128;
const float VOL_FAR = 300.0;
const float VOL_NEAR = 0.1;

// Exponential slice-to-depth mapping: concentrates precision near the camera.
// slice in [0, VOL_D], depth in [VOL_NEAR, VOL_FAR].
float slice_to_depth(float slice) {
    float t = slice / float(VOL_D);
    return VOL_NEAR * pow(VOL_FAR / VOL_NEAR, t);
}

// Inverse: depth to normalized slice [0, 1].
float depth_to_slice_n(float depth) {
    float clamped = clamp(depth, VOL_NEAR, VOL_FAR);
    return log(clamped / VOL_NEAR) / log(VOL_FAR / VOL_NEAR);
}

// Henyey-Greenstein phase function.
// cos_theta: dot(view_dir, light_dir), g: asymmetry parameter [-1, 1].
float hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

// Must match C++ GpuVolumeEmitter in vk_emitter_gather.h (64 bytes std430).
struct GpuVolumeEmitter {
    float world_pos[3];
    float radius;
    float world_dir[3];
    float spread;
    float length;
    float density;
    float rise;
    float turbulence;
    float color[3];
    float pad;
};
