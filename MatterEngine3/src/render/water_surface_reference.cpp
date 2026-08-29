#include "water_surface_reference.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace viewer {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kMinimumHemisphereDot = 0.05f;
constexpr float kMaximumWaterSlope = 4.0f;

float clamp01(float value) noexcept {
    return std::max(0.0f, std::min(1.0f, value));
}

float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

float length2(matter::Float2 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y);
}

float length3(matter::Float3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

matter::Float2 clamp_length(matter::Float2 value, float maximum) noexcept {
    const float magnitude = length2(value);
    if (!(magnitude > maximum) || !(magnitude > 0.0f)) return value;
    const float scale = maximum / magnitude;
    return {value.x * scale, value.y * scale};
}

matter::Float3 normalize(matter::Float3 value,
                         matter::Float3 fallback) noexcept {
    const float magnitude = length3(value);
    if (!(magnitude > 1.0e-8f) || !std::isfinite(magnitude)) return fallback;
    const float inverse = 1.0f / magnitude;
    return {value.x * inverse, value.y * inverse, value.z * inverse};
}

float dot(matter::Float3 a, matter::Float3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float luminance(matter::Float3 value) noexcept {
    return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

matter::Float3 mix3(matter::Float3 a, matter::Float3 b, float t) noexcept {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

float smoothstep(float low, float high, float value) noexcept {
    if (!(high > low)) return value >= high ? 1.0f : 0.0f;
    const float t = clamp01((value - low) / (high - low));
    return t * t * (3.0f - 2.0f * t);
}

float breakup_noise(matter::Float2 position, float scale_m) noexcept {
    const float scale = std::max(scale_m, 0.001f);
    const float phase = position.x * (2.17f / scale) +
                        position.y * (3.11f / scale);
    const float secondary = position.x * (5.03f / scale) -
                            position.y * (1.73f / scale);
    return clamp01(0.5f + 0.32f * std::sin(phase) +
                   0.18f * std::sin(secondary + std::sin(phase)));
}

std::size_t cell_offset(const PackedWaterField& field, std::uint32_t x,
                        std::uint32_t z) noexcept {
    return (static_cast<std::size_t>(z) * field.layout.width + x) * 4u;
}

float continuous_channel(const std::vector<std::uint16_t>& image,
                         const PackedWaterField& field, float relative_x,
                         float relative_z, std::uint32_t channel) noexcept {
    const float texel_x = relative_x - 0.5f;
    const float texel_z = relative_z - 0.5f;
    const int x0_unclamped = static_cast<int>(std::floor(texel_x));
    const int z0_unclamped = static_cast<int>(std::floor(texel_z));
    const float tx = texel_x - static_cast<float>(x0_unclamped);
    const float tz = texel_z - static_cast<float>(z0_unclamped);
    const auto clamp_x = [&field](int x) {
        return static_cast<std::uint32_t>(std::max(
            0, std::min(x, static_cast<int>(field.layout.width) - 1)));
    };
    const auto clamp_z = [&field](int z) {
        return static_cast<std::uint32_t>(std::max(
            0, std::min(z, static_cast<int>(field.layout.depth) - 1)));
    };
    const std::uint32_t x0 = clamp_x(x0_unclamped);
    const std::uint32_t x1 = clamp_x(x0_unclamped + 1);
    const std::uint32_t z0 = clamp_z(z0_unclamped);
    const std::uint32_t z1 = clamp_z(z0_unclamped + 1);
    const auto fetch = [&](std::uint32_t x, std::uint32_t z) {
        return water_half_to_float(image[cell_offset(field, x, z) + channel]);
    };
    const float row0 = lerp(fetch(x0, z0), fetch(x1, z0), tx);
    const float row1 = lerp(fetch(x0, z1), fetch(x1, z1), tx);
    return lerp(row0, row1, tz);
}

float feature_response(hydrology::RiverFeature feature) noexcept {
    switch (feature) {
        case hydrology::RiverFeature::Rapid:
        case hydrology::RiverFeature::Waterfall:
        case hydrology::RiverFeature::Impact:
        case hydrology::RiverFeature::Spillway:
            return 1.0f;
        case hydrology::RiverFeature::Current:
            return 0.35f;
        default:
            return 0.0f;
    }
}

float feature_foam_bias(hydrology::RiverFeature feature) noexcept {
    switch (feature) {
        case hydrology::RiverFeature::Rapid: return 0.10f;
        case hydrology::RiverFeature::Waterfall: return 0.30f;
        case hydrology::RiverFeature::Impact: return 0.35f;
        case hydrology::RiverFeature::Spillway: return 0.25f;
        case hydrology::RiverFeature::Current: return 0.02f;
        default: return 0.0f;
    }
}

}  // namespace

bool water_sample_field_reference(
    const PackedWaterField& field, WaterFieldBinding published_binding,
    WaterFieldBinding requested_binding, matter::Float2 world_xz,
    WaterSurfaceFieldSample& output) noexcept {
    output = {};
    if (!packed_water_field_valid(field) || !published_binding.valid() ||
        published_binding.slot != requested_binding.slot ||
        published_binding.generation != requested_binding.generation ||
        !std::isfinite(world_xz.x) || !std::isfinite(world_xz.y))
        return false;
    const float relative_x =
        (world_xz.x - field.layout.origin_m.x) / field.layout.cell_size_m;
    const float relative_z =
        (world_xz.y - field.layout.origin_m.z) / field.layout.cell_size_m;
    if (!(relative_x >= 0.0f) || !(relative_z >= 0.0f) ||
        !(relative_x < static_cast<float>(field.layout.width)) ||
        !(relative_z < static_cast<float>(field.layout.depth)))
        return false;
    const std::uint32_t nearest_x =
        static_cast<std::uint32_t>(std::floor(relative_x));
    const std::uint32_t nearest_z =
        static_cast<std::uint32_t>(std::floor(relative_z));
    const std::size_t nearest = cell_offset(field, nearest_x, nearest_z);
    if (field.image_c_rgba8[nearest + 2u] < 128u) return false;

    output.surface_height_m = continuous_channel(
        field.image_a_rgba16f, field, relative_x, relative_z, 0u);
    output.depth_m = continuous_channel(
        field.image_a_rgba16f, field, relative_x, relative_z, 1u);
    output.velocity_mps.x = continuous_channel(
        field.image_a_rgba16f, field, relative_x, relative_z, 2u);
    output.velocity_mps.z = continuous_channel(
        field.image_a_rgba16f, field, relative_x, relative_z, 3u);
    output.velocity_mps.y = continuous_channel(
        field.image_b_rgba16f, field, relative_x, relative_z, 0u);
    const float normal_x = continuous_channel(
        field.image_b_rgba16f, field, relative_x, relative_z, 1u);
    const float normal_z = continuous_channel(
        field.image_b_rgba16f, field, relative_x, relative_z, 2u);
    output.base_normal = normalize(
        {normal_x,
         std::sqrt(std::max(0.0f, 1.0f - normal_x * normal_x -
                                      normal_z * normal_z)),
         normal_z},
        {0.0f, 1.0f, 0.0f});
    output.turbulence = continuous_channel(
        field.image_b_rgba16f, field, relative_x, relative_z, 3u);
    constexpr float kUnorm = 1.0f / 255.0f;
    output.aeration = field.image_c_rgba8[nearest + 0u] * kUnorm;
    output.foam_potential = field.image_c_rgba8[nearest + 1u] * kUnorm;
    output.feature = decode_water_feature(field.image_c_rgba8[nearest + 3u]);
    output.local_foam_multiplier = continuous_channel(
        field.image_d_rgba16f, field, relative_x, relative_z, 0u);
    output.local_threshold_offset = continuous_channel(
        field.image_d_rgba16f, field, relative_x, relative_z, 1u);
    output.local_wave_multiplier = continuous_channel(
        field.image_d_rgba16f, field, relative_x, relative_z, 2u);
    output.valid = true;
    return true;
}

bool water_backtrace_rk2_reference(
    const PackedWaterField& field, WaterFieldBinding published_binding,
    WaterFieldBinding requested_binding, matter::Float2 world_xz,
    float age_seconds, float max_shading_speed_mps,
    matter::Float2& output) noexcept {
    output = world_xz;
    if (!std::isfinite(age_seconds) || age_seconds < 0.0f ||
        !std::isfinite(max_shading_speed_mps) ||
        !(max_shading_speed_mps > 0.0f))
        return false;
    WaterSurfaceFieldSample initial{};
    if (!water_sample_field_reference(field, published_binding,
                                      requested_binding, world_xz, initial))
        return false;
    const float dt = age_seconds / static_cast<float>(kWaterBacktraceSteps);
    matter::Float2 current = world_xz;
    for (int step = 0; step < kWaterBacktraceSteps; ++step) {
        WaterSurfaceFieldSample at_current{};
        if (!water_sample_field_reference(field, published_binding,
                                          requested_binding, current,
                                          at_current))
            break;
        const matter::Float2 velocity0 = clamp_length(
            {at_current.velocity_mps.x, at_current.velocity_mps.z},
            max_shading_speed_mps);
        const matter::Float2 midpoint{
            current.x - 0.5f * dt * velocity0.x,
            current.y - 0.5f * dt * velocity0.y};
        WaterSurfaceFieldSample at_midpoint{};
        if (!water_sample_field_reference(field, published_binding,
                                          requested_binding, midpoint,
                                          at_midpoint))
            break;
        const matter::Float2 midpoint_velocity = clamp_length(
            {at_midpoint.velocity_mps.x, at_midpoint.velocity_mps.z},
            max_shading_speed_mps);
        const matter::Float2 next{
            current.x - dt * midpoint_velocity.x,
            current.y - dt * midpoint_velocity.y};
        WaterSurfaceFieldSample at_next{};
        if (!water_sample_field_reference(field, published_binding,
                                          requested_binding, next, at_next))
            break;
        current = next;
    }
    output = current;
    return true;
}

WaterDualPhase water_wrapped_phase_reference(
    float time_seconds, float period_seconds) noexcept {
    WaterDualPhase phase{};
    if (!std::isfinite(time_seconds) || !std::isfinite(period_seconds) ||
        !(period_seconds > 0.0f))
        return phase;
    const auto positive_mod = [period_seconds](float value) {
        float wrapped = std::fmod(value, period_seconds);
        if (wrapped < 0.0f) wrapped += period_seconds;
        return wrapped;
    };
    phase.age_seconds[0] = positive_mod(time_seconds);
    phase.age_seconds[1] = positive_mod(time_seconds + 0.5f * period_seconds);
    for (int index = 0; index != 2; ++index) {
        const float sine = std::sin(kPi * phase.age_seconds[index] /
                                    period_seconds);
        phase.weight[index] = sine * sine;
    }
    const float sum = phase.weight[0] + phase.weight[1];
    if (sum > 0.0f) {
        phase.weight[0] /= sum;
        phase.weight[1] /= sum;
    }
    return phase;
}

std::array<float, kWaterWaveBandCount> water_band_responses_reference(
    const matter::WaterSurfaceDefinition& surface,
    const WaterSurfaceFieldSample& field) noexcept {
    std::array<float, kWaterWaveBandCount> result{};
    if (surface.wave_bands.size() != kWaterWaveBandCount || !field.valid)
        return result;
    const float speed = std::sqrt(field.velocity_mps.x * field.velocity_mps.x +
                                  field.velocity_mps.z * field.velocity_mps.z);
    const float slope = std::sqrt(field.base_normal.x * field.base_normal.x +
                                  field.base_normal.z * field.base_normal.z);
    const float feature = feature_response(field.feature);
    const float broad_driver = clamp01(
        0.55f * (field.depth_m / (field.depth_m + 1.5f)) +
        0.35f * (speed / (speed + 2.0f)) + 0.10f * feature);
    const float chop_driver = clamp01(
        0.35f * (speed / (speed + 2.0f)) + 0.20f * slope +
        0.35f * field.turbulence + 0.10f * feature);
    const float capillary_driver = clamp01(
        (0.45f * (speed / (speed + 1.0f)) +
         0.45f * field.turbulence + 0.10f * feature) *
        (1.0f - 0.75f * clamp01(field.foam_potential)));
    const float drivers[kWaterWaveBandCount] = {
        broad_driver, chop_driver, capillary_driver};
    for (int band = 0; band != kWaterWaveBandCount; ++band) {
        const float response = clamp01(surface.wave_bands[band].response);
        result[band] = lerp(1.0f - response, 1.0f, drivers[band]) *
                       std::max(0.0f, field.local_wave_multiplier);
    }
    return result;
}

WaterOpticalState water_optical_state_reference(
    const matter::WaterSurfaceDefinition& surface,
    float optical_distance_m, float foam_coverage) noexcept {
    WaterOpticalState optics{};
    const float optical_distance = std::max(0.0f, optical_distance_m);
    const float bounded_foam_coverage = clamp01(foam_coverage);
    const float depth_blend = smoothstep(1.5f, 4.0f, optical_distance);
    const matter::Float3 absorption = mix3(
        surface.optics.shallow_absorption,
        surface.optics.deep_absorption, depth_blend);
    const float absorption_distance = std::max(
        0.001f, lerp(surface.optics.shallow_distance_m,
                     surface.optics.deep_distance_m, depth_blend));
    const float optical_depth = optical_distance / absorption_distance;
    optics.transmittance = {
        std::exp(-absorption.x * optical_depth),
        std::exp(-absorption.y * optical_depth),
        std::exp(-absorption.z * optical_depth)};
    optics.bottom_visibility = clamp01(luminance(optics.transmittance));
    const float ior = std::clamp(surface.optics.ior, 1.0f, 2.5f);
    const float fresnel0 = (ior - 1.0f) / (ior + 1.0f);
    optics.reflection_weight = fresnel0 * fresnel0;
    optics.coherent_transmission_weight = clamp01(
        (1.0f - optics.reflection_weight) * optics.bottom_visibility *
        (1.0f - bounded_foam_coverage *
                    clamp01(surface.foam.transmission_loss)));
    const float remaining = std::max(
        0.0f, 1.0f - optics.reflection_weight -
                  optics.coherent_transmission_weight);
    const float depth_scattering = 1.0f - std::exp(
        -optical_distance /
        std::max(0.001f, surface.optics.scattering_distance_m));
    optics.diffuse_scattering_weight = std::min(
        remaining,
        depth_scattering + bounded_foam_coverage *
                               std::max(0.0f, surface.foam.scattering_gain));
    optics.scattering_color = mix3(
        surface.optics.scattering_color, {0.92f, 0.97f, 1.0f},
        bounded_foam_coverage);
    return optics;
}

float water_foam_driver_reference(
    float foam_potential, float turbulence, float aeration,
    hydrology::RiverFeature feature) noexcept {
    const float primary = std::clamp(foam_potential, 0.0f, 1.0f);
    const float turbulence_support =
        0.10f * std::clamp(turbulence, 0.0f, 1.0f);
    const float aeration_support =
        0.20f * std::clamp(aeration, 0.0f, 1.0f);
    const float feature_support =
        std::min(feature_foam_bias(feature), 0.15f);
    return std::clamp(primary + turbulence_support + aeration_support +
                          feature_support,
                      0.0f, 1.0f);
}

matter::Float3 water_apply_foam_radiance_reference(
    matter::Float3 base_radiance, float foam_coverage) noexcept {
    return mix3(base_radiance, {0.92f, 0.97f, 1.0f},
                clamp01(foam_coverage));
}

bool water_evaluate_surface_reference(
    const PackedWaterField& field, WaterFieldBinding published_binding,
    WaterFieldBinding requested_binding,
    const matter::WaterSurfaceDefinition& surface, matter::Float2 world_xz,
    matter::Float3 geometric_normal, float animation_time_seconds,
    float base_roughness, WaterSurfaceEvaluation& output) noexcept {
    output = {};
    output.shading_normal = normalize(geometric_normal, {0.0f, 1.0f, 0.0f});
    output.roughness = clamp01(base_roughness);
    if (surface.wave_bands.size() != kWaterWaveBandCount ||
        !std::isfinite(animation_time_seconds) ||
        !water_sample_field_reference(field, published_binding,
                                      requested_binding, world_xz,
                                      output.field))
        return false;

    output.foam.local_multiplier =
        std::max(0.0f, output.field.local_foam_multiplier);
    output.foam.threshold_offset = output.field.local_threshold_offset;
    output.foam.wave_multiplier =
        std::max(0.0f, output.field.local_wave_multiplier);
    const float foam_driver = water_foam_driver_reference(
        output.field.foam_potential, output.field.turbulence,
        output.field.aeration, output.field.feature);
    output.foam.macro_mask = clamp01(
        (foam_driver - surface.foam.threshold -
         output.foam.threshold_offset) * surface.foam.gain);
    matter::Float2 foam_position = world_xz;
    if (surface.foam.persistence_s > 0.0f) {
        const float age = std::fmod(
            std::max(0.0f, animation_time_seconds),
            std::max(0.001f, surface.foam.persistence_s));
        (void)water_backtrace_rk2_reference(
            field, published_binding, requested_binding, world_xz, age,
            20.0f, foam_position);
    }
    output.foam.breakup_detail =
        breakup_noise(foam_position, surface.foam.breakup_scale_m);
    output.foam.coverage = clamp01(
        output.foam.macro_mask * output.foam.local_multiplier *
        lerp(0.85f, 1.0f, output.foam.breakup_detail));

    output.optics = water_optical_state_reference(
        surface, std::max(output.field.depth_m, 0.0f), output.foam.coverage);
    output.reactivity = clamp01(std::max(
        output.foam.coverage,
        0.35f * clamp01(output.field.turbulence) +
            0.15f * clamp01(output.field.aeration)));

    const auto responses = water_band_responses_reference(surface, output.field);
    matter::Float2 flow{output.field.velocity_mps.x,
                        output.field.velocity_mps.z};
    float flow_length = length2(flow);
    if (!(flow_length > 0.05f)) {
        flow = {1.0f, 0.0f};
        flow_length = 1.0f;
    }
    flow.x /= flow_length;
    flow.y /= flow_length;
    matter::Float2 slope{};
    for (int band = 0; band < kWaterWaveBandCount; ++band) {
        const auto& wave = surface.wave_bands[band];
        if (!(wave.wavelength_m > 0.0f) ||
            !(wave.speed_multiplier > 0.0f) ||
            !(wave.normal_amplitude >= 0.0f))
            continue;
        const float angle = band == 0 ? 0.0f : (band == 1 ? 0.52f : -0.83f);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const matter::Float2 direction{
            flow.x * cosine - flow.y * sine,
            flow.x * sine + flow.y * cosine};
        const float period = std::max(
            0.25f, wave.wavelength_m / wave.speed_multiplier);
        const WaterDualPhase phases =
            water_wrapped_phase_reference(animation_time_seconds, period);
        float blended_gradient = 0.0f;
        for (int phase_index = 0; phase_index != 2; ++phase_index) {
            matter::Float2 backtraced = world_xz;
            (void)water_backtrace_rk2_reference(
                field, published_binding, requested_binding, world_xz,
                phases.age_seconds[phase_index], 20.0f, backtraced);
            const float theta =
                kTwoPi * (backtraced.x * direction.x +
                          backtraced.y * direction.y) /
                    wave.wavelength_m +
                kTwoPi * phases.age_seconds[phase_index] / period;
            blended_gradient += phases.weight[phase_index] * std::cos(theta);
        }
        const float amplitude =
            std::min(1.0f, wave.normal_amplitude) * responses[band];
        slope.x += direction.x * amplitude * blended_gradient;
        slope.y += direction.y * amplitude * blended_gradient;
    }
    const float slope_length = length2(slope);
    if (slope_length > kMaximumWaterSlope) {
        const float scale = kMaximumWaterSlope / slope_length;
        slope.x *= scale;
        slope.y *= scale;
    }
    const matter::Float3 base = output.shading_normal;
    matter::Float3 slope_world{slope.x, 0.0f, slope.y};
    const float along_base = dot(slope_world, base);
    slope_world.x -= base.x * along_base;
    slope_world.y -= base.y * along_base;
    slope_world.z -= base.z * along_base;
    matter::Float3 animated = normalize(
        {base.x - slope_world.x, base.y - slope_world.y,
         base.z - slope_world.z},
        base);
    float hemisphere = dot(animated, base);
    if (hemisphere < kMinimumHemisphereDot) {
        matter::Float3 tangent{
            animated.x - base.x * hemisphere,
            animated.y - base.y * hemisphere,
            animated.z - base.z * hemisphere};
        const float tangent_length = length3(tangent);
        if (tangent_length > 1.0e-8f) {
            const float tangent_scale =
                std::sqrt(1.0f - kMinimumHemisphereDot *
                                      kMinimumHemisphereDot) /
                tangent_length;
            animated = {
                base.x * kMinimumHemisphereDot +
                    tangent.x * tangent_scale,
                base.y * kMinimumHemisphereDot +
                    tangent.y * tangent_scale,
                base.z * kMinimumHemisphereDot +
                    tangent.z * tangent_scale};
        } else {
            animated = base;
        }
    }
    const float normal_softening =
        output.foam.coverage * clamp01(surface.foam.normal_softening);
    output.shading_normal = normalize(
        mix3(animated, base, normal_softening), base);
    output.roughness = clamp01(
        base_roughness + 0.05f * std::min(slope_length, 1.0f) +
        0.08f * clamp01(output.field.turbulence) +
        output.foam.coverage * clamp01(surface.foam.roughness_gain));
    output.animated = true;
    return true;
}

}  // namespace viewer
