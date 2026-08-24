#include "hydrology/physx_fluid_bake.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hydrology {

namespace {

bool finite(float value) noexcept { return std::isfinite(value); }

bool finite(const matter::Float2& value) noexcept {
    return finite(value.x) && finite(value.y);
}

bool finite(const matter::Float3& value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool valid_bounds(const matter::Aabb& bounds) noexcept {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x < bounds.maximum.x &&
           bounds.minimum.y < bounds.maximum.y &&
           bounds.minimum.z < bounds.maximum.z;
}

bool inside(const matter::Aabb& bounds,
            const matter::Float3& point) noexcept {
    return point.x >= bounds.minimum.x && point.x <= bounds.maximum.x &&
           point.y >= bounds.minimum.y && point.y <= bounds.maximum.y &&
           point.z >= bounds.minimum.z && point.z <= bounds.maximum.z;
}

gpu_meshing::ParticleJob tight_visual_job(
    const std::vector<FluidParticle>& particles, float radius_m,
    const gpu_meshing::ParticleJob& authored) {
    gpu_meshing::ParticleJob result = authored;
    if (particles.empty()) return result;
    matter::Float3 minimum = particles.front().position_m;
    matter::Float3 maximum = minimum;
    for (const FluidParticle& particle : particles) {
        minimum.x = std::min(minimum.x, particle.position_m.x);
        minimum.y = std::min(minimum.y, particle.position_m.y);
        minimum.z = std::min(minimum.z, particle.position_m.z);
        maximum.x = std::max(maximum.x, particle.position_m.x);
        maximum.y = std::max(maximum.y, particle.position_m.y);
        maximum.z = std::max(maximum.z, particle.position_m.z);
    }
    // Match the mesher's finite field-query support, then add one complete
    // voxel so marching cells cannot clip the outer isosurface.
    const float padding = radius_m * 2.5f +
                          authored.blend_width_m * 4.0f +
                          authored.voxel_m;
    result.bounds_m.min_m = {
        std::max(authored.bounds_m.min_m.x, minimum.x - padding),
        std::max(authored.bounds_m.min_m.y, minimum.y - padding),
        std::max(authored.bounds_m.min_m.z, minimum.z - padding)};
    result.bounds_m.max_m = {
        std::min(authored.bounds_m.max_m.x, maximum.x + padding),
        std::min(authored.bounds_m.max_m.y, maximum.y + padding),
        std::min(authored.bounds_m.max_m.z, maximum.z + padding)};
    return result;
}

bool fail(FluidBakeCode code, const char* message,
          FluidBakeOutput& output, FluidBakeError& error) {
    output = {};
    error = {code, message};
    return false;
}

bool fail_preserving_output(FluidBakeCode code, const char* message,
                            FluidBakeError& error) {
    error = {code, message};
    return false;
}

bool validate_input(const FluidBakeInput& input,
                    FluidBakeOutput& output, FluidBakeError& error) {
    if (!finite(input.network.cell_size_m) ||
        input.network.cell_size_m <= 0.0f || input.network.rivers.empty() ||
        input.network.sections.empty() || !input.network.bake_sequential) {
        return fail(FluidBakeCode::InvalidInput,
                    "river network is incomplete", output, error);
    }
    for (const auto& river : input.network.rivers) {
        if (river.name.empty() || river.curve.size() < 2u ||
            !finite(river.inlet.position_m) ||
            !finite(river.inlet.flow_m3s) || river.inlet.flow_m3s <= 0.0f) {
            return fail(FluidBakeCode::InvalidInput,
                        "river definition is invalid", output, error);
        }
        for (const auto& point : river.curve) {
            if (!finite(point)) {
                return fail(FluidBakeCode::InvalidInput,
                            "river curve contains a non-finite point",
                            output, error);
            }
        }
        for (const auto& point : river.channel_profile) {
            if (!finite(point.distance_m) || !finite(point.width_m) ||
                !finite(point.depth_m) || !finite(point.asymmetry)) {
                return fail(FluidBakeCode::InvalidInput,
                            "river channel profile contains a non-finite value",
                            output, error);
            }
        }
        if (river.channel_profile.empty()) {
            return fail(FluidBakeCode::InvalidInput,
                        "river channel profile is empty",
                        output, error);
        }
    }
    for (const auto& section : input.network.sections) {
        if (section.id.empty() || section.river.empty() ||
            !finite(section.from_m) || !finite(section.to_m) ||
            !finite(section.dry_margin_m)) {
            return fail(FluidBakeCode::InvalidInput,
                        "river-section settings contain an invalid value",
                        output, error);
        }
        for (const auto& waterfall : section.waterfalls) {
            if (!finite(waterfall.lip_distance_m) ||
                !finite(waterfall.landing_distance_m) ||
                !finite(waterfall.expected_drop_m)) {
                return fail(FluidBakeCode::InvalidInput,
                            "river-section waterfall contains a non-finite value",
                            output, error);
            }
        }
        if (section.terminal_pool &&
            (!finite(section.terminal_pool->start_distance_m) ||
             !finite(section.terminal_pool->end_distance_m) ||
             !finite(section.terminal_pool->fill_level_m))) {
            return fail(FluidBakeCode::InvalidInput,
                        "river-section pool contains a non-finite value",
                        output, error);
        }
        if (section.terminal_spillway &&
            (!finite(section.terminal_spillway->distance_m) ||
             !finite(section.terminal_spillway->width_m) ||
             !finite(section.terminal_spillway->effective_depth_m) ||
             !finite(section.terminal_spillway->overlap_m) ||
             !finite(section.terminal_spillway->dam_offset_m))) {
            return fail(FluidBakeCode::InvalidInput,
                        "river-section spillway contains a non-finite value",
                        output, error);
        }
    }
    if (input.geometry.centreline.size() < 2u ||
        !valid_bounds(input.geometry.bounds_m)) {
        return fail(FluidBakeCode::InvalidInput,
                    "river geometry is incomplete", output, error);
    }
    for (const auto& sample : input.geometry.centreline) {
        if (!finite(sample.position_m) || !finite(sample.tangent) ||
            !finite(sample.lateral) || !finite(sample.distance_m) ||
            !finite(sample.width_m) || !finite(sample.depth_m) ||
            !finite(sample.asymmetry)) {
            return fail(FluidBakeCode::InvalidInput,
                        "river geometry contains a non-finite sample",
                        output, error);
        }
    }
    if (input.collision.vertices.empty() ||
        input.collision.indices.empty() ||
        input.collision.indices.size() % 3u != 0u) {
        return fail(FluidBakeCode::InvalidInput,
                    "collision mesh is empty or not triangular", output,
                    error);
    }
    for (const auto& vertex : input.collision.vertices) {
        if (!finite(vertex)) {
            return fail(FluidBakeCode::InvalidInput,
                        "collision mesh contains a non-finite vertex",
                        output, error);
        }
    }
    for (const std::uint32_t index : input.collision.indices) {
        if (index >= input.collision.vertices.size()) {
            return fail(FluidBakeCode::InvalidInput,
                        "collision mesh index is out of range", output,
                        error);
        }
    }
    if (input.emitters.empty()) {
        return fail(FluidBakeCode::InvalidInput,
                    "at least one fluid emitter is required", output,
                    error);
    }
    std::unordered_set<std::uint32_t> emitter_ids;
    for (const auto& emitter : input.emitters) {
        if (!emitter_ids.insert(emitter.id).second ||
            !valid_fluid_emitter(emitter)) {
            return fail(FluidBakeCode::InvalidInput,
                        "fluid emitter is invalid", output, error);
        }
    }
    const auto& settings = input.settings;
    if (!finite(settings.particle_spacing_m) ||
        settings.particle_spacing_m <= 0.0f ||
        !finite(settings.rest_density_kg_m3) ||
        settings.rest_density_kg_m3 <= 0.0f ||
        !finite(settings.fixed_step_seconds) ||
        settings.fixed_step_seconds <= 0.0f ||
        settings.solver_iterations == 0u || settings.max_neighbors == 0u ||
        settings.batch_steps == 0u || settings.max_steps == 0u ||
        settings.batch_steps > settings.max_steps ||
        settings.max_particles == 0u ||
        !finite(settings.escape_policy.ratio) ||
        settings.escape_policy.ratio < 0.0f) {
        return fail(FluidBakeCode::InvalidInput,
                    "PBD bake settings are invalid", output, error);
    }
    for (const auto& emitter : input.emitters) {
        if (emitter.stop_step > settings.max_steps) {
            return fail(FluidBakeCode::InvalidInput,
                        "fluid emitter exceeds max_steps", output, error);
        }
    }
    if (!valid_bounds(input.sensor.bounds_m) ||
        !valid_fluid_fill_sensor_frame(input.sensor) ||
        input.sensor.resolution.x == 0u ||
        input.sensor.resolution.y == 0u ||
        input.sensor.resolution.z == 0u ||
        !finite(input.sensor.required_wet_fraction) ||
        input.sensor.required_wet_fraction <= 0.0f ||
        input.sensor.required_wet_fraction > 1.0f ||
        input.sensor.stable_steps == 0u ||
        input.sensor.minimum_particles_per_cell == 0u ||
        !valid_bounds(input.dry_collar_bounds_m)) {
        return fail(FluidBakeCode::InvalidInput,
                    "fill sensor or dry collar is invalid", output, error);
    }
    return true;
}

bool validate_output(const FluidBakeInput& input,
                     FluidBakeOutput& output, FluidBakeError& error) {
    if (output.particles.size() > input.settings.max_particles ||
        output.stats.active_particles > input.settings.max_particles ||
        output.stats.peak_particles > input.settings.max_particles ||
        output.stats.emitted_particles > input.settings.max_particles) {
        return fail(FluidBakeCode::CapacityExceeded,
                    "backend particle counts exceed the declared capacity",
                    output, error);
    }
    if (output.stats.active_particles != output.particles.size() ||
        output.stats.emitted_particles !=
            output.stats.active_particles + output.stats.retired_particles ||
        output.stats.escaped_particles > output.stats.retired_particles ||
        output.stats.escaped_particles !=
            output.quarantined_particles.size() ||
        output.stats.peak_particles < output.stats.emitted_particles ||
        output.stats.escape_policy.absolute_count !=
            input.settings.escape_policy.absolute_count ||
        output.stats.escape_policy.ratio !=
            input.settings.escape_policy.ratio ||
        output.stats.escape_budget !=
            fluid_escape_budget(output.stats.emitted_particles,
                                input.settings.escape_policy)) {
        return fail(FluidBakeCode::BackendFailure,
                    "backend particle statistics are inconsistent", output,
                    error);
    }
    if (output.stats.simulated_steps > input.settings.max_steps ||
        !std::isfinite(output.stats.wall_seconds) ||
        output.stats.wall_seconds < 0.0) {
        return fail(FluidBakeCode::BackendFailure,
                    "backend statistics are invalid", output, error);
    }
    if (output.stats.non_finite_particles != 0u) {
        return fail(FluidBakeCode::NonFinite,
                    "backend reported non-finite particles", output, error);
    }
    // Non-finite state always wins over diagnostic terminal categories.  Do
    // this complete scan before the sensor/budget checks so unsafe data can
    // never be retained for failed-bake rendering.
    for (const auto& particle : output.particles) {
        if (!finite(particle.position_m) || !finite(particle.velocity_mps)) {
            return fail(FluidBakeCode::NonFinite,
                        "backend output contains a non-finite particle",
                        output, error);
        }
    }
    for (const auto& particle : output.quarantined_particles) {
        if (!finite(particle.position_m)) {
            return fail(FluidBakeCode::NonFinite,
                        "backend quarantine contains a non-finite particle",
                        output, error);
        }
    }
    if (output.stats.escaped_particles > output.stats.escape_budget) {
        return fail_preserving_output(
            FluidBakeCode::Escaped,
            "backend exceeded the escaped-particle quarantine budget", error);
    }
    if (!output.sensor.complete ||
        output.sensor.completion_step > output.stats.simulated_steps ||
        output.sensor.completion_step < output.sensor.stable_steps ||
        output.sensor.stable_steps > output.stats.simulated_steps ||
        output.sensor.stable_steps < input.sensor.stable_steps ||
        !finite(output.sensor.wet_fraction) ||
        !finite(output.sensor.maximum_wet_fraction) ||
        !finite(output.sensor.final_wet_fraction) ||
        !finite(output.sensor.stable_window_wet_fraction) ||
        output.sensor.wet_fraction < input.sensor.required_wet_fraction ||
        output.sensor.wet_fraction > 1.0f ||
        output.sensor.final_wet_fraction != output.sensor.wet_fraction ||
        output.sensor.maximum_wet_fraction <
            output.sensor.final_wet_fraction ||
        output.sensor.maximum_wet_fraction <
            output.sensor.stable_window_wet_fraction ||
        output.sensor.maximum_wet_fraction > 1.0f ||
        output.sensor.stable_window_wet_fraction <
            input.sensor.required_wet_fraction ||
        output.sensor.stable_window_wet_fraction > 1.0f ||
        output.sensor.first_satisfied_step == 0u ||
        output.sensor.first_satisfied_step >
            output.sensor.completion_step) {
        return fail_preserving_output(
            FluidBakeCode::SensorNotReached,
            "backend did not satisfy the fill sensor contract", error);
    }
    for (const auto& particle : output.particles) {
        if (!inside(input.dry_collar_bounds_m, particle.position_m)) {
            return fail(FluidBakeCode::Escaped,
                        "backend output contains an escaped particle",
                        output, error);
        }
    }
    std::sort(output.particles.begin(), output.particles.end(),
              [](const FluidParticle& left, const FluidParticle& right) {
                  return left.id < right.id;
              });
    for (std::size_t index = 1u; index < output.particles.size(); ++index) {
        if (output.particles[index - 1u].id == output.particles[index].id) {
            return fail(FluidBakeCode::BackendFailure,
                        "backend output contains duplicate particle ids",
                        output, error);
        }
    }
    std::unordered_set<std::uint64_t> accepted_ids;
    accepted_ids.reserve(output.particles.size());
    for (const FluidParticle& particle : output.particles)
        accepted_ids.insert(particle.id);
    std::unordered_set<std::uint64_t> quarantined_ids;
    quarantined_ids.reserve(output.quarantined_particles.size());
    for (const FluidQuarantinedParticle& particle :
         output.quarantined_particles) {
        if (!finite(particle.position_m) ||
            inside(input.dry_collar_bounds_m, particle.position_m) ||
            !quarantined_ids.insert(particle.id).second ||
            accepted_ids.find(particle.id) != accepted_ids.end()) {
            return fail(FluidBakeCode::BackendFailure,
                        "backend quarantine records are inconsistent",
                        output, error);
        }
    }
    return true;
}

bool validate_failed_debug_snapshot(const FluidBakeInput& input,
                                    FluidBakeCode failure_code,
                                    FluidBakeOutput& output,
                                    FluidBakeError& error) {
    if (failure_code != FluidBakeCode::SensorNotReached &&
        failure_code != FluidBakeCode::Escaped)
        return false;
    if (output.particles.empty() ||
        output.particles.size() > input.settings.max_particles ||
        output.stats.active_particles != output.particles.size() ||
        output.stats.emitted_particles !=
            output.stats.active_particles + output.stats.retired_particles ||
        output.stats.escaped_particles > output.stats.retired_particles ||
        output.stats.escaped_particles !=
            output.quarantined_particles.size() ||
        output.stats.escape_policy.absolute_count !=
            input.settings.escape_policy.absolute_count ||
        output.stats.escape_policy.ratio !=
            input.settings.escape_policy.ratio ||
        output.stats.escape_budget !=
            fluid_escape_budget(output.stats.emitted_particles,
                                input.settings.escape_policy) ||
        output.stats.non_finite_particles != 0u ||
        !std::isfinite(output.stats.wall_seconds) ||
        output.stats.wall_seconds < 0.0) {
        return fail(FluidBakeCode::BackendFailure,
                    "failed bake did not provide a consistent finite host snapshot",
                    output, error);
    }
    std::sort(output.particles.begin(), output.particles.end(),
              [](const FluidParticle& left, const FluidParticle& right) {
                  return left.id < right.id;
              });
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(output.particles.size());
    for (const FluidParticle& particle : output.particles) {
        if (!finite(particle.position_m) || !finite(particle.velocity_mps) ||
            !inside(input.dry_collar_bounds_m, particle.position_m) ||
            !ids.insert(particle.id).second) {
            return fail(FluidBakeCode::NonFinite,
                        "failed bake host snapshot is unsafe for diagnostics",
                        output, error);
        }
    }
    for (const FluidQuarantinedParticle& particle :
         output.quarantined_particles) {
        if (!finite(particle.position_m) ||
            inside(input.dry_collar_bounds_m, particle.position_m) ||
            ids.find(particle.id) != ids.end()) {
            return fail(FluidBakeCode::BackendFailure,
                        "failed bake quarantine is inconsistent",
                        output, error);
        }
    }
    return true;
}

bool build_particle_visual_chunks(
    const std::vector<FluidParticle>& source,
    const PhysxFluidBake::ProductBuildSettings& settings,
    const PhysxFluidBake::VisualMesher& visual_mesher,
    gpu_meshing::MeshResult& merged, gpu_meshing::Error& error) {
    merged = {};
    merged.material = 4u;

    // Establish one immutable grid for the complete particle envelope.  Every
    // retry below is an integer cell range of this grid, so neighbouring jobs
    // evaluate their overlap at the same sample locations instead of creating
    // unrelated tight grids (the source of the false transverse gaps).
    const gpu_meshing::ParticleJob root_template = tight_visual_job(
        source, settings.particle_radius_m, settings.visual_job);
    std::vector<gpu_meshing::ParticleSample> root_particles;
    root_particles.reserve(source.size());
    for (const FluidParticle& particle : source)
        root_particles.push_back(
            {particle.position_m, settings.particle_radius_m});
    gpu_meshing::ParticleJob layout_job = root_template;
    layout_job.particles = root_particles.data();
    layout_job.particle_count = static_cast<std::uint32_t>(root_particles.size());
    layout_job.limits.max_particles = std::numeric_limits<std::uint32_t>::max();
    layout_job.limits.max_grid_vertices =
        std::numeric_limits<std::uint32_t>::max();
    layout_job.limits.max_mesh_vertices =
        std::numeric_limits<std::uint32_t>::max();
    layout_job.limits.max_mesh_indices =
        std::numeric_limits<std::uint32_t>::max();
    gpu_meshing::GridLayout root_layout{};
    if (!gpu_meshing::validate_particle_job(
            layout_job, root_layout, error)) {
        merged = {};
        return false;
    }

    struct CellRange {
        std::array<std::uint32_t, 3> begin{};
        std::array<std::uint32_t, 3> end{};
    };
    const auto coordinate = [](matter::Float3 value, std::size_t axis) {
        return axis == 0u ? value.x : axis == 1u ? value.y : value.z;
    };
    const auto set_coordinate = [](matter::Float3& value, std::size_t axis,
                                   float coordinate_value) {
        if (axis == 0u) value.x = coordinate_value;
        else if (axis == 1u) value.y = coordinate_value;
        else value.z = coordinate_value;
    };
    const auto grid_coordinate = [&](std::size_t axis,
                                     std::uint32_t cell) {
        return coordinate(root_layout.origin_m, axis) +
               coordinate(root_layout.spacing_m, axis) *
                   static_cast<float>(cell);
    };

    struct WeldKey {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        bool operator==(const WeldKey& other) const noexcept {
            return x == other.x && y == other.y && z == other.z;
        }
    };
    struct WeldKeyHash {
        std::size_t operator()(const WeldKey& value) const noexcept {
            std::size_t result = static_cast<std::size_t>(value.x);
            result ^= static_cast<std::size_t>(value.y) +
                      UINT64_C(0x9e3779b97f4a7c15) + (result << 6u) +
                      (result >> 2u);
            result ^= static_cast<std::size_t>(value.z) +
                      UINT64_C(0x9e3779b97f4a7c15) + (result << 6u) +
                      (result >> 2u);
            return result;
        }
    };
    const float weld_tolerance =
        std::max(1.0e-5f, settings.visual_job.voxel_m * 1.0e-4f);
    const float weld_tolerance_squared = weld_tolerance * weld_tolerance;
    std::unordered_map<WeldKey, std::vector<std::uint32_t>, WeldKeyHash>
        weld_buckets;

    const auto owns_triangle = [&](const CellRange& owned,
                                   const gpu_meshing::MeshResult& chunk,
                                   std::size_t triangle) {
        matter::Float3 centroid{};
        for (std::size_t corner = 0; corner != 3u; ++corner) {
            const std::uint32_t vertex =
                chunk.indices[triangle * 3u + corner];
            centroid.x += chunk.positions[vertex * 3u + 0u] / 3.0f;
            centroid.y += chunk.positions[vertex * 3u + 1u] / 3.0f;
            centroid.z += chunk.positions[vertex * 3u + 2u] / 3.0f;
        }
        for (std::size_t axis = 0; axis != 3u; ++axis) {
            const float relative =
                (coordinate(centroid, axis) -
                 coordinate(root_layout.origin_m, axis)) /
                coordinate(root_layout.spacing_m, axis);
            const auto last = root_layout.cell_dims[axis] - 1u;
            const std::uint32_t cell = relative <= 0.0f
                ? 0u
                : std::min(last, static_cast<std::uint32_t>(
                                      std::floor(relative)));
            if (cell < owned.begin[axis] || cell >= owned.end[axis])
                return false;
        }
        return true;
    };

    const auto weld_vertex = [&](const gpu_meshing::MeshResult& chunk,
                                 std::uint32_t source_vertex,
                                 std::uint32_t& destination_vertex) {
        const matter::Float3 position{
            chunk.positions[source_vertex * 3u + 0u],
            chunk.positions[source_vertex * 3u + 1u],
            chunk.positions[source_vertex * 3u + 2u]};
        const WeldKey base{
            static_cast<std::int64_t>(std::floor(position.x / weld_tolerance)),
            static_cast<std::int64_t>(std::floor(position.y / weld_tolerance)),
            static_cast<std::int64_t>(std::floor(position.z / weld_tolerance))};
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const auto found = weld_buckets.find(
                        {base.x + dx, base.y + dy, base.z + dz});
                    if (found == weld_buckets.end()) continue;
                    for (const std::uint32_t candidate : found->second) {
                        const float delta_x =
                            merged.positions[candidate * 3u + 0u] - position.x;
                        const float delta_y =
                            merged.positions[candidate * 3u + 1u] - position.y;
                        const float delta_z =
                            merged.positions[candidate * 3u + 2u] - position.z;
                        if (delta_x * delta_x + delta_y * delta_y +
                                delta_z * delta_z <= weld_tolerance_squared) {
                            destination_vertex = candidate;
                            merged.normals[candidate * 3u + 0u] +=
                                chunk.normals[source_vertex * 3u + 0u];
                            merged.normals[candidate * 3u + 1u] +=
                                chunk.normals[source_vertex * 3u + 1u];
                            merged.normals[candidate * 3u + 2u] +=
                                chunk.normals[source_vertex * 3u + 2u];
                            return true;
                        }
                    }
                }
            }
        }
        const std::size_t vertex_count = merged.positions.size() / 3u;
        if (vertex_count >= settings.visual_job.limits.max_mesh_vertices ||
            vertex_count >= std::numeric_limits<std::uint32_t>::max()) {
            error = {gpu_meshing::ErrorCode::LimitExceeded,
                     "particle-water chunks exceed the global vertex limit"};
            return false;
        }
        destination_vertex = static_cast<std::uint32_t>(vertex_count);
        merged.positions.insert(merged.positions.end(),
                                {position.x, position.y, position.z});
        merged.normals.insert(
            merged.normals.end(),
            {chunk.normals[source_vertex * 3u + 0u],
             chunk.normals[source_vertex * 3u + 1u],
             chunk.normals[source_vertex * 3u + 2u]});
        weld_buckets[base].push_back(destination_vertex);
        return true;
    };

    const auto append_owned_triangles = [&](const CellRange& owned,
                                            const gpu_meshing::MeshResult& chunk) {
        if (chunk.material != 4u || chunk.positions.size() % 3u != 0u ||
            chunk.normals.size() != chunk.positions.size() ||
            chunk.indices.size() % 3u != 0u) {
            error = {gpu_meshing::ErrorCode::ArtifactFailure,
                     "particle-water mesher returned malformed chunk geometry"};
            return false;
        }
        const std::size_t chunk_vertices = chunk.positions.size() / 3u;
        for (const std::uint32_t index : chunk.indices) {
            if (index >= chunk_vertices) {
                error = {gpu_meshing::ErrorCode::ArtifactFailure,
                         "particle-water mesher returned an invalid chunk index"};
                return false;
            }
        }
        for (std::size_t triangle = 0;
             triangle != chunk.indices.size() / 3u; ++triangle) {
            if (!owns_triangle(owned, chunk, triangle)) continue;
            if (merged.indices.size() + 3u >
                settings.visual_job.limits.max_mesh_indices) {
                error = {gpu_meshing::ErrorCode::LimitExceeded,
                         "particle-water chunks exceed the global index limit"};
                return false;
            }
            for (std::size_t corner = 0; corner != 3u; ++corner) {
                std::uint32_t welded = 0u;
                if (!weld_vertex(
                        chunk, chunk.indices[triangle * 3u + corner], welded))
                    return false;
                merged.indices.push_back(welded);
            }
        }
        return true;
    };

    std::function<bool(const CellRange&)> build_chunk;
    build_chunk = [&](const CellRange& owned) {
        const auto split_chunk = [&]() {
            std::size_t axis = 0u;
            std::uint32_t longest = 0u;
            for (std::size_t candidate = 0; candidate != 3u; ++candidate) {
                const std::uint32_t cells =
                    owned.end[candidate] - owned.begin[candidate];
                if (cells > longest) {
                    longest = cells;
                    axis = candidate;
                }
            }
            if (longest <= 1u) {
                error = {gpu_meshing::ErrorCode::LimitExceeded,
                         "particle-water meshing cannot split its last grid cell"};
                return false;
            }
            const std::uint32_t split =
                owned.begin[axis] + longest / 2u;
            CellRange left = owned;
            CellRange right = owned;
            left.end[axis] = split;
            right.begin[axis] = split;
            return build_chunk(left) && build_chunk(right);
        };

        CellRange mesh_range = owned;
        for (std::size_t axis = 0; axis != 3u; ++axis) {
            if (mesh_range.begin[axis] != 0u) --mesh_range.begin[axis];
            if (mesh_range.end[axis] != root_layout.cell_dims[axis])
                ++mesh_range.end[axis];
        }
        gpu_meshing::ParticleJob visual_template = settings.visual_job;
        for (std::size_t axis = 0; axis != 3u; ++axis) {
            set_coordinate(visual_template.bounds_m.min_m, axis,
                           grid_coordinate(axis, mesh_range.begin[axis]));
            set_coordinate(visual_template.bounds_m.max_m, axis,
                           grid_coordinate(axis, mesh_range.end[axis]));
        }

        std::vector<FluidParticle> halo_particles;
        halo_particles.reserve(source.size());
        const float halo = root_layout.query_radius_m;
        for (const FluidParticle& particle : source) {
            bool relevant = true;
            for (std::size_t axis = 0; axis != 3u; ++axis) {
                const float value = coordinate(particle.position_m, axis);
                if (value < coordinate(visual_template.bounds_m.min_m, axis) - halo ||
                    value > coordinate(visual_template.bounds_m.max_m, axis) + halo) {
                    relevant = false;
                    break;
                }
            }
            if (relevant) halo_particles.push_back(particle);
        }
        if (halo_particles.empty()) return true;

        std::vector<gpu_meshing::ParticleSample> job_particles;
        gpu_meshing::ParticleJob job{};
        gpu_meshing::Error job_error{};
        if (!make_fluid_particle_job(
                halo_particles, settings.particle_radius_m, visual_template,
                job_particles, job, job_error)) {
            if (job_error.code == gpu_meshing::ErrorCode::LimitExceeded)
                return split_chunk();
            error = std::move(job_error);
            return false;
        }
        gpu_meshing::MeshResult chunk{};
        gpu_meshing::Stats stats{};
        if (!visual_mesher(job, chunk, stats, error, {})) {
            if (error.code == gpu_meshing::ErrorCode::LimitExceeded)
                return split_chunk();
            return false;
        }
        return append_owned_triangles(owned, chunk);
    };

    CellRange root_range{};
    root_range.end = root_layout.cell_dims;
    if (!build_chunk(root_range)) {
        merged = {};
        return false;
    }
    for (std::size_t vertex = 0; vertex != merged.normals.size() / 3u;
         ++vertex) {
        const float x = merged.normals[vertex * 3u + 0u];
        const float y = merged.normals[vertex * 3u + 1u];
        const float z = merged.normals[vertex * 3u + 2u];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length > 1.0e-8f) {
            merged.normals[vertex * 3u + 0u] = x / length;
            merged.normals[vertex * 3u + 1u] = y / length;
            merged.normals[vertex * 3u + 2u] = z / length;
        }
    }
    error = {};
    merged.content_digest = gpu_meshing::mesh_content_digest(merged);
    return true;
}

}  // namespace

