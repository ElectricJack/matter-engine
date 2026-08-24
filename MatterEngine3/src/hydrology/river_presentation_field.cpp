#include "hydrology/river_presentation_field.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hydrology {
namespace {

bool finite(float value) { return std::isfinite(value); }

float clamp01(float value) {
    return finite(value) ? std::max(0.0f, std::min(1.0f, value)) : 0.0f;
}

bool valid_layout(const GameplayFieldLayout& layout) {
    return finite(layout.origin_m.x) && finite(layout.origin_m.y) &&
           finite(layout.origin_m.z) && finite(layout.cell_size_m) &&
           layout.cell_size_m > 0.0f && layout.width != 0u &&
           layout.depth != 0u &&
           static_cast<std::uint64_t>(layout.width) * layout.depth <=
               16ull * 1024ull * 1024ull;
}

bool wet(const GameplaySample& sample) {
    return sample.wet_valid && finite(sample.height_m) &&
           finite(sample.depth_m) && finite(sample.velocity_x_mps) &&
           finite(sample.velocity_y_mps) && finite(sample.velocity_z_mps);
}

bool valid_settings(const PresentationDerivationSettings& settings) {
    if (settings.contract_version == 0u) return false;
    const float values[] = {
        settings.velocity_variance_weight, settings.divergence_weight,
        settings.vorticity_weight, settings.vertical_speed_weight,
        settings.surface_slope_weight, settings.shallows_weight,
        settings.wake_distance_weight, settings.waterfall_weight,
        settings.impact_weight, settings.spillway_weight,
        settings.velocity_variance_scale_mps2,
        settings.divergence_scale_per_m, settings.vorticity_scale_per_m,
        settings.vertical_speed_scale_mps, settings.surface_slope_scale,
        settings.shallow_depth_m, settings.wake_distance_scale_m,
        settings.current_speed_mps, settings.rapid_speed_mps};
    for (float value : values)
        if (!finite(value) || value < 0.0f) return false;
    return settings.rapid_speed_mps >= settings.current_speed_mps;
}

std::size_t index_of(const GameplayFieldLayout& layout, std::uint32_t x,
                     std::uint32_t z) {
    return static_cast<std::size_t>(z) * layout.width + x;
}

bool neighbour(const PresentationDerivationInput& input, int x, int z,
               const GameplaySample*& result) {
    const GameplayFieldLayout& layout = input.layout;
    if (x < 0 || z < 0 || x >= static_cast<int>(layout.width) ||
        z >= static_cast<int>(layout.depth))
        return false;
    const GameplaySample& candidate =
        (*input.gameplay)[index_of(layout, static_cast<std::uint32_t>(x),
                                   static_cast<std::uint32_t>(z))];
    if (!wet(candidate)) return false;
    result = &candidate;
    return true;
}

float normalized(float value, float scale) {
    return scale > 0.0f ? clamp01(value / scale) : 0.0f;
}

float weighted_mean(const float* values, const float* weights,
                    std::size_t count) {
    float total = 0.0f;
    float weight_total = 0.0f;
    for (std::size_t i = 0; i != count; ++i) {
        total += values[i] * weights[i];
        weight_total += weights[i];
    }
    return weight_total > 0.0f ? total / weight_total : 0.0f;
}

RiverFeature feature_for(const PresentationMarkers& markers, float speed,
                         const PresentationDerivationSettings& settings) {
    if (markers.waterfall) return RiverFeature::Waterfall;
    if (markers.impact) return RiverFeature::Impact;
    if (markers.spillway) return RiverFeature::Spillway;
    if (markers.pool) return RiverFeature::Pool;
    if (speed >= settings.rapid_speed_mps) return RiverFeature::Rapid;
    if (speed >= settings.current_speed_mps) return RiverFeature::Current;
    return RiverFeature::Calm;
}

bool valid_presentation(const PresentationSample& sample) {
    if (!sample.wet_valid || !finite(sample.normal_x) ||
        !finite(sample.normal_z) || !finite(sample.turbulence) ||
        !finite(sample.aeration) || !finite(sample.foam_potential) ||
        sample.turbulence < 0.0f || sample.turbulence > 1.0f ||
        sample.aeration < 0.0f || sample.aeration > 1.0f ||
        sample.foam_potential < 0.0f || sample.foam_potential > 1.0f ||
        static_cast<std::uint8_t>(sample.feature) >
            static_cast<std::uint8_t>(RiverFeature::Pool))
        return false;
    const float normal_y_squared = 1.0f - sample.normal_x * sample.normal_x -
                                   sample.normal_z * sample.normal_z;
    return finite(normal_y_squared) && normal_y_squared >= 0.0f;
}

}  // namespace

