#include "hydrology/river_presentation_field.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hydrology {
namespace {

bool finite(float value) { return std::isfinite(value); }
bool finite(double value) { return std::isfinite(value); }

bool finite_float(double value, float& result) {
    if (!finite(value) || value < -std::numeric_limits<float>::max() ||
        value > std::numeric_limits<float>::max())
        return false;
    result = static_cast<float>(value);
    return finite(result);
}

// Stored presentation normals are exactly constrained to the closed X/Z unit
// disk.  Project after float conversion because rounding can otherwise move a
// mathematically valid positive-hemisphere normal just outside the disk.
bool project_normal(double x, double z, float& result_x, float& result_z) {
    if (!finite(x) || !finite(z)) return false;
    const double length = std::hypot(x, z);
    if (!finite(length)) return false;
    if (length > 1.0) {
        x /= length;
        z /= length;
    }
    if (!finite_float(x, result_x) || !finite_float(z, result_z)) return false;
    for (std::uint32_t attempt = 0u; attempt != 8u; ++attempt) {
        const double squared = static_cast<double>(result_x) * result_x +
                               static_cast<double>(result_z) * result_z;
        if (finite(squared) && squared <= 1.0) return true;
        if (!finite(squared)) return false;
        if (std::fabs(result_x) >= std::fabs(result_z))
            result_x = std::nextafter(result_x, 0.0f);
        else
            result_z = std::nextafter(result_z, 0.0f);
    }
    return false;
}

bool clamp01(double value, double& result) {
    if (!finite(value)) return false;
    result = std::max(0.0, std::min(1.0, value));
    return true;
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
        settings.impact_weight, settings.spillway_weight, settings.pool_weight,
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

bool normalized(double value, double scale, double& result) {
    if (!finite(value) || !finite(scale) || value < 0.0 || scale < 0.0)
        return false;
    return scale == 0.0 ? (result = 0.0, true) : clamp01(value / scale, result);
}

bool weighted_mean(const double* values, const double* weights,
                   std::size_t count, double& result) {
    double total = 0.0;
    double weight_total = 0.0;
    for (std::size_t i = 0; i != count; ++i) {
        if (!finite(values[i]) || !finite(weights[i]) || weights[i] < 0.0)
            return false;
        total += values[i] * weights[i];
        weight_total += weights[i];
        if (!finite(total) || !finite(weight_total)) return false;
    }
    result = weight_total > 0.0 ? total / weight_total : 0.0;
    return finite(result);
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
    const double normal_y_squared = 1.0 -
        static_cast<double>(sample.normal_x) * sample.normal_x -
        static_cast<double>(sample.normal_z) * sample.normal_z;
    return finite(normal_y_squared) && normal_y_squared >= 0.0;
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
    const auto fail = [&](const char* message) {
        samples.clear();
        error = message;
        return false;
    };
    const double inv_cell = 1.0 / static_cast<double>(input.layout.cell_size_m);
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
            double gradient_x = 0.0;
            double gradient_z = 0.0;
            if (has_left && has_right)
                gradient_x = (static_cast<double>(right->height_m) - left->height_m) * 0.5 * inv_cell;
            else if (has_right)
                gradient_x = (static_cast<double>(right->height_m) - gameplay.height_m) * inv_cell;
            else if (has_left)
                gradient_x = (static_cast<double>(gameplay.height_m) - left->height_m) * inv_cell;
            if (has_up && has_down)
                gradient_z = (static_cast<double>(down->height_m) - up->height_m) * 0.5 * inv_cell;
            else if (has_down)
                gradient_z = (static_cast<double>(down->height_m) - gameplay.height_m) * inv_cell;
            else if (has_up)
                gradient_z = (static_cast<double>(gameplay.height_m) - up->height_m) * inv_cell;
            if (!finite(gradient_x) || !finite(gradient_z)) {
                return fail("river presentation field gradient overflow");
            }
            const double slope_length = std::hypot(gradient_x, gradient_z);
            const double normal_length = std::hypot(1.0, slope_length);
            if (!finite(normal_length) || normal_length <= 0.0f) {
                return fail("river presentation field normal overflow");
            }
            PresentationSample result{};
            if (!project_normal(-gradient_x / normal_length,
                                -gradient_z / normal_length,
                                result.normal_x, result.normal_z))
                return fail("river presentation field normal is not representable");

            double dvx_dx = 0.0;
            double dvz_dz = 0.0;
            double dvz_dx = 0.0;
            double dvx_dz = 0.0;
            if (has_left && has_right) {
                dvx_dx = (static_cast<double>(right->velocity_x_mps) - left->velocity_x_mps) * 0.5 * inv_cell;
                dvz_dx = (static_cast<double>(right->velocity_z_mps) - left->velocity_z_mps) * 0.5 * inv_cell;
            } else if (has_right) {
                dvx_dx = (static_cast<double>(right->velocity_x_mps) - gameplay.velocity_x_mps) * inv_cell;
                dvz_dx = (static_cast<double>(right->velocity_z_mps) - gameplay.velocity_z_mps) * inv_cell;
            } else if (has_left) {
                dvx_dx = (static_cast<double>(gameplay.velocity_x_mps) - left->velocity_x_mps) * inv_cell;
                dvz_dx = (static_cast<double>(gameplay.velocity_z_mps) - left->velocity_z_mps) * inv_cell;
            }
            if (has_up && has_down) {
                dvz_dz = (static_cast<double>(down->velocity_z_mps) - up->velocity_z_mps) * 0.5 * inv_cell;
                dvx_dz = (static_cast<double>(down->velocity_x_mps) - up->velocity_x_mps) * 0.5 * inv_cell;
            } else if (has_down) {
                dvz_dz = (static_cast<double>(down->velocity_z_mps) - gameplay.velocity_z_mps) * inv_cell;
                dvx_dz = (static_cast<double>(down->velocity_x_mps) - gameplay.velocity_x_mps) * inv_cell;
            } else if (has_up) {
                dvz_dz = (static_cast<double>(gameplay.velocity_z_mps) - up->velocity_z_mps) * inv_cell;
                dvx_dz = (static_cast<double>(gameplay.velocity_x_mps) - up->velocity_x_mps) * inv_cell;
            }
            if (!finite(dvx_dx) || !finite(dvz_dz) || !finite(dvz_dx) ||
                !finite(dvx_dz)) {
                return fail("river presentation field derivative overflow");
            }
            const double retained_variance =
                input.gameplay_statistics->velocity_variance_mps2[index];
            if (!finite(retained_variance) || retained_variance < 0.0f) {
                return fail("river presentation field contains invalid retained variance");
            }
            double variance = 0.0, divergence = 0.0, vorticity = 0.0;
            double vertical_speed = 0.0, slope = 0.0, shallow = 0.0;
            if (!normalized(retained_variance, settings.velocity_variance_scale_mps2, variance) ||
                !normalized(std::fabs(dvx_dx + dvz_dz), settings.divergence_scale_per_m, divergence) ||
                !normalized(std::fabs(dvz_dx - dvx_dz), settings.vorticity_scale_per_m, vorticity) ||
                !normalized(std::fabs(static_cast<double>(gameplay.velocity_y_mps)), settings.vertical_speed_scale_mps, vertical_speed) ||
                !normalized(slope_length, settings.surface_slope_scale, slope) ||
                (settings.shallow_depth_m > 0.0f &&
                 !clamp01(1.0 - static_cast<double>(gameplay.depth_m) /
                                      settings.shallow_depth_m, shallow)))
                return fail("river presentation field metric is non-finite");
            const PresentationMarkers& markers = (*input.markers)[index];
            const PresentationLocalOverride& local_override =
                (*input.local_overrides)[index];
            const double turbulence_weights[] = {
                settings.velocity_variance_weight, settings.divergence_weight,
                settings.vorticity_weight, settings.vertical_speed_weight,
                settings.surface_slope_weight, settings.shallows_weight,
                markers.pool ? settings.pool_weight : 0.0f};
            const double turbulence_values_with_pool[] = {
                variance, divergence, vorticity, vertical_speed, slope, shallow, 0.0f};
            double raw_turbulence = 0.0;
            if (!weighted_mean(turbulence_values_with_pool, turbulence_weights, 7u,
                               raw_turbulence) ||
                !clamp01(raw_turbulence * local_override.turbulence_multiplier,
                         raw_turbulence) ||
                !finite_float(raw_turbulence, result.turbulence))
                return fail("river presentation turbulence overflow");
            const double aeration_weights[] = {
                settings.vertical_speed_weight, settings.velocity_variance_weight,
                settings.waterfall_weight, settings.impact_weight,
                markers.pool ? settings.pool_weight : 0.0f};
            const double aeration_values_with_pool[] = {
                vertical_speed, variance, markers.waterfall ? 1.0f : 0.0f,
                markers.impact ? 1.0f : 0.0f, 0.0f};
            double raw_aeration = 0.0;
            if (!weighted_mean(aeration_values_with_pool, aeration_weights, 5u,
                               raw_aeration) ||
                !clamp01(raw_aeration * local_override.aeration_multiplier,
                         raw_aeration) ||
                !finite_float(raw_aeration, result.aeration))
                return fail("river presentation aeration overflow");
            double wake = 0.0;
            if (settings.wake_distance_scale_m > 0.0f &&
                !clamp01(1.0 - static_cast<double>((*input.wake_distances_m)[index]) /
                                     settings.wake_distance_scale_m, wake))
                return fail("river presentation wake metric is non-finite");
            const double foam_weights[] = {
                1.0f, 1.0f, settings.shallows_weight,
                settings.wake_distance_weight, settings.spillway_weight,
                markers.pool ? settings.pool_weight : 0.0f};
            const double foam_values_with_pool[] = {
                result.turbulence, result.aeration, shallow, wake,
                markers.spillway ? 1.0f : 0.0f, 0.0f};
            double raw_foam = 0.0;
            if (!weighted_mean(foam_values_with_pool, foam_weights, 6u, raw_foam) ||
                !clamp01(raw_foam * local_override.foam_multiplier, raw_foam) ||
                !finite_float(raw_foam, result.foam_potential))
                return fail("river presentation foam overflow");
            const double speed = std::hypot(static_cast<double>(gameplay.velocity_x_mps),
                                            static_cast<double>(gameplay.velocity_z_mps));
            if (!finite(speed)) return fail("river presentation speed is non-finite");
            result.feature = feature_for(markers, static_cast<float>(
                std::min(speed, static_cast<double>(std::numeric_limits<float>::max()))), settings);
            result.wet_valid = finite(result.normal_x) && finite(result.normal_z) &&
                               finite(result.turbulence) && finite(result.aeration) &&
                               finite(result.foam_potential) &&
                               result.turbulence >= 0.0f && result.turbulence <= 1.0f &&
                               result.aeration >= 0.0f && result.aeration <= 1.0f &&
                               result.foam_potential >= 0.0f && result.foam_potential <= 1.0f;
            const double result_normal_sq = static_cast<double>(result.normal_x) * result.normal_x +
                                            static_cast<double>(result.normal_z) * result.normal_z;
            if (!result.wet_valid || !finite(result_normal_sq) || result_normal_sq > 1.0)
                return fail("river presentation field output is invalid");
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
    const double cell_x = (static_cast<double>(x_m) - layout.origin_m.x) /
                              layout.cell_size_m - 0.5;
    const double cell_z = (static_cast<double>(z_m) - layout.origin_m.z) /
                              layout.cell_size_m - 0.5;
    if (!finite(cell_x) || !finite(cell_z) || cell_x < 0.0 || cell_z < 0.0 ||
        cell_x > static_cast<double>(layout.width - 1u) ||
        cell_z > static_cast<double>(layout.depth - 1u))
        return false;
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(cell_x));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(cell_z));
    const std::uint32_t x1 = std::min(x0 + 1u, layout.width - 1u);
    const std::uint32_t z1 = std::min(z0 + 1u, layout.depth - 1u);
    const double tx = cell_x - static_cast<double>(x0);
    const double tz = cell_z - static_cast<double>(z0);
    const PresentationSample* contributors[] = {
        &samples[index_of(layout, x0, z0)], &samples[index_of(layout, x1, z0)],
        &samples[index_of(layout, x0, z1)], &samples[index_of(layout, x1, z1)]};
    const double weights[] = {(1.0 - tx) * (1.0 - tz), tx * (1.0 - tz),
                              (1.0 - tx) * tz, tx * tz};
    for (std::size_t i = 0; i != 4u; ++i)
        if (weights[i] > 0.0f && !valid_presentation(*contributors[i]))
            return false;
    double normal_x = 0.0, normal_z = 0.0, turbulence = 0.0;
    double aeration = 0.0, foam_potential = 0.0;
    for (std::size_t i = 0; i != 4u; ++i) {
        if (weights[i] == 0.0) continue;
        normal_x += static_cast<double>(contributors[i]->normal_x) * weights[i];
        normal_z += static_cast<double>(contributors[i]->normal_z) * weights[i];
        turbulence += static_cast<double>(contributors[i]->turbulence) * weights[i];
        aeration += static_cast<double>(contributors[i]->aeration) * weights[i];
        foam_potential += static_cast<double>(contributors[i]->foam_potential) * weights[i];
    }
    if (!project_normal(normal_x, normal_z, sample.normal_x, sample.normal_z) ||
        !finite_float(turbulence, sample.turbulence) ||
        !finite_float(aeration, sample.aeration) ||
        !finite_float(foam_potential, sample.foam_potential)) {
        sample = {};
        return false;
    }
    const std::uint32_t nearest_x =
        static_cast<std::uint32_t>(std::floor(cell_x + 0.5));
    const std::uint32_t nearest_z =
        static_cast<std::uint32_t>(std::floor(cell_z + 0.5));
    const PresentationSample& nearest = samples[index_of(
        layout, std::min(nearest_x, layout.width - 1u),
        std::min(nearest_z, layout.depth - 1u))];
    if (!valid_presentation(nearest)) {
        sample = {};
        return false;
    }
    sample.turbulence = std::max(0.0f, std::min(1.0f, sample.turbulence));
    sample.aeration = std::max(0.0f, std::min(1.0f, sample.aeration));
    sample.foam_potential = std::max(0.0f, std::min(1.0f, sample.foam_potential));
    sample.feature = nearest.feature;
    sample.wet_valid = true;
    return true;
}

}  // namespace hydrology
