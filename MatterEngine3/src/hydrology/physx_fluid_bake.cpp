#include "hydrology/physx_fluid_bake.h"

#include <algorithm>
#include <cmath>
#include <exception>
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

bool fail(FluidBakeCode code, const char* message,
          FluidBakeOutput& output, FluidBakeError& error) {
    output = {};
    error = {code, message};
    return false;
}

bool validate_input(const FluidBakeInput& input,
                    FluidBakeOutput& output, FluidBakeError& error) {
    if (!finite(input.network.cell_size_m) ||
        input.network.cell_size_m <= 0.0f || input.network.rivers.empty() ||
        input.network.first_section_river.empty()) {
        return fail(FluidBakeCode::InvalidInput,
                    "river network is incomplete", output, error);
    }
    for (const auto& river : input.network.rivers) {
        if (river.name.empty() || river.spline.size() < 2u ||
            !finite(river.inlet.position_m) ||
            !finite(river.inlet.flow_m3s) || river.inlet.flow_m3s <= 0.0f) {
            return fail(FluidBakeCode::InvalidInput,
                        "river definition is invalid", output, error);
        }
        for (const auto& point : river.spline) {
            if (!finite(point)) {
                return fail(FluidBakeCode::InvalidInput,
                            "river spline contains a non-finite point",
                            output, error);
            }
        }
        for (const auto& reach : river.reaches) {
            if (!finite(reach.until_m) || !finite(reach.base_grade) ||
                !finite(reach.meander) || !finite(reach.width_scale)) {
                return fail(FluidBakeCode::InvalidInput,
                            "river reach contains a non-finite value",
                            output, error);
            }
        }
        if (!finite(river.channel.width_m) ||
            !finite(river.channel.depth_m) ||
            !finite(river.channel.asymmetry) ||
            !finite(river.boulders.density) ||
            !finite(river.boulders.radius_m)) {
            return fail(FluidBakeCode::InvalidInput,
                        "river channel or boulder settings are non-finite",
                        output, error);
        }
    }
    const auto& first_section = input.network.first_section;
    if (!finite(first_section.minimum_length_m) ||
        !finite(first_section.dry_margin_m) ||
        !finite(first_section.crest_wet_fraction)) {
        return fail(FluidBakeCode::InvalidInput,
                    "first-section settings contain a non-finite value",
                    output, error);
    }
    if (input.geometry.centreline.size() < 2u ||
        !valid_bounds(input.geometry.bounds_m)) {
        return fail(FluidBakeCode::InvalidInput,
                    "river geometry is incomplete", output, error);
    }
    for (const auto& sample : input.geometry.centreline) {
        if (!finite(sample.position_m) || !finite(sample.tangent) ||
            !finite(sample.lateral) || !finite(sample.distance_m) ||
            !finite(sample.grade) || !finite(sample.meander) ||
            !finite(sample.width_scale)) {
            return fail(FluidBakeCode::InvalidInput,
                        "river geometry contains a non-finite sample",
                        output, error);
        }
    }
    for (const auto& boulder : input.geometry.boulders) {
        if (!finite(boulder.center_m) || !finite(boulder.radius_m)) {
            return fail(FluidBakeCode::InvalidInput,
                        "river geometry contains a non-finite boulder",
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
        const float direction_length_sq =
            emitter.direction.x * emitter.direction.x +
            emitter.direction.y * emitter.direction.y +
            emitter.direction.z * emitter.direction.z;
        if (!emitter_ids.insert(emitter.id).second ||
            !finite(emitter.position_m) || !finite(emitter.direction) ||
            !finite(emitter.initial_velocity_mps) ||
            !finite(direction_length_sq) || direction_length_sq <= 0.0f ||
            !finite(emitter.flow_m3s) || emitter.flow_m3s <= 0.0f ||
            !finite(emitter.radius_m) || emitter.radius_m <= 0.0f ||
            emitter.start_step >= emitter.stop_step) {
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
        settings.max_particles == 0u) {
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
        output.stats.peak_particles > input.settings.max_particles) {
        return fail(FluidBakeCode::CapacityExceeded,
                    "backend particle counts exceed the declared capacity",
                    output, error);
    }
    if (output.stats.active_particles != output.particles.size() ||
        output.stats.peak_particles < output.stats.active_particles) {
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
    if (output.stats.escaped_particles != 0u) {
        return fail(FluidBakeCode::Escaped,
                    "backend reported escaped particles", output, error);
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
        return fail(FluidBakeCode::SensorNotReached,
                    "backend did not satisfy the fill sensor contract",
                    output, error);
    }
    for (const auto& particle : output.particles) {
        if (!finite(particle.position_m) || !finite(particle.velocity_mps)) {
            return fail(FluidBakeCode::NonFinite,
                        "backend output contains a non-finite particle",
                        output, error);
        }
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

bool PhysxFluidBake::build_accepted_artifact(
    const FluidBakeOutput& output, const ProductBuildSettings& settings,
    const TerrainHeightSampler& terrain, const VisualMesher& visual_mesher,
    HydrologyArtifact& artifact, FluidBakeError& error) noexcept {
    artifact = {};
    error = {};
    try {
        if (!output.sensor.complete || output.stats.active_particles != output.particles.size() ||
            output.stats.peak_particles < output.stats.active_particles ||
            output.stats.escaped_particles != 0u ||
            output.stats.non_finite_particles != 0u || !visual_mesher) {
            error = {FluidBakeCode::ProductFailure,
                     "fluid products require an accepted simulation and visual mesher"};
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
        if (!make_fluid_particle_job(output.particles, settings.particle_radius_m,
                                    settings.visual_job, particles, job,
                                    mesher_error)) {
            error = {FluidBakeCode::ProductFailure, mesher_error.message};
            return false;
        }
        gpu_meshing::Stats visual_stats{};
        gpu_meshing::MeshResult visual{};
        if (!visual_mesher(job, visual, visual_stats, mesher_error, {})) {
            error = {FluidBakeCode::ProductFailure,
                     mesher_error.message.empty() ? "Matter GPU visual meshing failed"
                                                   : mesher_error.message};
            return false;
        }
        if (visual.positions.empty() || visual.indices.empty() ||
            visual.material != 4u) {
            error = {FluidBakeCode::ProductFailure,
                     "Matter GPU visual meshing returned an empty required product"};
            return false;
        }
        gpu_meshing::MeshResult coarse{};
        if (!build_cpu_particle_visual(job, settings.coarse_voxel_m, coarse,
                                       mesher_error)) {
            error = {FluidBakeCode::ProductFailure,
                     mesher_error.message.empty() ? "Matter CPU query meshing failed"
                                                   : mesher_error.message};
            return false;
        }
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
        candidate.semantic_key = derive_hydrology_semantic_key(settings.semantic);
        ProductIdentitySettings identity = settings.identity;
        identity.semantic_key = candidate.semantic_key;
        candidate.product_keys = derive_product_keys(job, snapshot, identity);
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

}  // namespace hydrology
