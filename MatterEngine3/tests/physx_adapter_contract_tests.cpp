#include "check.h"

#include "hydrology/fill_sensor.h"
#include "hydrology/fluid_emission.h"
#include "hydrology/physx_collision_input.h"
#include "hydrology/physx_fluid_bake.h"
#if defined(MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
#include "matter/engine_context.h"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using hydrology::FluidBakeCallbacks;
using hydrology::FluidBakeCode;
using hydrology::FluidBakeError;
using hydrology::FluidBakeInput;
using hydrology::FluidBakeOutput;
using hydrology::FluidBakeProgress;
using hydrology::FluidBackendProbe;
using hydrology::FluidCollisionBuildInput;
using hydrology::FluidCollisionBuildOutput;
using hydrology::FluidCollisionSurface;
using hydrology::FluidCollisionSurfaceKind;
using hydrology::IFluidBakeBackend;

matter::Mat4f identity_transform(float translate_x = 0.0f) {
    matter::Mat4f result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    result.m[3] = translate_x;
    return result;
}

FluidCollisionSurface triangle_surface(FluidCollisionSurfaceKind kind) {
    FluidCollisionSurface surface{};
    surface.kind = kind;
    surface.local_to_world = identity_transform();
    surface.mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    surface.mesh.indices = {0u, 1u, 2u};
    return surface;
}

FluidCollisionBuildInput collision_build_input() {
    FluidCollisionBuildInput input{};
    input.section_bounds_m = {{0.0f, -1.0f, 0.0f},
                              {10.0f, 5.0f, 10.0f}};
    input.dry_margin_m = 2.0f;
    return input;
}

void test_collision_assembly_deduplicates_without_changing_winding() {
    auto input = collision_build_input();
    input.surfaces.push_back(
        triangle_surface(FluidCollisionSurfaceKind::Terrain));
    auto reversed = triangle_surface(FluidCollisionSurfaceKind::Boulder);
    reversed.mesh.indices = {0u, 2u, 1u};
    input.surfaces.push_back(std::move(reversed));

    FluidCollisionBuildOutput output{};
    FluidBakeError error{};
    CHECK(hydrology::build_physx_collision_input(input, output, error),
          error.message.c_str());
    CHECK(output.mesh.vertices.size() == 3u,
          "coincident world-space vertices are deduplicated");
    CHECK(output.mesh.indices ==
              std::vector<std::uint32_t>({0u, 1u, 2u, 0u, 2u, 1u}),
          "vertex deduplication preserves each authored triangle winding");
    CHECK(output.ranges.size() == 2u &&
              output.ranges[0].kind == FluidCollisionSurfaceKind::Terrain &&
              output.ranges[0].first_index == 0u &&
              output.ranges[0].index_count == 3u &&
              output.ranges[1].kind == FluidCollisionSurfaceKind::Boulder &&
              output.ranges[1].first_index == 3u &&
              output.ranges[1].index_count == 3u,
          "authored collision source tags survive assembly");
}

void test_collision_assembly_rejects_invalid_geometry_and_transforms() {
    auto expect_invalid = [](FluidCollisionBuildInput input,
                             const char* message) {
        FluidCollisionBuildOutput output{};
        output.mesh.vertices.push_back({99.0f, 99.0f, 99.0f});
        FluidBakeError error{};
        CHECK(!hydrology::build_physx_collision_input(input, output, error),
              message);
        CHECK(error.code == FluidBakeCode::InvalidInput &&
                  output.mesh.vertices.empty() && output.ranges.empty(),
              "invalid collision input clears output and reports InvalidInput");
    };

    auto input = collision_build_input();
    auto surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.mesh.indices.push_back(0u);
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "partial collision triangle is rejected");

    input = collision_build_input();
    surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.mesh.indices[2] = 99u;
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "out-of-range collision index is rejected");

    input = collision_build_input();
    surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.mesh.vertices[2] = {2.0f, 0.0f, 0.0f};
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "degenerate collision triangle is rejected");

    input = collision_build_input();
    surface = triangle_surface(FluidCollisionSurfaceKind::Terrain);
    surface.local_to_world.m[6] =
        std::numeric_limits<float>::quiet_NaN();
    input.surfaces.push_back(std::move(surface));
    expect_invalid(std::move(input), "non-finite collision transform is rejected");
}

void test_collision_bounds_and_virtual_dam_do_not_create_hidden_walls() {
    auto input = collision_build_input();
    input.surfaces.push_back(
        triangle_surface(FluidCollisionSurfaceKind::Terrain));
    auto dam = triangle_surface(FluidCollisionSurfaceKind::VirtualDam);
    dam.local_to_world = identity_transform(4.0f);
    input.surfaces.push_back(std::move(dam));

    FluidCollisionBuildOutput output{};
    FluidBakeError error{};
    CHECK(hydrology::build_physx_collision_input(input, output, error),
          error.message.c_str());
    CHECK(output.mesh.indices.size() == 6u &&
              output.authored_triangle_count == 2u,
          "assembly emits exactly the two authored triangles and no AABB walls");
    CHECK(output.ranges.size() == 2u &&
              output.ranges[1].kind == FluidCollisionSurfaceKind::VirtualDam,
          "the downstream virtual dam remains explicitly tagged");
    CHECK(output.dry_collar_bounds_m.minimum.x == -2.0f &&
              output.dry_collar_bounds_m.minimum.y == -3.0f &&
              output.dry_collar_bounds_m.minimum.z == -2.0f &&
              output.dry_collar_bounds_m.maximum.x == 12.0f &&
              output.dry_collar_bounds_m.maximum.y == 7.0f &&
              output.dry_collar_bounds_m.maximum.z == 12.0f,
          "dry collar is derived from authored section bounds and margin");
}

