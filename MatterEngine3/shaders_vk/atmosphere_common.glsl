#ifndef MATTER_ATMOSPHERE_COMMON_GLSL
#define MATTER_ATMOSPHERE_COMMON_GLSL

const float ATM_PLANET_RADIUS = 6360000.0;
const float ATM_TOP_RADIUS = 6460000.0;
const float ATM_RAYLEIGH_HEIGHT = 8000.0;
const float ATM_MIE_HEIGHT = 1200.0;
const float ATM_OZONE_CENTER = 25000.0;
const float ATM_OZONE_HALF_WIDTH = 15000.0;
const vec3 ATM_RAYLEIGH = vec3(5.802e-6, 13.558e-6, 33.100e-6);
const vec3 ATM_MIE_SCATTER = vec3(3.996e-6);
const vec3 ATM_MIE_EXTINCTION = vec3(4.440e-6);
const vec3 ATM_OZONE = vec3(0.650e-6, 1.881e-6, 0.085e-6);
const vec3 ATM_EXTRATERRESTRIAL_SOLAR_RGB = vec3(1.0, 1.0, 1.0);
const float ATM_PI = 3.14159265358979323846;

layout(push_constant) uniform AtmospherePush {
    vec4 settings0; // rayleigh, mie, anisotropy, ground albedo
    vec4 settings1; // sea_level_y, ozone, camera_world_y, pad
    vec4 to_sun;
} atmosphere;

float atmosphere_exit(vec3 origin, vec3 direction, float radius) {
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    float root = sqrt(d);
    float near_t = -b - root;
    float far_t = -b + root;
    return far_t > 0.0 ? (near_t > 0.0 ? near_t : far_t) : -1.0;
}

float atmosphere_density_rayleigh(float height_m) { return exp(-max(height_m, 0.0) / ATM_RAYLEIGH_HEIGHT); }
float atmosphere_density_mie(float height_m) { return exp(-max(height_m, 0.0) / ATM_MIE_HEIGHT); }
float atmosphere_density_ozone(float height_m) {
    return max(0.0, 1.0 - abs(height_m - ATM_OZONE_CENTER) / ATM_OZONE_HALF_WIDTH);
}

vec3 atmosphere_extinction(float height_m) {
    return ATM_RAYLEIGH * atmosphere.settings0.x * atmosphere_density_rayleigh(height_m) +
           ATM_MIE_EXTINCTION * atmosphere.settings0.y * atmosphere_density_mie(height_m) +
           ATM_OZONE * atmosphere.settings1.y * atmosphere_density_ozone(height_m);
}

vec3 atmosphere_transmittance(vec3 origin, vec3 direction, int samples, out bool planet_hit) {
    float top_t = atmosphere_exit(origin, direction, ATM_TOP_RADIUS);
    float ground_t = atmosphere_exit(origin, direction, ATM_PLANET_RADIUS);
    planet_hit = ground_t > 0.0 && ground_t < top_t;
    if (planet_hit || top_t <= 0.0) return vec3(0.0);
    float step_t = top_t / float(samples);
    vec3 optical_depth = vec3(0.0);
    for (int step_index = 0; step_index < samples; ++step_index) {
        vec3 point = origin + direction * ((float(step_index) + 0.5) * step_t);
        float height_m = length(point) - ATM_PLANET_RADIUS;
        optical_depth += atmosphere_extinction(height_m) * step_t;
    }
    return exp(-max(optical_depth, vec3(0.0)));
}

vec3 atmosphere_segment_transmittance(vec3 origin, vec3 direction,
                                      float distance_t, int samples) {
    vec3 optical_depth = vec3(0.0);
    float step_t = distance_t / float(samples);
    for (int step_index = 0; step_index < samples; ++step_index) {
        vec3 point = origin + direction * ((float(step_index) + 0.5) * step_t);
        optical_depth += atmosphere_extinction(length(point) - ATM_PLANET_RADIUS) * step_t;
    }
    return exp(-max(optical_depth, vec3(0.0)));
}

float rayleigh_phase(float cosine) { return 3.0 * (1.0 + cosine * cosine) / (16.0 * ATM_PI); }
float mie_phase(float cosine) {
    float g = clamp(atmosphere.settings0.z, -0.99, 0.99);
    float denominator = max(1.0 + g * g - 2.0 * g * cosine, 1e-4);
    return 3.0 * (1.0 - g * g) * (1.0 + cosine * cosine) /
           (8.0 * ATM_PI * (2.0 + g * g) * pow(denominator, 1.5));
}

#endif
