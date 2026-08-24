#include "hydrology/fluid_gameplay_field.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hydrology {
namespace {

bool finite(float value) { return std::isfinite(value); }

bool valid_layout(const GameplayFieldLayout& layout) {
    return finite(layout.origin_m.x) && finite(layout.origin_m.y) &&
           finite(layout.origin_m.z) && finite(layout.cell_size_m) &&
           layout.cell_size_m > 0.0f && layout.width != 0u &&
           layout.depth != 0u &&
           static_cast<std::uint64_t>(layout.width) * layout.depth <=
               16ull * 1024ull * 1024ull;
}

bool valid_wet_sample(const GameplaySample& sample) {
    return sample.wet_valid && finite(sample.height_m) &&
           finite(sample.depth_m) && finite(sample.velocity_x_mps) &&
           finite(sample.velocity_y_mps) && finite(sample.velocity_z_mps);
}

}  // namespace

bool build_fluid_gameplay_field(
    const std::vector<FluidParticle>& particles, float particle_radius_m,
    const GameplayFieldLayout& layout, const TerrainHeightSampler& terrain,
    std::vector<GameplaySample>& samples, std::string& error) {
    samples.clear();
    error.clear();
    if (!valid_layout(layout) || !finite(particle_radius_m) ||
        particle_radius_m <= 0.0f || !terrain) {
        error = "fluid gameplay field input is invalid";
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(layout.width) * layout.depth;
    samples.assign(count, {});
    std::vector<float> velocity_weight(count, 0.0f);
    std::vector<float> terrain_height(count, 0.0f);
    const float volume = 4.1887902047863909846f * particle_radius_m *
                         particle_radius_m * particle_radius_m;
    for (std::uint32_t z = 0; z != layout.depth; ++z) {
        for (std::uint32_t x = 0; x != layout.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(z) * layout.width + x;
            const float world_x = layout.origin_m.x +
                (static_cast<float>(x) + 0.5f) * layout.cell_size_m;
            const float world_z = layout.origin_m.z +
                (static_cast<float>(z) + 0.5f) * layout.cell_size_m;
            if (!terrain(world_x, world_z, terrain_height[index]) ||
                !finite(terrain_height[index])) {
                samples.clear();
                error = "terrain height sampling failed for fluid gameplay field";
                return false;
            }
        }
    }
    for (const FluidParticle& particle : particles) {
        if (!finite(particle.position_m.x) || !finite(particle.position_m.y) ||
            !finite(particle.position_m.z) || !finite(particle.velocity_mps.x) ||
            !finite(particle.velocity_mps.y) || !finite(particle.velocity_mps.z)) {
            samples.clear();
            error = "fluid gameplay field particle is non-finite";
            return false;
        }
        const int x = static_cast<int>(std::floor(
            (particle.position_m.x - layout.origin_m.x) / layout.cell_size_m));
        const int z = static_cast<int>(std::floor(
            (particle.position_m.z - layout.origin_m.z) / layout.cell_size_m));
        if (x < 0 || z < 0 || x >= static_cast<int>(layout.width) ||
            z >= static_cast<int>(layout.depth))
            continue;
        const std::size_t index = static_cast<std::size_t>(z) * layout.width +
                                  static_cast<std::size_t>(x);
        GameplaySample& sample = samples[index];
        const float surface = particle.position_m.y + particle_radius_m;
        if (!sample.wet_valid || surface > sample.height_m)
            sample.height_m = surface;
        sample.velocity_x_mps += particle.velocity_mps.x * volume;
        sample.velocity_y_mps += particle.velocity_mps.y * volume;
        sample.velocity_z_mps += particle.velocity_mps.z * volume;
        velocity_weight[index] += volume;
        sample.wet_valid = true;
    }
    for (std::size_t index = 0; index != samples.size(); ++index) {
        GameplaySample& sample = samples[index];
        if (!sample.wet_valid) continue;
        if (sample.height_m <= terrain_height[index]) {
            sample = {};
            continue;
        }
        sample.depth_m = sample.height_m - terrain_height[index];
        sample.velocity_x_mps /= velocity_weight[index];
        sample.velocity_y_mps /= velocity_weight[index];
        sample.velocity_z_mps /= velocity_weight[index];
    }
    return true;
}

bool sample_fluid_gameplay_field(const GameplayFieldLayout& layout,
                                 const std::vector<GameplaySample>& samples,
                                 float x_m, float z_m,
                                 GameplaySample& sample) noexcept {
    sample = {};
    if (!valid_layout(layout) || !finite(x_m) || !finite(z_m) ||
        samples.size() != static_cast<std::size_t>(layout.width) * layout.depth)
        return false;
    const float cell_x = (x_m - layout.origin_m.x) / layout.cell_size_m -
                         0.5f;
    const float cell_z = (z_m - layout.origin_m.z) / layout.cell_size_m -
                         0.5f;
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
    const GameplaySample* contributors[] = {
        &samples[static_cast<std::size_t>(z0) * layout.width + x0],
        &samples[static_cast<std::size_t>(z0) * layout.width + x1],
        &samples[static_cast<std::size_t>(z1) * layout.width + x0],
        &samples[static_cast<std::size_t>(z1) * layout.width + x1]};
    const float weights[] = {(1.0f - tx) * (1.0f - tz), tx * (1.0f - tz),
                             (1.0f - tx) * tz, tx * tz};
    for (std::size_t i = 0; i != 4u; ++i)
        if (weights[i] > 0.0f && !valid_wet_sample(*contributors[i]))
            return false;
    for (std::size_t i = 0; i != 4u; ++i) {
        sample.height_m += contributors[i]->height_m * weights[i];
        sample.depth_m += contributors[i]->depth_m * weights[i];
        sample.velocity_x_mps += contributors[i]->velocity_x_mps * weights[i];
        sample.velocity_y_mps += contributors[i]->velocity_y_mps * weights[i];
        sample.velocity_z_mps += contributors[i]->velocity_z_mps * weights[i];
    }
    sample.wet_valid = finite(sample.height_m) && finite(sample.depth_m) &&
                       finite(sample.velocity_x_mps) &&
                       finite(sample.velocity_y_mps) &&
                       finite(sample.velocity_z_mps);
    if (!sample.wet_valid) sample = {};
    return sample.wet_valid;
}

}  // namespace hydrology