void test_emission_fractional_carry_boundaries_and_stable_ids() {
    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f;
    settings.max_particles = 32u;
    const float particle_volume =
        hydrology::physx_particle_volume_m3(settings.particle_spacing_m);

    hydrology::FluidEmitter main{};
    main.id = 7u;
    main.flow_m3s = 2.5f * particle_volume;
    main.radius_m = 1.0f;
    main.direction = {0.0f, 0.0f, 1.0f};
    main.start_step = 0u;
    main.stop_step = 4u;
    hydrology::FluidEmitter tributary = main;
    tributary.id = 3u;
    tributary.flow_m3s = 1.5f * particle_volume;
    tributary.start_step = 1u;
    tributary.stop_step = 3u;

    hydrology::FluidEmissionState state{};
    hydrology::FluidBakeError error{};
    std::vector<hydrology::FluidParticleActivation> activations;
    std::vector<std::uint32_t> main_counts;
    std::vector<std::uint32_t> tributary_counts;
    std::uint32_t active_count = 0u;
    for (std::uint32_t step = 0u; step != 4u; ++step) {
        CHECK(hydrology::schedule_fluid_emission_step(
                  {main, tributary}, settings, step, active_count,
                  state, activations, error),
              error.message.c_str());
        std::uint32_t main_count = 0u;
        std::uint32_t tributary_count = 0u;
        for (const auto& activation : activations) {
            main_count += activation.emitter_id == main.id ? 1u : 0u;
            tributary_count +=
                activation.emitter_id == tributary.id ? 1u : 0u;
            CHECK(activation.id == active_count + main_count +
                                       tributary_count - 1u,
                  "particle ids remain contiguous in authored-emitter order");
        }
        main_counts.push_back(main_count);
        tributary_counts.push_back(tributary_count);
        active_count += static_cast<std::uint32_t>(activations.size());
    }
    CHECK(main_counts == std::vector<std::uint32_t>({2u, 3u, 2u, 3u}),
          "2.5 particles per step deterministically yields 2,3,2,3");
    CHECK(tributary_counts ==
              std::vector<std::uint32_t>({0u, 1u, 2u, 0u}),
          "each emitter keeps independent carry and exact start/stop bounds");
    CHECK(active_count == 13u && state.next_particle_id == 13u,
          "stable ids cover every activated particle exactly once");
}

void test_emission_capacity_is_checked_before_state_or_output_changes() {
    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f;
    settings.max_particles = 4u;
    hydrology::FluidEmitter emitter{};
    emitter.id = 9u;
    emitter.direction = {0.0f, 0.0f, 1.0f};
    emitter.radius_m = 1.0f;
    emitter.flow_m3s = 2.5f *
        hydrology::physx_particle_volume_m3(settings.particle_spacing_m);
    emitter.start_step = 0u;
    emitter.stop_step = 4u;

    hydrology::FluidEmissionState state{};
    hydrology::FluidBakeError error{};
    std::vector<hydrology::FluidParticleActivation> activations;
    CHECK(hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 0u, 0u, state, activations, error) &&
              activations.size() == 2u,
          "first emission fits capacity");
    const auto state_before_failure = state;
    activations.push_back({});
    CHECK(!hydrology::schedule_fluid_emission_step(
              {emitter}, settings, 1u, 2u, state, activations, error),
          "next emission fails before exceeding particle capacity");
    CHECK(error.code == FluidBakeCode::CapacityExceeded &&
              activations.empty() &&
              state.next_particle_id == state_before_failure.next_particle_id &&
              state.next_step == state_before_failure.next_step &&
              state.fractional_carry == state_before_failure.fractional_carry,
          "capacity failure is transactional for schedule state and writes");
}

void test_emission_rejects_duplicate_ids_before_initializing_state() {
    hydrology::FluidPbdSettings settings{};
    settings.particle_spacing_m = 0.2f;
    settings.fixed_step_seconds = 1.0f;
    settings.max_particles = 8u;
    hydrology::FluidEmitter emitter{};
    emitter.id = 4u;
    emitter.direction = {0.0f, 0.0f, 1.0f};
    emitter.radius_m = 0.5f;
    emitter.flow_m3s = hydrology::physx_particle_volume_m3(0.2f);
    emitter.start_step = 0u;
    emitter.stop_step = 2u;

    hydrology::FluidEmissionState state{};
    hydrology::FluidBakeError error{};
    std::vector<hydrology::FluidParticleActivation> activations;
    CHECK(!hydrology::schedule_fluid_emission_step(
              {emitter, emitter}, settings, 0u, 0u, state, activations,
              error) &&
              error.code == FluidBakeCode::InvalidInput &&
              !state.initialized && activations.empty(),
          "standalone emission scheduling rejects duplicate emitter ids transactionally");
}