bool PhysxFluidBake::run(const FluidBakeInput& input,
                         IFluidBakeBackend& backend,
                         const FluidBakeCallbacks& callbacks,
                         FluidBakeOutput& output,
                         FluidBakeError& error) noexcept {
    output = {};
    error = {};
    try {
        if (!validate_input(input, output, error)) return false;
        if (callbacks.cancelled && callbacks.cancelled()) {
            return fail(FluidBakeCode::Cancelled,
                        "fluid bake was cancelled before backend probe",
                        output, error);
        }

        const FluidBackendProbe probe = backend.probe();
        if (!probe.available) {
            return fail(FluidBakeCode::BackendUnavailable,
                        probe.message.empty() ? "fluid backend is unavailable"
                                              : probe.message.c_str(),
                        output, error);
        }

        bool callback_failed = false;
        bool cancellation_seen = false;
        bool progress_failed = false;
        bool saw_progress = false;
        std::uint32_t last_step = 0u;
        FluidBakeCallbacks guarded{};
        guarded.cancelled = [&]() {
            if (!callbacks.cancelled) return false;
            try {
                const bool cancelled = callbacks.cancelled();
                cancellation_seen = cancellation_seen || cancelled;
                return cancelled;
            } catch (...) {
                callback_failed = true;
                return true;
            }
        };
        guarded.progress = [&](const FluidBakeProgress& progress) {
            const bool valid =
                progress.total_steps == input.settings.max_steps &&
                progress.completed_steps <= progress.total_steps &&
                progress.active_particles <= input.settings.max_particles &&
                finite(progress.sensor_wet_fraction) &&
                progress.sensor_wet_fraction >= 0.0f &&
                progress.sensor_wet_fraction <= 1.0f &&
                (!saw_progress || progress.completed_steps >= last_step);
            if (!valid) {
                progress_failed = true;
                return;
            }
            saw_progress = true;
            last_step = progress.completed_steps;
            if (callbacks.progress) {
                try {
                    callbacks.progress(progress);
                } catch (...) {
                    callback_failed = true;
                }
            }
        };

        FluidBakeOutput candidate{};
        FluidBakeError backend_error{};
        if (!backend.run(input, guarded, candidate, backend_error)) {
            if (callback_failed) {
                return fail(FluidBakeCode::BackendFailure,
                            "fluid bake callback raised an exception",
                            output, error);
            }
            if (cancellation_seen) {
                return fail(FluidBakeCode::Cancelled,
                            "fluid bake cancellation was observed", output,
                            error);
            }
            const FluidBakeCode code =
                backend_error.code == FluidBakeCode::Ready
                    ? FluidBakeCode::BackendFailure
                    : backend_error.code;
            if ((code == FluidBakeCode::SensorNotReached ||
                 code == FluidBakeCode::Escaped) &&
                validate_failed_debug_snapshot(
                    input, code, candidate, error)) {
                output = std::move(candidate);
                error = {code,
                         backend_error.message.empty()
                             ? "fluid backend returned a finite terminal-failure snapshot"
                             : backend_error.message};
                return false;
            }
            return fail(code,
                        backend_error.message.empty()
                            ? "fluid backend failed without a diagnostic"
                            : backend_error.message.c_str(),
                        output, error);
        }
        if (callback_failed) {
            return fail(FluidBakeCode::BackendFailure,
                        "fluid bake callback raised an exception", output,
                        error);
        }
        if (cancellation_seen) {
            return fail(FluidBakeCode::Cancelled,
                        "fluid bake cancellation was observed", output,
                        error);
        }
        if (progress_failed) {
            return fail(FluidBakeCode::BackendFailure,
                        "fluid backend violated the progress contract",
                        output, error);
        }
        output = std::move(candidate);
        if (!validate_output(input, output, error)) return false;
        error = {};
        return true;
    } catch (const std::exception& exception) {
        return fail(FluidBakeCode::BackendFailure, exception.what(), output,
                    error);
    } catch (...) {
        return fail(FluidBakeCode::BackendFailure,
                    "fluid backend raised an unknown exception", output,
                    error);
    }
}