bool build_river_presentation_field(
    const PresentationDerivationInput& input,
    const PresentationDerivationSettings& settings,
    std::vector<PresentationSample>& samples, std::string& error) {
    samples.clear();
    error.clear();
    if (!valid_layout(input.layout) || !valid_settings(settings) ||
        input.gameplay == nullptr || input.gameplay_statistics == nullptr ||
        input.terrain_heights_m == nullptr || input.wake_distances_m == nullptr ||
        input.markers == nullptr || input.local_overrides == nullptr) {
        error = "river presentation field input is invalid";
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(input.layout.width) *
                              input.layout.depth;
    if (input.gameplay->size() != count ||
        input.gameplay_statistics->velocity_variance_mps2.size() != count ||
        input.terrain_heights_m->size() != count ||
        input.wake_distances_m->size() != count || input.markers->size() != count ||
        input.local_overrides->size() != count) {
        error = "river presentation field input dimensions do not match layout";
        return false;
    }
    for (std::size_t i = 0; i != count; ++i) {
        const PresentationLocalOverride& local_override =
            (*input.local_overrides)[i];
        if (!finite((*input.terrain_heights_m)[i]) ||
            !finite((*input.wake_distances_m)[i]) ||
            (*input.wake_distances_m)[i] < 0.0f ||
            !finite(local_override.turbulence_multiplier) ||
            !finite(local_override.aeration_multiplier) ||
            !finite(local_override.foam_multiplier) ||
            local_override.turbulence_multiplier < 0.0f ||
            local_override.aeration_multiplier < 0.0f ||
            local_override.foam_multiplier < 0.0f) {
            error = "river presentation field contains invalid authored input";
            return false;
        }
    }
    samples.assign(count, {});
    const float inv_cell = 1.0f / input.layout.cell_size_m;
    for (std::uint32_t z = 0; z != input.layout.depth; ++z) {
        for (std::uint32_t x = 0; x != input.layout.width; ++x) {
            const std::size_t index = index_of(input.layout, x, z);
            const GameplaySample& gameplay = (*input.gameplay)[index];
            if (!wet(gameplay)) continue;
            const GameplaySample *left = nullptr, *right = nullptr;
            const GameplaySample *up = nullptr, *down = nullptr;
            const bool has_left = neighbour(input, static_cast<int>(x) - 1,
                                            static_cast<int>(z), left);
            const bool has_right = neighbour(input, static_cast<int>(x) + 1,
                                             static_cast<int>(z), right);
            const bool has_up = neighbour(input, static_cast<int>(x),
                                          static_cast<int>(z) - 1, up);
            const bool has_down = neighbour(input, static_cast<int>(x),
                                            static_cast<int>(z) + 1, down);
            float gradient_x = 0.0f;
            float gradient_z = 0.0f;
            if (has_left && has_right)
                gradient_x = (right->height_m - left->height_m) * 0.5f * inv_cell;
            else if (has_right)
                gradient_x = (right->height_m - gameplay.height_m) * inv_cell;
            else if (has_left)
                gradient_x = (gameplay.height_m - left->height_m) * inv_cell;
            if (has_up && has_down)
                gradient_z = (down->height_m - up->height_m) * 0.5f * inv_cell;
            else if (has_down)
                gradient_z = (down->height_m - gameplay.height_m) * inv_cell;
            else if (has_up)
                gradient_z = (gameplay.height_m - up->height_m) * inv_cell;
            const float normal_length = std::sqrt(
                gradient_x * gradient_x + 1.0f + gradient_z * gradient_z);
            PresentationSample result{};
            result.normal_x = -gradient_x / normal_length;
            result.normal_z = -gradient_z / normal_length;

            float dvx_dx = 0.0f;
            float dvz_dz = 0.0f;
            float dvz_dx = 0.0f;
            float dvx_dz = 0.0f;
            if (has_left && has_right) {
                dvx_dx = (right->velocity_x_mps - left->velocity_x_mps) * 0.5f * inv_cell;
                dvz_dx = (right->velocity_z_mps - left->velocity_z_mps) * 0.5f * inv_cell;
            } else if (has_right) {
                dvx_dx = (right->velocity_x_mps - gameplay.velocity_x_mps) * inv_cell;
                dvz_dx = (right->velocity_z_mps - gameplay.velocity_z_mps) * inv_cell;
            } else if (has_left) {
                dvx_dx = (gameplay.velocity_x_mps - left->velocity_x_mps) * inv_cell;
                dvz_dx = (gameplay.velocity_z_mps - left->velocity_z_mps) * inv_cell;
            }
            if (has_up && has_down) {
                dvz_dz = (down->velocity_z_mps - up->velocity_z_mps) * 0.5f * inv_cell;
                dvx_dz = (down->velocity_x_mps - up->velocity_x_mps) * 0.5f * inv_cell;
            } else if (has_down) {
                dvz_dz = (down->velocity_z_mps - gameplay.velocity_z_mps) * inv_cell;
                dvx_dz = (down->velocity_x_mps - gameplay.velocity_x_mps) * inv_cell;
            } else if (has_up) {
                dvz_dz = (gameplay.velocity_z_mps - up->velocity_z_mps) * inv_cell;
                dvx_dz = (gameplay.velocity_x_mps - up->velocity_x_mps) * inv_cell;
            }
            const float retained_variance =
                input.gameplay_statistics->velocity_variance_mps2[index];
            if (!finite(retained_variance) || retained_variance < 0.0f) {
                samples.clear();
                error = "river presentation field contains invalid retained variance";
                return false;
            }
            const float variance = normalized(retained_variance,
                                              settings.velocity_variance_scale_mps2);
            const float divergence = normalized(std::fabs(dvx_dx + dvz_dz),
                                                settings.divergence_scale_per_m);
            const float vorticity = normalized(std::fabs(dvz_dx - dvx_dz),
                                               settings.vorticity_scale_per_m);
            const float vertical_speed = normalized(std::fabs(gameplay.velocity_y_mps),
                                                    settings.vertical_speed_scale_mps);
            const float slope = normalized(std::sqrt(gradient_x * gradient_x +
                                                      gradient_z * gradient_z),
                                           settings.surface_slope_scale);
            const float shallow = settings.shallow_depth_m > 0.0f
                                      ? clamp01(1.0f - gameplay.depth_m /
                                                           settings.shallow_depth_m)
                                      : 0.0f;
            const PresentationMarkers& markers = (*input.markers)[index];
            const PresentationLocalOverride& local_override =
                (*input.local_overrides)[index];
            const float turbulence_values[] = {
                variance, divergence, vorticity, vertical_speed, slope, shallow};
            const float turbulence_weights[] = {
                settings.velocity_variance_weight, settings.divergence_weight,
                settings.vorticity_weight, settings.vertical_speed_weight,
                settings.surface_slope_weight, settings.shallows_weight};
            result.turbulence = clamp01(
                weighted_mean(turbulence_values, turbulence_weights, 6u) *
                local_override.turbulence_multiplier);
            const float aeration_values[] = {
                vertical_speed, variance, markers.waterfall ? 1.0f : 0.0f,
                markers.impact ? 1.0f : 0.0f};
            const float aeration_weights[] = {
                settings.vertical_speed_weight, settings.velocity_variance_weight,
                settings.waterfall_weight, settings.impact_weight};
            result.aeration = clamp01(
                weighted_mean(aeration_values, aeration_weights, 4u) *
                local_override.aeration_multiplier);
            const float wake = settings.wake_distance_scale_m > 0.0f
                                   ? clamp01(1.0f - (*input.wake_distances_m)[index] /
                                                        settings.wake_distance_scale_m)
                                   : 0.0f;
            const float foam_values[] = {
                result.turbulence, result.aeration, shallow, wake,
                markers.spillway ? 1.0f : 0.0f};
            const float foam_weights[] = {
                1.0f, 1.0f, settings.shallows_weight,
                settings.wake_distance_weight, settings.spillway_weight};
            result.foam_potential = clamp01(
                weighted_mean(foam_values, foam_weights, 5u) *
                local_override.foam_multiplier);
            const float speed = std::sqrt(gameplay.velocity_x_mps * gameplay.velocity_x_mps +
                                          gameplay.velocity_z_mps * gameplay.velocity_z_mps);
            result.feature = feature_for(markers, speed, settings);
            result.wet_valid = finite(result.normal_x) && finite(result.normal_z) &&
                               finite(result.turbulence) && finite(result.aeration) &&
                               finite(result.foam_potential);
            if (!result.wet_valid) result = {};
            samples[index] = result;
        }
    }
    return true;
}

RiverFeature classify_river_feature(
    const PresentationMarkers& markers, float speed_mps,
    const PresentationDerivationSettings& settings) noexcept {
    if (!valid_settings(settings) || !finite(speed_mps) || speed_mps < 0.0f)
        return RiverFeature::Calm;
    return feature_for(markers, speed_mps, settings);
}

bool sample_river_presentation_field(
    const GameplayFieldLayout& layout,
    const std::vector<PresentationSample>& samples, float x_m, float z_m,
    PresentationSample& sample) noexcept {
    sample = {};
    if (!valid_layout(layout) || !finite(x_m) || !finite(z_m) ||
        samples.size() != static_cast<std::size_t>(layout.width) * layout.depth)
        return false;
    const float cell_x = (x_m - layout.origin_m.x) / layout.cell_size_m - 0.5f;
    const float cell_z = (z_m - layout.origin_m.z) / layout.cell_size_m - 0.5f;
    if (cell_x < 0.0f || cell_z < 0.0f ||
        cell_x > static_cast<float>(layout.width - 1u) ||
        cell_z > static_cast<float>(layout.depth - 1u))
        return false;
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(cell_x));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(cell_z));
    const std::uint32_t x1 = std::min(x0 + 1u, layout.width - 1u);
    const std::uint32_t z1 = std::min(z0 + 1u, layout.depth - 1u);
    const float tx = cell_x - static_cast<float>(x0);
    const float tz = cell_z - static_cast<float>(z0);
    const PresentationSample* contributors[] = {
        &samples[index_of(layout, x0, z0)], &samples[index_of(layout, x1, z0)],
        &samples[index_of(layout, x0, z1)], &samples[index_of(layout, x1, z1)]};
    const float weights[] = {(1.0f - tx) * (1.0f - tz), tx * (1.0f - tz),
                             (1.0f - tx) * tz, tx * tz};
    for (std::size_t i = 0; i != 4u; ++i)
        if (weights[i] > 0.0f && !valid_presentation(*contributors[i]))
            return false;
    for (std::size_t i = 0; i != 4u; ++i) {
        if (weights[i] == 0.0f) continue;
        sample.normal_x += contributors[i]->normal_x * weights[i];
        sample.normal_z += contributors[i]->normal_z * weights[i];
        sample.turbulence += contributors[i]->turbulence * weights[i];
        sample.aeration += contributors[i]->aeration * weights[i];
        sample.foam_potential += contributors[i]->foam_potential * weights[i];
    }
    const float length_sq = sample.normal_x * sample.normal_x +
                            sample.normal_z * sample.normal_z;
    if (!finite(length_sq) || length_sq > 1.0f) return false;
    const std::uint32_t nearest_x =
        static_cast<std::uint32_t>(std::floor(cell_x + 0.5f));
    const std::uint32_t nearest_z =
        static_cast<std::uint32_t>(std::floor(cell_z + 0.5f));
    const PresentationSample& nearest = samples[index_of(
        layout, std::min(nearest_x, layout.width - 1u),
        std::min(nearest_z, layout.depth - 1u))];
    if (!valid_presentation(nearest)) return false;
    sample.turbulence = clamp01(sample.turbulence);
    sample.aeration = clamp01(sample.aeration);
    sample.foam_potential = clamp01(sample.foam_potential);
    sample.feature = nearest.feature;
    sample.wet_valid = true;
    return true;
}

}  // namespace hydrology