std::vector<matter::Float3> sensor_columns(std::uint32_t count,
                                           std::uint32_t contributions) {
    std::vector<matter::Float3> particles;
    for (std::uint32_t column = 0; column < count; ++column) {
        const float x = static_cast<float>(column % 4u) + 0.5f;
        const float z = static_cast<float>(column / 4u) + 0.5f;
        for (std::uint32_t sample = 0; sample < contributions; ++sample) {
            particles.push_back({x, 0.25f + 0.1f * sample, z});
        }
    }
    return particles;
}

void test_fill_sensor_rejects_jets_and_requires_a_consecutive_window() {
    hydrology::FluidFillSensor sensor{};
    sensor.bounds_m = {{0.0f, 0.0f, 0.0f}, {4.0f, 1.0f, 4.0f}};
    sensor.resolution = {4u, 2u, 4u};
    sensor.required_wet_fraction = 0.5f;
    sensor.stable_steps = 3u;
    sensor.minimum_particles_per_cell = 2u;

    hydrology::FillSensorState state{};
    hydrology::FillSensorResult result{};
    hydrology::FluidBakeError error{};
    std::vector<matter::Float3> narrow_jet(40u, {0.5f, 0.5f, 0.5f});
    CHECK(hydrology::update_fill_sensor(
              sensor, narrow_jet, 1u, state, result, error),
          error.message.c_str());
    CHECK(result.wet_fraction == 1.0f / 16.0f &&
              result.stable_steps == 0u && !result.complete,
          "many particles in one column cannot complete a broad sensor");

    const auto broad_wet = sensor_columns(8u, 2u);
    CHECK(hydrology::update_fill_sensor(
              sensor, broad_wet, 2u, state, result, error) &&
              result.wet_fraction == 0.5f && result.stable_steps == 1u,
          "first broad wet sample starts the stable window");
    const auto too_narrow = sensor_columns(7u, 2u);
    CHECK(hydrology::update_fill_sensor(
              sensor, too_narrow, 3u, state, result, error) &&
              result.stable_steps == 0u,
          "one below-threshold sample resets consecutive stability");
    for (std::uint32_t step = 4u; step <= 6u; ++step) {
        CHECK(hydrology::update_fill_sensor(
                  sensor, broad_wet, step, state, result, error),
              error.message.c_str());
    }
    CHECK(result.complete && result.stable_steps == 3u &&
              result.completion_step == 6u &&
              result.first_satisfied_step == 2u &&
              result.maximum_wet_fraction == 0.5f &&
              result.final_wet_fraction == 0.5f &&
              result.stable_window_wet_fraction == 0.5f,
          "sensor completes on the exact third consecutive wet step");

    hydrology::FillSensorState reduced_state{};
    hydrology::FillSensorResult reduced_result{};
    CHECK(hydrology::update_fill_sensor_counts(
              sensor, 8u, 16u, 1u, reduced_state, reduced_result, error) &&
              reduced_result.wet_fraction == 0.5f &&
              reduced_result.stable_steps == 1u,
          "bounded GPU occupancy counts use the same temporal sensor rule");
}

enum class BackendBehavior {
    Succeed,
    Cancel,
    RegressProgress,
    NonFiniteOutput,
    EscapedOutput,
    DuplicateIds,
    IgnoreCancellation,
    Throw,
};

class RecordingBackend final : public IFluidBakeBackend {
public:
    FluidBackendProbe probe() override {
        ++probe_calls;
        if (throw_on_probe) throw std::runtime_error("fake probe exception");
        if (!available) {
            return {false, "fake", "5.6.1", "Fake GPU",
                    FluidBakeCode::BackendUnavailable,
                    "fake backend unavailable"};
        }
        return {true, "fake", "5.6.1", "Fake GPU",
                FluidBakeCode::Ready, {}};
    }

    bool run(const FluidBakeInput& input,
             const FluidBakeCallbacks& callbacks,
             FluidBakeOutput& output,
             FluidBakeError& error) override {
        ++run_calls;
        if (behavior == BackendBehavior::Throw) {
            throw std::runtime_error("fake run exception");
        }
        observed_emitter_ids.clear();
        observed_emitter_velocities.clear();
        for (const auto& emitter : input.emitters) {
            observed_emitter_ids.push_back(emitter.id);
            observed_emitter_velocities.push_back(emitter.initial_velocity_mps);
        }
        if (behavior == BackendBehavior::Cancel && callbacks.cancelled &&
            callbacks.cancelled()) {
            error = {FluidBakeCode::Cancelled, "cancelled in fake backend"};
            return false;
        }
        if (behavior == BackendBehavior::IgnoreCancellation &&
            callbacks.cancelled) {
            (void)callbacks.cancelled();
        }
        if (callbacks.progress) {
            callbacks.progress({1u, input.settings.max_steps, 3u, 0.2f});
            callbacks.progress({behavior == BackendBehavior::RegressProgress
                                    ? 0u
                                    : 2u,
                                input.settings.max_steps, 3u, 0.5f});
        }

        output.particles = {
            {{2.0f, 2.0f, 2.0f}, {0.0f, 0.0f, 1.0f}, 9u},
            {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 2.0f}, 2u},
            {{3.0f, 3.0f, 3.0f}, {0.0f, 0.0f, 3.0f}, 5u},
        };
        output.sensor = {0.75f, 4u, 5u, true,
                         0.8f, 0.75f, 0.75f, 2u};
        output.stats = {5u, 3u, 3u, 0u, 0u, 0.01};
        if (behavior == BackendBehavior::NonFiniteOutput) {
            output.particles[0].velocity_mps.x =
                std::numeric_limits<float>::quiet_NaN();
        }
        if (behavior == BackendBehavior::EscapedOutput) {
            output.particles[0].position_m.x = 1000.0f;
        }
        if (behavior == BackendBehavior::DuplicateIds) {
            output.particles[0].id = output.particles[1].id;
        }
        if (mutate_output) mutate_output(output);
        error = {};
        return true;
    }

    bool available = true;
    bool throw_on_probe = false;
    BackendBehavior behavior = BackendBehavior::Succeed;
    int probe_calls = 0;
    int run_calls = 0;
    std::vector<std::uint32_t> observed_emitter_ids;
    std::vector<matter::Float3> observed_emitter_velocities;
    std::function<void(FluidBakeOutput&)> mutate_output;
};

