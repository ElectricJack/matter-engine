#ifndef MATTER_CLOUD_SHADOW_COMMON_GLSL
#define MATTER_CLOUD_SHADOW_COMMON_GLSL

// Binding-free Task 10 sampling contract. environment_common.glsl owns all
// declarations; this file only consumes its stable set-1 ABI.
bool cloud_shadow_finite(float value) {
    return !isnan(value) && !isinf(value);
}

bool cloud_shadow_inside(vec3 uvw) {
    return all(not(isnan(uvw))) && all(not(isinf(uvw))) &&
           all(greaterThanEqual(uvw, vec3(0.0))) &&
           all(lessThanEqual(uvw, vec3(1.0)));
}

float cloud_shadow_edge_fade(vec3 uvw) {
    float edge_distance = min(min(uvw.x, uvw.y),
                              min(1.0 - uvw.x, 1.0 - uvw.y));
    return smoothstep(0.0, 0.08, edge_distance);
}

float cloud_shadow_tau_fetch(int level, int ping, vec3 uvw) {
    float tau = 0.0;
    if (level == 0) {
        tau = ping == 0 ? texture(cloud_shadow_near_0, uvw).r
                        : texture(cloud_shadow_near_1, uvw).r;
    } else {
        tau = ping == 0 ? texture(cloud_shadow_far_0, uvw).r
                        : texture(cloud_shadow_far_1, uvw).r;
    }
    return cloud_shadow_finite(tau) ? clamp(tau, 0.0, 80.0) : 0.0;
}

ivec3 cloud_shadow_texture_size(int level, int ping) {
    if (level == 0)
        return ping == 0 ? textureSize(cloud_shadow_near_0, 0)
                         : textureSize(cloud_shadow_near_1, 0);
    return ping == 0 ? textureSize(cloud_shadow_far_0, 0)
                     : textureSize(cloud_shadow_far_1, 0);
}

float cloud_shadow_filtered_tau(int level, vec3 uvw,
                                float receiver_distance_m) {
    int ping = level == 0 ? int(environment.cloud_state.y + 0.5)
                          : int(environment.cloud_state.z + 0.5);
    ping = clamp(ping, 0, 1);
    float voxel_size = level == 0 ? environment.cloud_filter.x
                                  : environment.cloud_filter.y;
    float radius = 0.0;
    if (cloud_shadow_finite(environment.cloud_filter.z) &&
        cloud_shadow_finite(environment.cloud_filter.w) &&
        cloud_shadow_finite(receiver_distance_m) &&
        cloud_shadow_finite(voxel_size) && receiver_distance_m > 0.0 &&
        voxel_size > 0.0 && environment.cloud_filter.z > 0.0 &&
        environment.cloud_filter.w > 0.0) {
        radius = clamp(environment.cloud_filter.z * receiver_distance_m /
                       voxel_size * environment.cloud_filter.w, 0.0, 4.0);
    }
    vec2 texel = 1.0 / vec2(cloud_shadow_texture_size(level, ping).xy);
    vec2 offset = texel * radius;
    float tau = cloud_shadow_tau_fetch(level, ping, uvw);
    tau += cloud_shadow_tau_fetch(level, ping,
                                  uvw + vec3(offset.x, 0.0, 0.0));
    tau += cloud_shadow_tau_fetch(level, ping,
                                  uvw - vec3(offset.x, 0.0, 0.0));
    tau += cloud_shadow_tau_fetch(level, ping,
                                  uvw + vec3(0.0, offset.y, 0.0));
    tau += cloud_shadow_tau_fetch(level, ping,
                                  uvw - vec3(0.0, offset.y, 0.0));
    return clamp(tau * 0.2, 0.0, 80.0);
}

float cloud_shadow_tau_level(int level, vec3 uvw,
                             float receiver_distance_m) {
    if (!cloud_shadow_inside(uvw)) return 0.0;
    float tau = cloud_shadow_filtered_tau(level, uvw, receiver_distance_m);
    if (!cloud_shadow_finite(tau)) return 0.0;
    return clamp(tau * cloud_shadow_edge_fade(uvw), 0.0, 80.0);
}

float cloud_shadow_sample_active(vec3 world_pos, float receiver_distance_m) {
    vec3 near_uvw = (environment.cloud_world_to_uvw[0] *
                     vec4(world_pos, 1.0)).xyz;
    vec3 far_uvw = (environment.cloud_world_to_uvw[1] *
                    vec4(world_pos, 1.0)).xyz;
    float far_tau = cloud_shadow_tau_level(1, far_uvw, receiver_distance_m);
    float tau = far_tau;
    if (cloud_shadow_inside(near_uvw)) {
        float near_weight = cloud_shadow_edge_fade(near_uvw);
        float near_tau = cloud_shadow_tau_level(
            0, near_uvw, receiver_distance_m);
        tau = mix(far_tau, near_tau, near_weight);
    }
    if (!cloud_shadow_finite(tau)) return 1.0;
    return clamp(exp(-clamp(tau, 0.0, 80.0)), 0.0, 1.0);
}

#endif
