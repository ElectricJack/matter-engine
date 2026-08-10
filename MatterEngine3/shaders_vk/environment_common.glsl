#ifndef MATTER_ENVIRONMENT_COMMON_GLSL
#define MATTER_ENVIRONMENT_COMMON_GLSL

// Stable physical-environment ABI.  All raster, RT, and froxel consumers bind
// this identical set at set 1; the neutral cloud fields make the declaration
// valid before cloud-shadow production exists.
#ifdef MATTER_ENVIRONMENT_SAMPLING_TEST
layout(set = 0, binding = 0) uniform sampler2D atmosphere_sky_view;
#else
layout(set = 1, binding = 0) uniform sampler2D atmosphere_sky_view;
layout(set = 1, binding = 1) uniform sampler2D atmosphere_irradiance_sh;
layout(set = 1, binding = 2) uniform sampler3D cloud_shadow_near_0;
layout(set = 1, binding = 3) uniform sampler3D cloud_shadow_near_1;
layout(set = 1, binding = 4) uniform sampler3D cloud_shadow_far_0;
layout(set = 1, binding = 5) uniform sampler3D cloud_shadow_far_1;
layout(set = 1, binding = 6, std140) uniform EnvironmentBlock {
    mat4 cloud_world_to_uvw[2];
    vec4 cloud_state;
    vec4 cloud_filter;
    vec4 direct_world_sun_ratio;
    vec4 sun_disc_reserved;
    vec4 sky_display_reserved;
    vec4 sky_irradiance_ambient_ratio;
} environment;

#include "cloud_shadow_common.glsl"

float sample_cloud_transmittance(vec3 world_pos, float receiver_distance_m) {
    // The disabled/failure path is independent of transforms and image
    // contents. Active sampling remains binding-free in cloud_shadow_common.
    if (environment.cloud_state.x == 0.0 ||
        isnan(environment.cloud_state.x) ||
        isinf(environment.cloud_state.x)) return 1.0;
    return cloud_shadow_sample_active(world_pos, receiver_distance_m);
}
#endif

const float ENV_PI = 3.14159265359;

vec2 atmosphere_sky_uv(vec3 world_dir, vec3 to_sun) {
    vec3 direction = normalize(world_dir);
    vec3 sun = normalize(to_sun);
    vec3 sun_horizontal = sun - vec3(0.0, sun.y, 0.0);
    vec3 forward = length(sun_horizontal) > 1e-4
        ? normalize(sun_horizontal) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), forward));
    vec3 horizontal = direction - vec3(0.0, direction.y, 0.0);
    float azimuth = length(horizontal) > 1e-5
        ? atan(dot(normalize(horizontal), right),
               dot(normalize(horizontal), forward)) : 0.0;
    float zenith = acos(clamp(direction.y, -1.0, 1.0));
    float azimuth_u = (azimuth + ENV_PI) / (2.0 * ENV_PI);
    float v = zenith / ENV_PI;
    return vec2(fract(azimuth_u), clamp(v, 0.5 / 108.0, 107.5 / 108.0));
}

#ifndef MATTER_ENVIRONMENT_SAMPLING_TEST
vec3 sample_physical_sky(vec3 world_dir, vec3 to_sun) {
    return texture(atmosphere_sky_view,
                   atmosphere_sky_uv(world_dir, to_sun)).rgb *
           environment.sky_display_reserved.rgb;
}

float environment_sh_basis(int index, vec3 d) {
    if (index == 0) return 0.282095;
    if (index == 1) return 0.488603 * d.y;
    if (index == 2) return 0.488603 * d.z;
    if (index == 3) return 0.488603 * d.x;
    if (index == 4) return 1.092548 * d.x * d.y;
    if (index == 5) return 1.092548 * d.y * d.z;
    if (index == 6) return 0.315392 * (3.0 * d.z * d.z - 1.0);
    if (index == 7) return 1.092548 * d.x * d.z;
    return 0.546274 * (d.x * d.x - d.y * d.y);
}

vec3 sample_sky_irradiance(vec3 normal) {
    vec3 n = normalize(normal);
    vec3 irradiance = vec3(0.0);
    for (int coefficient = 0; coefficient < 9; ++coefficient) {
        // Stored coefficients are radiance SH.  Apply Lambertian cosine
        // convolution once: l=0 pi, l=1 2pi/3, l=2 pi/4.
        float band = coefficient == 0 ? ENV_PI
                   : coefficient < 4 ? 2.0 * ENV_PI / 3.0
                                     : ENV_PI / 4.0;
        irradiance += texelFetch(atmosphere_irradiance_sh,
                                 ivec2(coefficient % 3, coefficient / 3), 0).rgb *
                      environment_sh_basis(coefficient, n) * band;
    }
    return max(irradiance, vec3(0.0)) *
           environment.sky_irradiance_ambient_ratio.rgb;
}
#endif

#endif