FluidBakeInput valid_input() {
    FluidBakeInput input{};
    input.network.cell_size_m = 1.0f;
    input.network.seed = 42u;
    input.network.first_section_river = "main";
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{1.0f, 9.0f, 1.0f}, 3.5f};
    river.spline = {{1.0f, 9.0f, 1.0f}, {9.0f, 1.0f, 9.0f}};
    input.network.rivers.push_back(river);

    input.geometry.centreline = {
        {{1.0f, 9.0f, 1.0f}, {0.7f, -0.1f, 0.7f},
         {-0.7f, 0.0f, 0.7f}, 0.0f, 0.1f, 0.0f, 1.0f},
        {{9.0f, 1.0f, 9.0f}, {0.7f, -0.1f, 0.7f},
         {-0.7f, 0.0f, 0.7f}, 12.0f, 0.1f, 0.0f, 1.0f},
    };
    input.geometry.bounds_m = {{0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}};
    input.geometry.revision = 11u;

    input.collision.vertices = {
        {0.0f, 0.0f, 0.0f},
        {10.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 10.0f},
    };
    input.collision.indices = {0u, 1u, 2u};
    input.emitters = {
        {7u, {1.0f, 8.0f, 1.0f}, {0.0f, -0.2f, 1.0f},
         {0.0f, -0.5f, 6.0f}, 3.5f, 0.5f, 0u, 120u},
        {3u, {3.0f, 7.0f, 2.0f}, {0.2f, -0.1f, 1.0f},
         {1.0f, -0.25f, 4.0f}, 1.0f, 0.3f, 10u, 90u},
    };
    input.sensor = {{{7.0f, 0.0f, 7.0f}, {9.0f, 2.0f, 9.0f}},
                    {4u, 2u, 4u}, 0.7f, 4u};
    input.settings = {0.2f, 1000.0f, 1.0f / 60.0f, 4u, 96u,
                      8u, 120u, 1000u};
    input.dry_collar_bounds_m = {{-1.0f, -1.0f, -1.0f},
                                 {11.0f, 11.0f, 11.0f}};
    return input;
}

void test_invalid_input_never_invokes_backend() {
    RecordingBackend backend;
    auto input = valid_input();
    input.collision.indices[2] = 99u;
    FluidBakeOutput output{};
    output.particles.push_back({});
    FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              input, backend, {}, output, error),
          "out-of-range collision input is rejected");
    CHECK(error.code == FluidBakeCode::InvalidInput,
          "invalid index receives stable InvalidInput code");
    CHECK(backend.probe_calls == 0 && backend.run_calls == 0,
          "invalid input is rejected before touching the backend");
    CHECK(output.particles.empty(),
          "failed orchestration clears caller-owned output");

    input = valid_input();
    input.emitters[0].position_m.x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::PhysxFluidBake::run(
              input, backend, {}, output, error),
          "non-finite emitter input is rejected");
    CHECK(backend.probe_calls == 0 && backend.run_calls == 0,
          "non-finite input is rejected before backend probing");
}

void test_every_nested_numeric_input_is_validated() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto expect_rejected_before_backend = [](FluidBakeInput input,
                                             const char* message) {
        RecordingBackend backend;
        FluidBakeOutput output{};
        FluidBakeError error{};
        CHECK(!hydrology::PhysxFluidBake::run(
                  input, backend, {}, output, error),
              message);
        CHECK(error.code == FluidBakeCode::InvalidInput &&
                  backend.probe_calls == 0 && backend.run_calls == 0,
              "nested non-finite input is rejected before backend probe");
    };

    auto input = valid_input();
    input.network.rivers[0].reaches.push_back({10.0f, nan, 0.2f, 1.0f});
    expect_rejected_before_backend(std::move(input),
                                   "non-finite river reach is rejected");
    input = valid_input();
    input.network.rivers[0].channel.width_m = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite channel is rejected");
    input = valid_input();
    input.network.rivers[0].boulders.radius_m.y = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite boulder authoring is rejected");
    input = valid_input();
    input.network.first_section.crest_wet_fraction = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite section settings are rejected");
    input = valid_input();
    input.geometry.boulders.push_back({{2.0f, 3.0f, 4.0f}, nan});
    expect_rejected_before_backend(std::move(input),
                                   "non-finite generated boulder is rejected");
    input = valid_input();
    input.emitters[0].initial_velocity_mps.z = nan;
    expect_rejected_before_backend(std::move(input),
                                   "non-finite inlet velocity is rejected");
}

