#include "hydrology/fluid_gameplay_field.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hydrology {
namespace {

bool finite(float value) { return std::isfinite(value); }
bool finite(double value) { return std::isfinite(value); }

bool to_float(double value, float& result) {
    if (!finite(value) || value < -std::numeric_limits<float>::max() ||
        value > std::numeric_limits<float>::max())
        return false;
    result = static_cast<float>(value);
    return finite(result);
}

bool terrain_cell_centre(float origin, float cell_size, std::uint32_t index,
                         float& result) {
    const double offset = (static_cast<double>(index) + 0.5) * cell_size;
    const double margin = static_cast<double>(std::numeric_limits<float>::max()) -
                          static_cast<double>(origin);
    if (!finite(offset) || offset <= 0.0 || !finite(margin) || offset > margin ||
        !to_float(static_cast<double>(origin) + offset, result) ||
        !(result > origin))
        return false;
    if (index == 0u) return true;
    const double previous_offset = (static_cast<double>(index) - 0.5) * cell_size;
    float previous = 0.0f;
    return finite(previous_offset) && previous_offset > 0.0 &&
           previous_offset <= margin &&
           to_float(static_cast<double>(origin) + previous_offset, previous) &&
           result > previous;
}

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

struct VelocityWelford {
    std::uint32_t count = 0u;
    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    double m2 = 0.0;
    bool add(const matter::Float3& velocity) {
        if (count == std::numeric_limits<std::uint32_t>::max()) return false;
        const double next = static_cast<double>(count) + 1.0;
        const double dx = static_cast<double>(velocity.x) - mean_x;
        const double dy = static_cast<double>(velocity.y) - mean_y;
        const double dz = static_cast<double>(velocity.z) - mean_z;
        mean_x += dx / next; mean_y += dy / next; mean_z += dz / next;
        m2 += dx * (static_cast<double>(velocity.x) - mean_x) +
              dy * (static_cast<double>(velocity.y) - mean_y) +
              dz * (static_cast<double>(velocity.z) - mean_z);
        ++count;
        return std::isfinite(m2) && m2 >= 0.0;
    }
};

}  // namespace