gpu_meshing::ParticleJob PhysxFluidBake::resolved_visual_job(
    const std::vector<FluidParticle>& particles, float particle_radius_m,
    const gpu_meshing::ParticleJob& authored) {
    return tight_visual_job(particles, particle_radius_m, authored);
}

bool PhysxFluidBake::build_failed_debug_visual(
    const FluidBakeOutput& output, FluidBakeCode failure_code,
    const ProductBuildSettings& settings, const VisualMesher& visual_mesher,
    gpu_meshing::MeshResult& visual, FluidBakeError& error) noexcept {
    visual = {};
    error = {};
    try {
        if ((failure_code != FluidBakeCode::SensorNotReached &&
             failure_code != FluidBakeCode::Escaped) ||
            !visual_mesher || output.particles.empty() ||
            output.stats.non_finite_particles != 0u ||
            output.stats.active_particles != output.particles.size() ||
            output.stats.emitted_particles !=
                output.stats.active_particles + output.stats.retired_particles ||
            output.stats.escaped_particles > output.stats.retired_particles ||
            output.stats.escaped_particles !=
                output.quarantined_particles.size() ||
            output.stats.escape_budget !=
                fluid_escape_budget(output.stats.emitted_particles,
                                    output.stats.escape_policy)) {
            error = {FluidBakeCode::ProductFailure,
                     "debug water requires a finite diagnostic terminal snapshot"};
            return false;
        }
        for (const FluidParticle& particle : output.particles) {
            if (!finite(particle.position_m) ||
                !finite(particle.velocity_mps)) {
                error = {FluidBakeCode::NonFinite,
                         "debug water refuses non-finite particles"};
                return false;
            }
        }
        gpu_meshing::Error mesher_error{};
        if (!build_particle_visual_chunks(
                output.particles, settings, visual_mesher, visual,
                mesher_error) ||
            visual.positions.empty() || visual.indices.empty() ||
            visual.material != 4u) {
            visual = {};
            error = {
                FluidBakeCode::ProductFailure,
                mesher_error.message.empty()
                    ? "failed-bake debug visual meshing returned no material-4 water"
                    : mesher_error.message};
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        visual = {};
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        visual = {};
        error = {FluidBakeCode::ProductFailure,
                 "failed-bake debug visual construction raised an unknown exception"};
        return false;
    }
}

bool PhysxFluidBake::build_failed_debug_visual_on_renderer(
    const FluidBakeOutput& output, FluidBakeCode failure_code,
    const ProductBuildSettings& settings, const GpuRunner& gpu_run,
    const VisualMesher& vk_particle_visual_bake,
    gpu_meshing::MeshResult& visual, FluidBakeError& error) noexcept {
    if (!vk_particle_visual_bake) {
        visual = {};
        error = {FluidBakeCode::ProductFailure,
                 "Vulkan particle-water visual mesher is unavailable"};
        return false;
    }
    const VisualMesher visual_mesher =
        [gpu_run, vk_particle_visual_bake](
            const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& result, gpu_meshing::Stats& stats,
            gpu_meshing::Error& mesher_error,
            const gpu_meshing::BuildControl& control) {
            std::string run_error;
            const auto invoke = [&](std::string&) {
                return vk_particle_visual_bake(
                    job, result, stats, mesher_error, control);
            };
            const bool completed = gpu_run
                ? gpu_run("hydrology_failed_debug_visual", invoke, run_error)
                : invoke(run_error);
            if (!completed && mesher_error.message.empty()) {
                mesher_error.code = gpu_meshing::ErrorCode::VulkanFailure;
                mesher_error.message = run_error.empty()
                    ? "Vulkan failed-bake debug visual meshing failed"
                    : run_error;
            }
            return completed;
        };
    return build_failed_debug_visual(
        output, failure_code, settings, visual_mesher, visual, error);
}

bool PhysxFluidBake::build_accepted_artifact(
    const FluidBakeOutput& output, const ProductBuildSettings& settings,
    const TerrainHeightSampler& terrain, const VisualMesher& visual_mesher,
    HydrologyArtifact& artifact, FluidBakeError& error,
    ProductBuildTimings* timings) noexcept {
    artifact = {};
    error = {};
    if (timings) *timings = {};
    try {
        if (!output.sensor.complete || output.stats.active_particles != output.particles.size() ||
            output.stats.peak_particles < output.stats.active_particles ||
            output.stats.escaped_particles > output.stats.escape_budget ||
            output.stats.emitted_particles !=
                output.stats.active_particles +
                    output.stats.retired_particles ||
            output.stats.escaped_particles > output.stats.retired_particles ||
            output.stats.non_finite_particles != 0u || !visual_mesher) {
            error = {FluidBakeCode::ProductFailure,
                     "fluid products require an accepted simulation and visual mesher"};
            return false;
        }
        if (settings.semantic.physx_sdk_version == 0u ||
            settings.semantic.adapter_version == 0u ||
            settings.provenance.gpu_vendor == 0u ||
            settings.provenance.gpu_device == 0u ||
            settings.provenance.driver_version == 0u ||
            settings.provenance.physx_sdk_version !=
                settings.semantic.physx_sdk_version ||
            settings.provenance.adapter_version !=
                settings.semantic.adapter_version) {
            error = {FluidBakeCode::ProductFailure,
                     "fluid product provenance must match the semantic PhysX and adapter versions"};
            return false;
        }
        for (std::size_t index = 1; index < output.particles.size(); ++index) {
            if (output.particles[index - 1].id >= output.particles[index].id) {
                error = {FluidBakeCode::ProductFailure,
                         "fluid products require a stable-id-sorted particle snapshot"};
                return false;
            }
        }
        std::vector<gpu_meshing::ParticleSample> particles;
        gpu_meshing::ParticleJob job{};
        gpu_meshing::Error mesher_error{};
        const gpu_meshing::ParticleJob visual_template = tight_visual_job(
            output.particles, settings.particle_radius_m,
            settings.visual_job);
        // The canonical full-envelope job supplies product identity and the
        // coarse CPU query product.  Its fine visual grid may legitimately be
        // larger than one dispatch, so validate that envelope with a relaxed
        // grid cap and let build_particle_visual_chunks enforce the authored
        // per-dispatch and final-mesh limits below.
        gpu_meshing::ParticleJob envelope_template = visual_template;
        envelope_template.limits.max_grid_vertices =
            std::numeric_limits<std::uint32_t>::max();
        if (!make_fluid_particle_job(output.particles, settings.particle_radius_m,
                                    envelope_template, particles, job,
                                    mesher_error)) {
            error = {FluidBakeCode::ProductFailure, mesher_error.message};
            return false;
        }
        job.limits = settings.visual_job.limits;
        gpu_meshing::MeshResult visual{};
        const auto gpu_mesh_start = std::chrono::steady_clock::now();
        if (!build_particle_visual_chunks(
                output.particles, settings, visual_mesher, visual,
                mesher_error)) {
            if (timings)
                timings->gpu_mesh_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - gpu_mesh_start).count();
            error = {FluidBakeCode::ProductFailure,
                     mesher_error.message.empty() ? "Matter GPU visual meshing failed"
                                                   : mesher_error.message};
            return false;
        }
        if (timings)
            timings->gpu_mesh_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - gpu_mesh_start).count();
        if (visual.positions.empty() || visual.indices.empty() ||
            visual.material != 4u) {
            error = {FluidBakeCode::ProductFailure,
                     "Matter GPU visual meshing returned an empty required product"};
            return false;
        }
        gpu_meshing::MeshResult coarse{};
        gpu_meshing::ParticleJob coarse_job = job;
        coarse_job.voxel_m = settings.coarse_voxel_m;
        const auto cpu_mesh_start = std::chrono::steady_clock::now();
        if (!build_cpu_particle_visual(coarse_job, settings.coarse_voxel_m, coarse,
                                       mesher_error)) {
            if (timings)
                timings->cpu_mesh_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - cpu_mesh_start).count();
            error = {FluidBakeCode::ProductFailure,
                     mesher_error.message.empty() ? "Matter CPU query meshing failed"
                                                   : mesher_error.message};
            return false;
        }
        if (timings)
            timings->cpu_mesh_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpu_mesh_start).count();
        if (coarse.positions.empty() || coarse.indices.empty() ||
            coarse.material != 4u) {
            error = {FluidBakeCode::ProductFailure,
                     "Matter CPU query meshing returned an empty required product"};
            return false;
        }
        std::vector<GameplaySample> gameplay;
        std::string gameplay_error;
        if (!build_fluid_gameplay_field(output.particles, settings.particle_radius_m,
                                        settings.gameplay_layout, terrain,
                                        gameplay, gameplay_error)) {
            error = {FluidBakeCode::ProductFailure, gameplay_error};
            return false;
        }
        if (std::none_of(gameplay.begin(), gameplay.end(),
                         [](const GameplaySample& sample) { return sample.wet_valid; })) {
            error = {FluidBakeCode::ProductFailure,
                     "fluid gameplay field contains no wet samples"};
            return false;
        }
        const std::uint64_t snapshot = fluid_particle_snapshot_digest(
            output.particles, settings.particle_radius_m);
        HydrologyArtifact candidate{};
        candidate.section = settings.section;
        candidate.semantic_key = derive_hydrology_semantic_key(settings.semantic);
        ProductIdentitySettings identity = settings.identity;
        identity.semantic_key = candidate.semantic_key;
        candidate.product_keys = derive_product_keys(
            job, snapshot, identity, settings.coarse_voxel_m,
            settings.gameplay_layout);
        candidate.particle_snapshot_digest = snapshot;
        candidate.particle_radius_m = settings.particle_radius_m;
        candidate.accepted = true;
        candidate.stats = output.stats;
        candidate.sensor = output.sensor;
        candidate.particles = output.particles;
        candidate.visual_mesh = std::move(visual);
        candidate.coarse_cpu_mesh = std::move(coarse);
        candidate.gameplay_layout = settings.gameplay_layout;
        candidate.gameplay_field = std::move(gameplay);
        candidate.provenance = settings.provenance;
        artifact = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        error = {FluidBakeCode::ProductFailure,
                 "fluid product construction raised an unknown exception"};
        return false;
    }
}