void test_unavailable_backend_and_cancellation_are_stable() {
    RecordingBackend backend;
    backend.available = false;
    FluidBakeOutput output{};
    FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "unavailable backend is not treated as a successful dry bake");
    CHECK(error.code == FluidBakeCode::BackendUnavailable &&
              backend.probe_calls == 1 && backend.run_calls == 0,
          "probe failure translates to BackendUnavailable without run");

    backend = {};
    FluidBakeCallbacks callbacks{};
    callbacks.cancelled = [] { return true; };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "pre-cancelled bake is rejected");
    CHECK(error.code == FluidBakeCode::Cancelled &&
              backend.probe_calls == 0 && backend.run_calls == 0,
          "pre-cancellation propagates before backend allocation");

    backend = {};
    backend.behavior = BackendBehavior::Cancel;
    int cancellation_polls = 0;
    callbacks.cancelled = [&] { return ++cancellation_polls >= 2; };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "cancellation during a backend batch propagates");
    CHECK(error.code == FluidBakeCode::Cancelled && backend.run_calls == 1,
          "backend cancellation keeps its stable status");

    backend = {};
    backend.behavior = BackendBehavior::IgnoreCancellation;
    cancellation_polls = 0;
    callbacks.cancelled = [&] { return ++cancellation_polls >= 2; };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "observed cancellation cannot be ignored by a backend");
    CHECK(error.code == FluidBakeCode::Cancelled,
          "ignored backend cancellation still returns Cancelled");
}

void test_success_preserves_emitters_progress_and_stable_particle_order() {
    RecordingBackend backend;
    std::vector<std::uint32_t> progress_steps;
    FluidBakeCallbacks callbacks{};
    callbacks.progress = [&](const FluidBakeProgress& progress) {
        progress_steps.push_back(progress.completed_steps);
    };
    FluidBakeOutput output{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          error.message.c_str());
    CHECK(error.code == FluidBakeCode::Ready,
          "success reports Ready rather than a backend-specific code");
    CHECK(backend.observed_emitter_ids ==
              std::vector<std::uint32_t>({7u, 3u}),
          "multiple emitters reach the backend in authored order");
    CHECK(backend.observed_emitter_velocities.size() == 2u &&
              backend.observed_emitter_velocities[0].z == 6.0f &&
              backend.observed_emitter_velocities[1].x == 1.0f,
          "authored inlet velocities reach the backend unchanged");
    CHECK(progress_steps == std::vector<std::uint32_t>({1u, 2u}),
          "monotonic backend progress reaches the caller");
    CHECK(output.particles.size() == 3u &&
              output.particles[0].id == 2u &&
              output.particles[1].id == 5u &&
              output.particles[2].id == 9u,
          "accepted particles are sorted by stable id");
}

void test_progress_and_backend_output_are_validated() {
    FluidBakeOutput output{};
    FluidBakeError error{};

    RecordingBackend backend;
    backend.behavior = BackendBehavior::RegressProgress;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "regressing backend progress is rejected");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "progress contract violation maps to BackendFailure");

    backend = {};
    backend.behavior = BackendBehavior::NonFiniteOutput;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "non-finite backend particles are rejected");
    CHECK(error.code == FluidBakeCode::NonFinite,
          "non-finite output receives stable NonFinite code");

    backend = {};
    backend.behavior = BackendBehavior::EscapedOutput;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "particles outside the dry collar are rejected");
    CHECK(error.code == FluidBakeCode::Escaped,
          "escaped output receives stable Escaped code");

    backend = {};
    backend.behavior = BackendBehavior::DuplicateIds;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "duplicate stable particle ids are rejected");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "duplicate ids receive a backend contract failure");
}

void test_backend_exceptions_never_cross_the_matter_boundary() {
    FluidBakeOutput output{};
    FluidBakeError error{};
    RecordingBackend backend;
    backend.throw_on_probe = true;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "backend probe exception is contained");
    CHECK(error.code == FluidBakeCode::BackendFailure &&
              error.message.find("probe") != std::string::npos,
          "probe exception becomes a stable backend diagnostic");

    backend = {};
    backend.behavior = BackendBehavior::Throw;
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "backend run exception is contained");
    CHECK(error.code == FluidBakeCode::BackendFailure &&
              error.message.find("run") != std::string::npos,
          "run exception becomes a stable backend diagnostic");

    backend = {};
    FluidBakeCallbacks callbacks{};
    callbacks.progress = [](const FluidBakeProgress&) {
        throw std::runtime_error("fake progress exception");
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, callbacks, output, error),
          "caller progress exception is contained");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "callback exception becomes a stable backend diagnostic");
}