bool build_fluid_gameplay_field(
    const std::vector<FluidParticle>& particles, float particle_radius_m,
    const GameplayFieldLayout& layout, const TerrainHeightSampler& terrain,
    std::vector<GameplaySample>& samples, std::string& error,
    GameplayFieldStatistics* statistics) {
    samples.clear();
    error.clear();
    if (statistics != nullptr) statistics->velocity_variance_mps2.clear();
    if (!valid_layout(layout) || !finite(particle_radius_m) ||
        particle_radius_m <= 0.0f || !terrain) {
        error = "fluid gameplay field input is invalid";
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(layout.width) * layout.depth;
    samples.assign(count, {});
    // GameplaySample stores the running mean; this count is the only extra
    // storage necessary when variance statistics are not requested.
    std::vector<std::uint32_t> particle_count(count, 0u);
    std::vector<VelocityWelford> velocity_statistics;
    if (statistics != nullptr) velocity_statistics.resize(count);
    std::vector<float> terrain_height(count, 0.0f);
    const auto fail = [&](const char* message) {
        samples.clear();
        if (statistics != nullptr) statistics->velocity_variance_mps2.clear();
        error = message;
        return false;
    };
    for (std::uint32_t z = 0; z != layout.depth; ++z) {
        for (std::uint32_t x = 0; x != layout.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(z) * layout.width + x;
            float world_x = 0.0f, world_z = 0.0f;
            if (!terrain_cell_centre(layout.origin_m.x, layout.cell_size_m, x,
                                     world_x) ||
                !terrain_cell_centre(layout.origin_m.z, layout.cell_size_m, z,
                                     world_z))
                return fail("fluid gameplay field terrain coordinate is not representable");
            if (!terrain(world_x, world_z, terrain_height[index]) ||
                !finite(terrain_height[index])) {
                return fail("terrain height sampling failed for fluid gameplay field");
            }
        }
    }
    for (const FluidParticle& particle : particles) {
        if (!finite(particle.position_m.x) || !finite(particle.position_m.y) ||
            !finite(particle.position_m.z) || !finite(particle.velocity_mps.x) ||
            !finite(particle.velocity_mps.y) || !finite(particle.velocity_mps.z)) {
            return fail("fluid gameplay field particle is non-finite");
        }
        const double cell_x = (static_cast<double>(particle.position_m.x) -
                               layout.origin_m.x) / layout.cell_size_m;
        const double cell_z = (static_cast<double>(particle.position_m.z) -
                               layout.origin_m.z) / layout.cell_size_m;
        if (!finite(cell_x) || !finite(cell_z))
            return fail("fluid gameplay field particle coordinate is non-finite");
        if (cell_x < 0.0 || cell_z < 0.0 ||
            cell_x >= static_cast<double>(layout.width) ||
            cell_z >= static_cast<double>(layout.depth))
            continue;
        const std::uint32_t x = static_cast<std::uint32_t>(std::floor(cell_x));
        const std::uint32_t z = static_cast<std::uint32_t>(std::floor(cell_z));
        const std::size_t index = static_cast<std::size_t>(z) * layout.width + x;
        GameplaySample& sample = samples[index];
        float surface = 0.0f;
        if (!to_float(static_cast<double>(particle.position_m.y) +
                          static_cast<double>(particle_radius_m), surface))
            return fail("fluid gameplay field surface is not representable");
        if (particle_count[index] == std::numeric_limits<std::uint32_t>::max())
            return fail("fluid gameplay field particle count overflow");
        const double next = static_cast<double>(particle_count[index]) + 1.0;
        const auto add_velocity = [&](float current, float incoming,
                                      float& result) {
            return to_float(static_cast<double>(current) +
                                (static_cast<double>(incoming) - current) / next,
                            result);
        };
        float velocity_x = 0.0f, velocity_y = 0.0f, velocity_z = 0.0f;
        if (!add_velocity(sample.velocity_x_mps, particle.velocity_mps.x, velocity_x) ||
            !add_velocity(sample.velocity_y_mps, particle.velocity_mps.y, velocity_y) ||
            !add_velocity(sample.velocity_z_mps, particle.velocity_mps.z, velocity_z))
            return fail("fluid gameplay field velocity is not representable");
        if (!sample.wet_valid || surface > sample.height_m) sample.height_m = surface;
        sample.velocity_x_mps = velocity_x;
        sample.velocity_y_mps = velocity_y;
        sample.velocity_z_mps = velocity_z;
        ++particle_count[index];
        if (statistics != nullptr && !velocity_statistics[index].add(particle.velocity_mps))
            return fail("fluid gameplay field velocity statistics overflow");
        sample.wet_valid = true;
    }
    for (std::size_t index = 0; index != samples.size(); ++index) {
        GameplaySample& sample = samples[index];
        if (!sample.wet_valid) continue;
        if (sample.height_m <= terrain_height[index]) {
            sample = {};
            continue;
        }
        if (!to_float(static_cast<double>(sample.height_m) - terrain_height[index],
                      sample.depth_m))
            return fail("fluid gameplay field depth is not representable");
        if (!valid_wet_sample(sample))
            return fail("fluid gameplay field produced a non-finite wet sample");
    }
    if (statistics != nullptr) {
        statistics->velocity_variance_mps2.assign(count, 0.0f);
        for (std::size_t index = 0; index != count; ++index) {
            const GameplaySample& sample = samples[index];
            if (!sample.wet_valid) continue;
            const VelocityWelford& accumulator = velocity_statistics[index];
            const double variance = accumulator.count == 0u ? 0.0 :
                accumulator.m2 / static_cast<double>(accumulator.count);
            if (!std::isfinite(variance) || variance < 0.0 ||
                variance > std::numeric_limits<float>::max()) {
                return fail("fluid gameplay field variance is non-finite");
            }
            statistics->velocity_variance_mps2[index] =
                static_cast<float>(variance);
        }
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
    const GameplaySample* contributors[] = {
        &samples[static_cast<std::size_t>(z0) * layout.width + x0],
        &samples[static_cast<std::size_t>(z0) * layout.width + x1],
        &samples[static_cast<std::size_t>(z1) * layout.width + x0],
        &samples[static_cast<std::size_t>(z1) * layout.width + x1]};
    const double weights[] = {(1.0 - tx) * (1.0 - tz), tx * (1.0 - tz),
                              (1.0 - tx) * tz, tx * tz};
    for (std::size_t i = 0; i != 4u; ++i)
        if (weights[i] > 0.0f && !valid_wet_sample(*contributors[i]))
            return false;
    double height = 0.0, depth = 0.0, velocity_x = 0.0, velocity_y = 0.0,
           velocity_z = 0.0;
    for (std::size_t i = 0; i != 4u; ++i) {
        if (weights[i] == 0.0) continue;
        height += static_cast<double>(contributors[i]->height_m) * weights[i];
        depth += static_cast<double>(contributors[i]->depth_m) * weights[i];
        velocity_x += static_cast<double>(contributors[i]->velocity_x_mps) * weights[i];
        velocity_y += static_cast<double>(contributors[i]->velocity_y_mps) * weights[i];
        velocity_z += static_cast<double>(contributors[i]->velocity_z_mps) * weights[i];
    }
    sample.wet_valid = to_float(height, sample.height_m) && to_float(depth, sample.depth_m) &&
                       to_float(velocity_x, sample.velocity_x_mps) &&
                       to_float(velocity_y, sample.velocity_y_mps) &&
                       to_float(velocity_z, sample.velocity_z_mps);
    if (!sample.wet_valid) sample = {};
    return sample.wet_valid;
}

}  // namespace hydrology