bool PhysxFluidBake::build_accepted_artifact_on_renderer(
    const FluidBakeOutput& output, const ProductBuildSettings& settings,
    const TerrainHeightSampler& terrain, const GpuRunner& gpu_run,
    const VisualMesher& vk_particle_visual_bake, HydrologyArtifact& artifact,
    FluidBakeError& error, ProductBuildTimings* timings) noexcept {
    artifact = {};
    error = {};
    if (!vk_particle_visual_bake) {
        error = {FluidBakeCode::ProductFailure,
                 "Vulkan particle-water visual mesher is unavailable"};
        return false;
    }
    const VisualMesher visual_mesher =
        [gpu_run, vk_particle_visual_bake](
            const gpu_meshing::ParticleJob& job, gpu_meshing::MeshResult& result,
            gpu_meshing::Stats& stats, gpu_meshing::Error& mesher_error,
            const gpu_meshing::BuildControl& control) {
            std::string run_error;
            const auto invoke = [&](std::string&) {
                return vk_particle_visual_bake(job, result, stats,
                                                mesher_error, control);
            };
            const bool completed = gpu_run
                ? gpu_run("hydrology_particle_visual", invoke, run_error)
                : invoke(run_error);
            if (!completed && mesher_error.message.empty()) {
                mesher_error.code = gpu_meshing::ErrorCode::VulkanFailure;
                mesher_error.message = run_error.empty()
                    ? "Vulkan particle-water visual meshing failed"
                    : run_error;
            }
            return completed;
        };
    return build_accepted_artifact(output, settings, terrain, visual_mesher,
                                   artifact, error, timings);
}

}  // namespace hydrology