void test_capacity_statistics_and_sensor_consistency_are_distinct() {
    FluidBakeOutput output{};
    FluidBakeError error{};
    RecordingBackend backend;
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.stats.active_particles = 2u;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "inconsistent active count is rejected");
    CHECK(error.code == FluidBakeCode::BackendFailure,
          "telemetry inconsistency is not mislabeled as capacity exhaustion");

    backend = {};
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.stats.peak_particles = 1001u;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "peak count beyond capacity is rejected");
    CHECK(error.code == FluidBakeCode::CapacityExceeded,
          "peak count beyond the cap reports CapacityExceeded");

    backend = {};
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.sensor.stable_steps = 6u;
        candidate.sensor.completion_step = 5u;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error),
          "impossible stable-window telemetry is rejected");
    CHECK(error.code == FluidBakeCode::SensorNotReached,
          "impossible sensor telemetry reports SensorNotReached");

    backend = {};
    backend.mutate_output = [](FluidBakeOutput& candidate) {
        candidate.sensor.maximum_wet_fraction = 0.5f;
    };
    CHECK(!hydrology::PhysxFluidBake::run(
              valid_input(), backend, {}, output, error) &&
              error.code == FluidBakeCode::SensorNotReached,
          "sensor telemetry rejects a maximum below the final wet fraction");
}

void test_accepted_snapshot_builds_all_products_or_publishes_nothing() {
    FluidBakeOutput output{};
    output.particles = {
        {{0.25f, 1.0f, 0.25f}, {1.0f, 0.0f, 0.0f}, 2u},
        {{0.75f, 1.5f, 0.25f}, {3.0f, 0.0f, 0.0f}, 5u},
    };
    output.stats = {6u, 2u, 2u, 0u, 0u, 0.1};
    output.sensor = {0.8f, 3u, 6u, true, 0.8f, 0.8f, 0.8f, 4u};
    hydrology::PhysxFluidBake::ProductBuildSettings settings{};
    settings.particle_radius_m = 0.65f;
    settings.coarse_voxel_m = 0.5f;
    settings.visual_job.bounds_m = {{-1.0f, -1.0f, -1.0f}, {2.0f, 3.0f, 2.0f}};
    settings.visual_job.voxel_m = 0.25f;
    settings.visual_job.blend_width_m = 0.1f;
    settings.visual_job.iso_value = 0.0f;
    settings.visual_job.limits = {16u, 4096u, 65536u, 65536u};
    settings.gameplay_layout = {{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 1u};
    settings.semantic = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    settings.provenance = {0x10deu, 0x2684u, 1u, 1u, 2u};
    bool saw_water_job = false;
    auto visual = [&](const gpu_meshing::ParticleJob& job,
                      gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                      gpu_meshing::Error&, const gpu_meshing::BuildControl&) {
        saw_water_job = job.material == 4u && job.particle_count == 2u &&
                        job.particles[0].radius_m == 0.65f &&
                        job.particles[1].position_m.x == 0.75f;
        mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                          0.0f, 1.0f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    hydrology::HydrologyArtifact artifact{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings,
              [](float, float, float& height) { height = 0.0f; return true; },
              visual, artifact, error), error.message.c_str());
    CHECK(saw_water_job && artifact.accepted && artifact.visual_mesh.material == 4u &&
              !artifact.coarse_cpu_mesh.positions.empty() &&
              artifact.gameplay_field[0].wet_valid,
          "the accepted stable-id snapshot feeds existing visual and CPU meshers plus gameplay fields");

    int gpu_run_calls = 0;
    int vk_visual_calls = 0;
    hydrology::HydrologyArtifact renderer_artifact{};
    const hydrology::PhysxFluidBake::GpuRunner gpu_run =
        [&](const char* name, std::function<bool(std::string&)> work,
            std::string& runner_error) {
            ++gpu_run_calls;
            CHECK(std::string(name) == "hydrology_particle_visual",
                  "the accepted fluid visual product uses the renderer job name");
            return work(runner_error);
        };
    const hydrology::PhysxFluidBake::VisualMesher vk_visual =
        [&](const gpu_meshing::ParticleJob& job, gpu_meshing::MeshResult& mesh,
            gpu_meshing::Stats& stats, gpu_meshing::Error& gpu_error,
            const gpu_meshing::BuildControl& control) {
            ++vk_visual_calls;
            return visual(job, mesh, stats, gpu_error, control);
        };
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact_on_renderer(
              output, settings,
              [](float, float, float& height) { height = 0.0f; return true; },
              gpu_run, vk_visual, renderer_artifact, error) &&
              gpu_run_calls == 1 && vk_visual_calls == 1 &&
              renderer_artifact.accepted,
          "the LocalProvider-compatible seam marshals accepted water through vk_particle_visual_bake");

    hydrology::HydrologyArtifact rejected{};
    auto failed_visual = [](const gpu_meshing::ParticleJob&, gpu_meshing::MeshResult&,
                            gpu_meshing::Stats&, gpu_meshing::Error& error,
                            const gpu_meshing::BuildControl&) {
        error.message = "deliberate GPU mesher failure";
        return false;
    };
    CHECK(!hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings,
              [](float, float, float& height) { height = 0.0f; return true; },
              failed_visual, rejected, error) && rejected.particles.empty() &&
              error.code == FluidBakeCode::ProductFailure,
          "a failed required visual product leaves no publishable artifact");
}

