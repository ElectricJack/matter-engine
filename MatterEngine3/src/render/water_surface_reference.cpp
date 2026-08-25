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
        result[band] = lerp(1.0f - response, 1.0f, drivers[band]);
    }
    return result;
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
    output.shading_normal = animated;
    output.roughness = clamp01(
        base_roughness + 0.05f * std::min(slope_length, 1.0f) +
        0.08f * clamp01(output.field.turbulence));
    output.animated = true;
    return true;
}

}  // namespace viewer
