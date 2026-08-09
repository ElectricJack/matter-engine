// Binding-free shared cloud density. cloud_layer_tests.cpp protects the
// neutral (weather/erosion/bias zero) full path against arithmetic drift.
#include "vt_noise.glsl"

struct CloudDensitySample { float coarse; float full; };

float cloud_height_profile(GpuCloudLayer L, float y) {
    float lo = L.min_height, hi = L.max_height;
    if (!(hi > lo) || y <= lo || y >= hi) return 0.0;
    float thickness = hi - lo;
    float f_lo = clamp(L.falloff_min, 0.0, thickness);
    float f_hi = clamp(L.falloff_max, 0.0, thickness);
    float rise = f_lo > 0.0 ? smoothstep(lo, lo + f_lo, y) : 1.0;
    float fall = f_hi > 0.0 ? 1.0 - smoothstep(hi - f_hi, hi, y) : 1.0;
    return min(rise, fall);
}

float cloud_fbm(GpuCloudLayer L, vec3 p, int octaves) {
    return vt_fbm3(p.x, p.y, p.z, uint(L.seed), octaves, L.gain,
                   L.lacunarity, 1.0, false) * 0.5 + 0.5;
}

CloudDensitySample evaluate_cloud_density(GpuCloudLayer L, vec3 world_pos,
                                          float time_seconds) {
    CloudDensitySample result = CloudDensitySample(0.0, 0.0);
    float profile = cloud_height_profile(L, world_pos.y);
    if (profile <= 0.0) return result;
    vec3 p = (world_pos + vec3(L.wind[0], L.wind[1], L.wind[2]) * time_seconds) * L.noise_scale;
    float coverage = L.coverage;
    if (L.weather_scale_influence_detail_scale_detail_erosion.y > 0.0) {
        float weather = cloud_fbm(L, vec3(world_pos.xz *
            L.weather_scale_influence_detail_scale_detail_erosion.x, 0.0), 2);
        coverage = clamp(coverage + (weather - 0.5) * 2.0 *
                         L.weather_scale_influence_detail_scale_detail_erosion.y,
                         0.0, 1.0);
    }
    if (coverage <= 0.0) return result;
    float threshold = 1.0 - coverage;
    int authored_octaves = int(L.octaves);
    float full_shape = smoothstep(
        threshold - CLOUD_COVERAGE_EDGE, threshold + CLOUD_COVERAGE_EDGE,
        cloud_fbm(L, p, authored_octaves));
    float coarse_shape = smoothstep(
        threshold - CLOUD_COVERAGE_EDGE, threshold + CLOUD_COVERAGE_EDGE,
        cloud_fbm(L, p, min(authored_octaves, 2)));
    result.coarse = profile * L.max_density * coarse_shape;
    // cloud_layer_tests::test_task9_neutral_density_source_contract pins this
    // neutral path: no weather/bias/erosion arithmetic may move the authored
    // octave expression when all three new controls are zero.
    if (L.shape_bias_padding.x != 0.0)
        full_shape = clamp(full_shape + L.shape_bias_padding.x, 0.0, 1.0);
    result.full = profile * L.max_density * full_shape;
    if (L.weather_scale_influence_detail_scale_detail_erosion.w > 0.0) {
        float detail01 = cloud_fbm(L, world_pos *
            L.weather_scale_influence_detail_scale_detail_erosion.z, 3);
        result.full *= mix(1.0, smoothstep(0.2, 0.8, detail01),
                           L.weather_scale_influence_detail_scale_detail_erosion.w);
    }
    return result;
}