void test_product_keys_follow_the_settings_the_extractors_consume() {
    FluidBakeOutput output{};
    output.particles = {
        {{0.25f, 1.0f, 0.25f}, {1.0f, 0.0f, 0.0f}, 2u},
        {{0.75f, 1.5f, 0.25f}, {3.0f, 0.0f, 0.0f}, 5u},
    };
    output.stats = {6u, 2u, 2u, 0u, 0u, 0.1};
    output.sensor = {0.8f, 3u, 6u, true, 0.8f, 0.8f, 0.8f, 4u};
    hydrology::PhysxFluidBake::ProductBuildSettings settings{};
    settings.particle_radius_m = 0.65f;
    settings.coarse_voxel_m = 0.5f;
    settings.visual_job.bounds_m = {{-1.0f, -1.0f, -1.0f}, {2.0f, 3.0f, 2.0f}};
    settings.visual_job.voxel_m = 0.25f;
    settings.visual_job.blend_width_m = 0.1f;
    settings.visual_job.limits = {16u, 4096u, 65536u, 65536u};
    settings.gameplay_layout = {{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 1u};
    settings.semantic = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    settings.provenance = {0x10deu, 0x2684u, 1u, 1u, 2u};
    auto visual = [](const gpu_meshing::ParticleJob& job,
                     gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                     gpu_meshing::Error&, const gpu_meshing::BuildControl&) {
        mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                          0.0f, 1.0f, 0.0f};
        mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    };
    const auto terrain = [](float, float, float& height) {
        height = 0.0f;
        return true;
    };
    hydrology::HydrologyArtifact first{};
    FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, settings, terrain, visual, first, error), error.message.c_str());
    auto coarse_changed = settings;
    coarse_changed.coarse_voxel_m = 0.4f;
    hydrology::HydrologyArtifact second{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, coarse_changed, terrain, visual, second, error), error.message.c_str());
    CHECK(first.product_keys.coarse_cpu != second.product_keys.coarse_cpu &&
              first.product_keys.gameplay == second.product_keys.gameplay,
          "the coarse key follows the CPU mesher voxel setting without churning gameplay");
    auto layout_changed = settings;
    layout_changed.gameplay_layout = {{-0.5f, 0.0f, -0.5f}, 1.0f, 3u, 2u};
    hydrology::HydrologyArtifact third{};
    CHECK(hydrology::PhysxFluidBake::build_accepted_artifact(
              output, layout_changed, terrain, visual, third, error), error.message.c_str());
    CHECK(first.product_keys.gameplay != third.product_keys.gameplay &&
              first.product_keys.coarse_cpu == third.product_keys.coarse_cpu,
          "the gameplay key follows its complete section-local layout without churning CPU mesh");

    auto mismatch = settings;
    mismatch.provenance.adapter_version = 99u;
    hydrology::HydrologyArtifact rejected{};
    CHECK(!hydrology::PhysxFluidBake::build_accepted_artifact(
              output, mismatch, terrain, visual, rejected, error) &&
              error.code == FluidBakeCode::ProductFailure && rejected.particles.empty(),
          "mismatched PhysX or adapter provenance cannot become an accepted artifact");
}

#if defined(MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
bool write_world_session_fixture(const std::filesystem::path& root,
                                 bool fluid_enabled = true) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "objects", error);
    if (error) return false;
    std::filesystem::create_directories(root / "worlds", error);
    if (error) return false;
    {
        std::ofstream part(root / "objects" / "FluidBakePart.js");
        part << "class FluidBakePart extends Part {\n"
                "  build(p) {\n"
                "    this.fill(MAT.stone);\n"
                "    this.beginShape(SHAPE.triangles);\n"
                "    this.vertex(0, 0, 0); this.vertex(1, 0, 0); this.vertex(0, 1, 0);\n"
                "    this.endShape();\n"
                "  }\n"
                "}\n";
        if (!part) return false;
    }
    std::ofstream world(root / "worlds" / "Demo.js");
    world << "class Demo extends World {\n"
             "  static hydrology = {\n"
             "    enabled: " << (fluid_enabled ? "true" : "false") <<
             ", origin: [0, 0, 0], dimensions: [8, 8, 8],\n"
             "    cellSize: 1, dt: 0.01, gravity: 9.81, downstream: [1, 0],\n"
             "    residualGrade: [-0.01, 0], inletFlow: 1, inletHead: 4,\n"
             "    outletHead: 1, batchSteps: 8, maxSteps: 120\n"
             "  };\n"
             "  static roots = [{ module: 'FluidBakePart', transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] }];\n"
             "}\n";
    return static_cast<bool>(world);
}

bool drive_world_session_bake(matter::WorldSession& session) {
    session.request_bake();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        session.pump_gpu_jobs(8.0f);
        matter::Event event{};
        bool observed_event = false;
        while (session.poll_event(event)) {
            observed_event = true;
            if (event.type == matter::EventType::BakeFinished) return true;
            if (event.type == matter::EventType::BakeError) {
                return false;
            }
        }
        if (!observed_event)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

struct WorldSessionFluidCase {
    bool opened = false;
    bool finished = false;
    bool accepted = false;
    std::uint32_t dry_instance_count = 0;
    int backend_factory_calls = 0;
    int backend_run_calls = 0;
    int visual_calls = 0;
};

WorldSessionFluidCase run_world_session_fluid_case(
    const char* fixture_name, bool fluid_enabled, bool visual_succeeds) {
    WorldSessionFluidCase result{};
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / fixture_name;
    CHECK(write_world_session_fixture(root, fluid_enabled),
          "the live fluid request test created its minimal editor world");
    auto backend = std::make_shared<RecordingBackend>();
    {
        const std::string cache_root = (root / ".cache").string();
        matter::EngineDesc engine_desc{};
        engine_desc.cache_root = cache_root.c_str();
        engine_desc.allow_gl_lt_46 = true;
        std::string error;
        auto engine = matter::EngineContext::create(engine_desc, error);
        CHECK(engine != nullptr,
              error.empty() ? "the live fluid test created an engine"
                            : error.c_str());
        if (!engine) return result;

        const std::string project_dir = root.string();
        matter::WorldDesc world_desc{};
        world_desc.project_dir = project_dir.c_str();
        world_desc.world_name = "Demo";
        auto session = engine->open_world(world_desc, error);
        CHECK(session != nullptr,
              error.empty() ? "the live fluid test opened an editor world"
                            : error.c_str());
        if (!session) return result;
        result.opened = true;

        session->set_test_fluid_bake_dependencies(
            [&] {
                ++result.backend_factory_calls;
                return backend;
            },
            [&](const gpu_meshing::ParticleJob& job,
                gpu_meshing::MeshResult& mesh, gpu_meshing::Stats&,
                gpu_meshing::Error&,
                const gpu_meshing::BuildControl&) {
                ++result.visual_calls;
                if (!visual_succeeds) return false;
                mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f};
                mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                0.0f, 0.0f, 1.0f};
                mesh.indices = {0u, 1u, 2u};
                mesh.material = job.material;
                mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
                return true;
            });
        CHECK(result.backend_factory_calls == 0,
              "open_world keeps the authored fluid backend lazy before the bake");
        result.finished = drive_world_session_bake(*session);
        result.accepted = session->has_accepted_fluid_artifact_for_test();
        result.dry_instance_count = session->instance_count();
        result.backend_run_calls = backend->run_calls;
    }
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    return result;
}

void test_world_session_runs_authored_fluid_bake_before_publication() {
    const WorldSessionFluidCase success = run_world_session_fluid_case(
        "matter-live-fluid-success-contract", true, true);
    CHECK(success.opened && success.finished && success.dry_instance_count > 0 &&
              success.accepted && success.backend_factory_calls == 1 &&
              success.backend_run_calls == 1 && success.visual_calls == 1,
          "the live WorldSession path sends authored fluid through the production renderer before publication");

    const WorldSessionFluidCase renderer_failure = run_world_session_fluid_case(
        "matter-live-fluid-renderer-failure-contract", true, false);
    CHECK(renderer_failure.opened && renderer_failure.finished &&
              renderer_failure.dry_instance_count > 0 &&
              !renderer_failure.accepted &&
              renderer_failure.backend_factory_calls == 1 &&
              renderer_failure.backend_run_calls == 1 &&
              renderer_failure.visual_calls == 1,
          "a live renderer product failure preserves dry terrain and publishes no fluid artifact");

    const WorldSessionFluidCase authored_disabled = run_world_session_fluid_case(
        "matter-live-fluid-disabled-contract", false, true);
    CHECK(authored_disabled.opened && authored_disabled.finished &&
              authored_disabled.dry_instance_count > 0 &&
              !authored_disabled.accepted &&
              authored_disabled.backend_factory_calls == 0 &&
              authored_disabled.backend_run_calls == 0 &&
              authored_disabled.visual_calls == 0,
          "an authored-disabled live world remains lazy and publishes dry terrain");
}
#endif

}  // namespace

int main() {
    test_collision_assembly_deduplicates_without_changing_winding();
    test_collision_assembly_rejects_invalid_geometry_and_transforms();
    test_collision_bounds_and_virtual_dam_do_not_create_hidden_walls();
    test_emission_fractional_carry_boundaries_and_stable_ids();
    test_emission_capacity_is_checked_before_state_or_output_changes();
    test_emission_rejects_duplicate_ids_before_initializing_state();
    test_fill_sensor_rejects_jets_and_requires_a_consecutive_window();
    test_invalid_input_never_invokes_backend();
    test_every_nested_numeric_input_is_validated();
    test_unavailable_backend_and_cancellation_are_stable();
    test_success_preserves_emitters_progress_and_stable_particle_order();
    test_progress_and_backend_output_are_validated();
    test_backend_exceptions_never_cross_the_matter_boundary();
    test_capacity_statistics_and_sensor_consistency_are_distinct();
    test_accepted_snapshot_builds_all_products_or_publishes_nothing();
    test_product_keys_follow_the_settings_the_extractors_consume();
#if defined(MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
    test_world_session_runs_authored_fluid_bake_before_publication();
#endif
    return check_summary();
}
