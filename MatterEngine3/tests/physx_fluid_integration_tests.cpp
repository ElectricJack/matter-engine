#include "check.h"

#include "hydrology/authored_fluid_request.h"
#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/physx_fluid_bake.h"
#include "hydrology/physx_runtime.h"
#include "hydrology/water_visual_products.h"
#include "provider/local_provider.h"
#include "script/world_definition_loader.h"
#include "terrain_river_overlay.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CloseWindow CloseWindowWin32
#define ShowCursor ShowCursorWin32
#include <windows.h>
#undef ShowCursor
#undef CloseWindow
#endif

namespace {

hydrology::FluidCollisionMesh horizontal_quad(float half_extent,
                                               float height = 0.0f) {
    hydrology::FluidCollisionMesh mesh{};
    mesh.vertices = {
        {-half_extent, height, -half_extent},
        {half_extent, height, -half_extent},
        {half_extent, height, half_extent},
        {-half_extent, height, half_extent},
    };
    mesh.indices = {0u, 2u, 1u, 0u, 3u, 2u};
    return mesh;
}

hydrology::FluidCollisionMesh horizontal_triangle(float half_extent) {
    hydrology::FluidCollisionMesh mesh{};
    mesh.vertices = {
        {-half_extent, 0.0f, -half_extent},
        {0.0f, 0.0f, half_extent},
        {half_extent, 0.0f, -half_extent},
    };
    mesh.indices = {0u, 1u, 2u};
    return mesh;
}

hydrology::FluidCollisionMesh box_mesh(matter::Float3 minimum,
                                       matter::Float3 maximum) {
    hydrology::FluidCollisionMesh mesh{};
    mesh.vertices = {
        {minimum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
    };
    mesh.indices = {
        0u, 1u, 2u, 0u, 2u, 3u,  // front
        4u, 6u, 5u, 4u, 7u, 6u,  // back
        0u, 4u, 5u, 0u, 5u, 1u,  // bottom
        3u, 6u, 2u, 3u, 7u, 6u,  // top
        0u, 3u, 7u, 0u, 7u, 4u,  // left
        1u, 5u, 6u, 1u, 6u, 2u,  // right
    };
    return mesh;
}

hydrology::FluidCollisionMesh ramp_mesh() {
    hydrology::FluidCollisionMesh mesh{};
    mesh.vertices = {
        {-4.0f, 3.0f, -5.0f},
        {4.0f, 3.0f, -5.0f},
        {4.0f, 0.0f, 10.0f},
        {-4.0f, 0.0f, 10.0f},
    };
    mesh.indices = {0u, 2u, 1u, 0u, 3u, 2u};
    return mesh;
}

void append_quad(hydrology::FluidCollisionMesh& mesh,
                 matter::Float3 a, matter::Float3 b,
                 matter::Float3 c, matter::Float3 d,
                 bool reverse_winding) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {a, b, c, d});
    if (reverse_winding) {
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 2u, base + 1u,
                             base, base + 3u, base + 2u});
    } else {
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1u, base + 2u,
                             base, base + 2u, base + 3u});
    }
}

hydrology::FluidCollisionMesh short_matter_chute_mesh() {
    hydrology::FluidCollisionMesh mesh{};
    const float top = 11.0f;
    const matter::Float3 left[] = {
        {-2.0f, 6.0f, 0.0f},
        {-3.5f, 4.5f, 10.0f},
        {-2.5f, 3.0f, 20.0f},
    };
    const matter::Float3 right[] = {
        {2.0f, 6.0f, 0.0f},
        {3.5f, 4.5f, 10.0f},
        {2.5f, 3.0f, 20.0f},
    };
    for (std::size_t section = 0; section != 2u; ++section) {
        append_quad(mesh, left[section], right[section],
                    right[section + 1u], left[section + 1u], true);
        append_quad(mesh, left[section], left[section + 1u],
                    {left[section + 1u].x, top, left[section + 1u].z},
                    {left[section].x, top, left[section].z}, true);
        append_quad(mesh, right[section], right[section + 1u],
                    {right[section + 1u].x, top, right[section + 1u].z},
                    {right[section].x, top, right[section].z}, false);
    }
    append_quad(mesh, left[2], right[2],
                {right[2].x, top, right[2].z},
                {left[2].x, top, left[2].z}, true);
    return mesh;
}

hydrology::FluidCollisionProbeInput probe_input(
    hydrology::FluidCollisionMesh collision,
    std::vector<matter::Float3> starts) {
    hydrology::FluidCollisionProbeInput input{};
    input.collision = std::move(collision);
    input.initial_positions_m = std::move(starts);
    input.dry_collar_bounds_m = {{-20.0f, -10.0f, -20.0f},
                                 {20.0f, 20.0f, 30.0f}};
    input.probe_radius_m = 0.25f;
    input.fixed_step_seconds = 1.0f / 120.0f;
    input.max_steps = 240u;
    return input;
}

bool finite(matter::Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

hydrology::FluidBakeInput pbd_basin_input() {
    hydrology::FluidBakeInput input{};
    input.network.cell_size_m = 1.0f;
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{0.0f, 1.5f, 0.0f}, 1.0f};
    river.curve = {{0.0f, 1.5f, -0.5f}, {0.0f, 0.5f, 1.0f}};
    river.channel_profile = {{0.0f, 4.0f, 2.0f, 0.0f},
                             {2.0f, 4.0f, 2.0f, 0.0f}};
    input.network.rivers.push_back(river);
    matter::RiverSectionDefinition section{};
    section.id = "basin";
    section.river = "main";
    section.to_m = 2.0f;
    section.dry_margin_m = 1.0f;
    input.network.sections.push_back(section);
    input.network.bake_sequential = true;
    input.geometry.centreline = {
        {{0.0f, 1.5f, -0.5f}, {0.0f, -0.5f, 1.0f},
         {-1.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 2.0f, 0.0f},
        {{0.0f, 0.5f, 1.0f}, {0.0f, -0.5f, 1.0f},
         {-1.0f, 0.0f, 0.0f}, 2.0f, 4.0f, 2.0f, 0.0f},
    };
    input.geometry.bounds_m = {{-2.0f, 0.0f, -2.0f},
                               {2.0f, 4.0f, 2.0f}};
    input.collision = box_mesh({-2.0f, 0.0f, -2.0f},
                               {2.0f, 4.0f, 2.0f});
    input.settings = {0.2f, 1000.0f, 1.0f / 60.0f, 4u, 96u,
                      4u, 24u, 64u};
    const float particle_volume =
        1.333f * 3.14159f * 0.2f * 0.2f * 0.2f;
    input.emitters = {
        {7u, hydrology::FluidEmitterShape::Disc,
         {0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {}, {},
         {0.0f, 0.0f, 0.25f}, particle_volume * 60.0f,
         0.35f, {}, 0u, 24u},
    };
    input.sensor = {{{-0.75f, 0.0f, -0.75f},
                     {0.75f, 3.0f, 0.75f}},
                    {1u, 1u, 1u}, 1.0f, 2u, 1u};
    input.sensor.frame_origin_m = input.sensor.bounds_m.minimum;
    input.sensor.frame_extent_m = {1.5f, 3.0f, 1.5f};
    input.dry_collar_bounds_m = {{-3.0f, -1.0f, -3.0f},
                                 {3.0f, 5.0f, 3.0f}};
    return input;
}

struct RuntimeTrace {
    std::vector<std::tuple<hydrology::PhysxRuntimeEvent,
                           std::uint32_t, std::uint32_t>> events;
};

void record_runtime_event(hydrology::PhysxRuntimeEvent event,
                          std::uint32_t step, std::uint32_t value,
                          void* user_data) {
    static_cast<RuntimeTrace*>(user_data)->events.emplace_back(
        event, step, value);
}

void test_pbd_batch_loop_uses_gpu_sensor_and_returns_one_snapshot() {
    RuntimeTrace trace{};
    hydrology::PhysxRuntimeOptions options{};
    options.execution_hook = &record_runtime_event;
    options.execution_hook_user_data = &trace;
    hydrology::PhysxRuntime runtime(options);
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    const auto input = pbd_basin_input();

    CHECK(hydrology::PhysxFluidBake::run(
              input, runtime, {}, output, error),
          error.message.c_str());
    CHECK(output.sensor.complete && output.sensor.completion_step == 2u &&
              output.sensor.first_satisfied_step == 1u &&
              output.sensor.maximum_wet_fraction == 1.0f &&
              output.sensor.final_wet_fraction == 1.0f &&
              output.sensor.stable_window_wet_fraction == 1.0f &&
              output.stats.simulated_steps == 4u &&
              output.stats.active_particles == 4u &&
              output.particles.size() == 4u,
          "native PBD run completes from the GPU occupancy sensor");
    CHECK(output.particles[0].id == 0u && output.particles[1].id == 1u &&
              finite(output.particles[0].position_m) &&
              finite(output.particles[1].velocity_mps),
          "accepted native run copies one finite stable-id snapshot");
    CHECK(!output.animation_capture.has_value(),
          "a network without meshAnimation retains the single-snapshot path");
    // Golden snapshot captured from this fixed fixture using the unmodified
    // PhysX 5.6.1 SnippetPBF material/offset/mass setup on the reference RTX
    // 4090. Tolerances allow same-architecture driver variation while catching
    // changes to activation order or official PBD parameterization.
    const matter::Float3 reference_positions[] = {
        {0.00504111685f, 1.48561764f, 0.0162575003f},
        {0.187237307f, 1.67765975f, 0.0124340793f},
        {0.00498337019f, 1.68650365f, 0.00859848596f},
        {-0.197983429f, 1.69680321f, 0.00422162469f},
    };
    const matter::Float3 reference_velocities[] = {
        {0.0818048865f, -0.0750849992f, 0.230594605f},
        {-0.623257995f, -0.620274246f, 0.240396068f},
        {0.196425825f, -0.603899717f, 0.262732208f},
        {0.215965629f, -0.202863485f, 0.253319174f},
    };
    bool control_parity = output.particles.size() == 4u;
    for (std::size_t index = 0u;
         control_parity && index < output.particles.size(); ++index) {
        const auto close = [](matter::Float3 value, matter::Float3 reference,
                              float tolerance) {
            return std::fabs(value.x - reference.x) <= tolerance &&
                   std::fabs(value.y - reference.y) <= tolerance &&
                   std::fabs(value.z - reference.z) <= tolerance;
        };
        control_parity = control_parity &&
            close(output.particles[index].position_m,
                  reference_positions[index], 2.0e-4f) &&
            close(output.particles[index].velocity_mps,
                  reference_velocities[index], 2.0e-3f);
    }
    CHECK(control_parity,
          "fixed Matter output matches the recorded SnippetPBF control snapshot");

    bool ordered = true;
    for (std::uint32_t step = 1u; step <= 2u; ++step) {
        std::size_t upload = trace.events.size();
        std::size_t simulate = trace.events.size();
        std::size_t sensor = trace.events.size();
        for (std::size_t index = 0; index < trace.events.size(); ++index) {
            if (std::get<1>(trace.events[index]) != step) continue;
            if (std::get<0>(trace.events[index]) ==
                hydrology::PhysxRuntimeEvent::ActivationUploaded) upload = index;
            if (std::get<0>(trace.events[index]) ==
                hydrology::PhysxRuntimeEvent::SimulateBegin) simulate = index;
            if (std::get<0>(trace.events[index]) ==
                hydrology::PhysxRuntimeEvent::SensorCountsReady) sensor = index;
        }
        ordered = ordered && upload < simulate && simulate < sensor;
    }
    CHECK(ordered, "activation upload precedes simulate and bounded sensor readback");
    std::size_t fourth_simulate = trace.events.size();
    std::size_t first_sensor = trace.events.size();
    for (std::size_t index = 0; index < trace.events.size(); ++index) {
        if (std::get<0>(trace.events[index]) ==
                hydrology::PhysxRuntimeEvent::SimulateBegin &&
            std::get<1>(trace.events[index]) == 4u) {
            fourth_simulate = index;
        }
        if (first_sensor == trace.events.size() &&
            std::get<0>(trace.events[index]) ==
                hydrology::PhysxRuntimeEvent::SensorCountsReady) {
            first_sensor = index;
        }
    }
    CHECK(fourth_simulate < first_sensor,
          "sensor counts return to the host once after the fixed-step batch");
}

void test_pbd_animation_capture_is_device_rolled_and_host_compacted_once() {
    auto input = pbd_basin_input();
    input.settings.fixed_step_seconds = 1.0f / 120.0f;
    input.settings.batch_steps = 120u;
    input.settings.max_steps = 120u;
    input.settings.max_particles = 128u;
    input.emitters.front().stop_step = 120u;
    input.network.fluid.mesh_animation = {
        true, 30u, 1.0f, 0.5f, 30u, 4u, 15u,
    };

    RuntimeTrace trace{};
    hydrology::PhysxRuntimeOptions options{};
    options.execution_hook = &record_runtime_event;
    options.execution_hook_user_data = &trace;
    hydrology::PhysxRuntime runtime(options);
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::run(
              input, runtime, {}, output, error),
          error.message.c_str());
    CHECK(output.animation_capture.has_value(),
          "an accepted animation-enabled run publishes its rolling capture");
    if (!output.animation_capture) return;
    const auto& capture = *output.animation_capture;
    CHECK(capture.frames_per_second == 30u &&
              capture.phase_offset_frames == 15u &&
              capture.frames.size() == 30u &&
              capture.frames.front().simulation_step == 4u &&
              capture.frames.back().simulation_step == 120u,
          "the host snapshot contains the chronological one-second history");
    bool finite_positions = true;
    for (const auto& frame : capture.frames) {
        for (matter::Float3 position : frame.positions_m)
            finite_positions = finite_positions && finite(position);
    }
    CHECK(finite_positions,
          "every captured frame stores only finite compacted positions");
    const auto captured_events = static_cast<std::uint32_t>(std::count_if(
        trace.events.begin(), trace.events.end(), [](const auto& event) {
            return std::get<0>(event) ==
                hydrology::PhysxRuntimeEvent::AnimationFrameCaptured;
        }));
    CHECK(captured_events == 30u,
          "the adapter reports one bounded diagnostic for each retained slot");
}

void test_pbd_ribbon_emitter_activates_a_broad_grid() {
    auto input = pbd_basin_input();
    auto& emitter = input.emitters.front();
    emitter.shape = hydrology::FluidEmitterShape::Ribbon;
    emitter.lateral_axis = {1.0f, 0.0f, 0.0f};
    emitter.up_axis = {0.0f, 1.0f, 0.0f};
    emitter.radius_m = 0.0f;
    emitter.half_extent_m = {0.4f, 0.2f};
    hydrology::PhysxRuntime runtime;
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::run(
              input, runtime, {}, output, error),
          error.message.c_str());
    float minimum_x = std::numeric_limits<float>::infinity();
    float maximum_x = -std::numeric_limits<float>::infinity();
    for (const auto& particle : output.particles) {
        minimum_x = std::min(minimum_x, particle.position_m.x);
        maximum_x = std::max(maximum_x, particle.position_m.x);
    }
    CHECK(output.sensor.complete && output.particles.size() == 4u &&
              maximum_x - minimum_x > 0.15f,
          "real PhysX activation preserves the ribbon's broad cross-stream layout");
}

void test_pbd_particles_reach_and_rest_on_authored_collision() {
    auto input = pbd_basin_input();
    input.settings.max_steps = 120u;
    input.emitters[0].stop_step = 4u;
    input.sensor.bounds_m = {{-1.0f, 0.0f, -1.0f},
                             {1.0f, 0.6f, 1.0f}};
    input.sensor.frame_origin_m = input.sensor.bounds_m.minimum;
    input.sensor.frame_extent_m = {2.0f, 0.6f, 2.0f};
    input.sensor.stable_steps = 3u;
    input.dry_collar_bounds_m.minimum.y = -0.5f;

    hydrology::PhysxRuntime runtime;
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::run(
              input, runtime, {}, output, error),
          error.message.c_str());
    float minimum_y = (std::numeric_limits<float>::max)();
    for (const auto& particle : output.particles) {
        minimum_y = (std::min)(minimum_y, particle.position_m.y);
    }
    CHECK(output.sensor.complete && output.stats.active_particles == 4u &&
              minimum_y > 0.02f,
          "PBD particles reach the lower sensor and remain above the authored basin floor");
}

void test_native_quarantine_excludes_escaped_ids_from_sensor_and_snapshot() {
    auto input = pbd_basin_input();
    input.dry_collar_bounds_m.minimum.x = -0.15f;
    input.dry_collar_bounds_m.maximum.x = 0.15f;

    hydrology::PhysxRuntime runtime;
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::PhysxFluidBake::run(
              input, runtime, {}, output, error),
          error.message.c_str());
    bool quarantined_id_leaked = false;
    for (const auto& particle : output.particles) {
        for (const auto& quarantined : output.quarantined_particles)
            quarantined_id_leaked = quarantined_id_leaked ||
                                    particle.id == quarantined.id;
    }
    bool accepted_inside = true;
    for (const auto& particle : output.particles) {
        accepted_inside = accepted_inside &&
            particle.position_m.x >= input.dry_collar_bounds_m.minimum.x &&
            particle.position_m.x <= input.dry_collar_bounds_m.maximum.x;
    }
    CHECK(output.sensor.complete && output.stats.emitted_particles == 4u &&
              output.stats.escaped_particles >= 1u &&
              output.stats.escaped_particles <= output.stats.escape_budget &&
              output.stats.active_particles == output.particles.size() &&
              output.stats.active_particles + output.stats.retired_particles ==
                  output.stats.emitted_particles &&
              output.stats.escaped_particles <=
                  output.stats.retired_particles &&
              output.quarantined_particles.size() ==
                  output.stats.escaped_particles &&
              !quarantined_id_leaked && accepted_inside,
          "native GPU quarantine permanently excludes escaped stable ids from sensor completion and final particles");
}

void test_pbd_cancellation_is_sampled_between_batches() {
    auto input = pbd_basin_input();
    input.sensor.bounds_m = {{1.0f, 0.0f, 1.0f}, {1.5f, 1.0f, 1.5f}};
    input.sensor.frame_origin_m = input.sensor.bounds_m.minimum;
    input.sensor.frame_extent_m = {0.5f, 1.0f, 0.5f};
    std::uint32_t progress_calls = 0u;
    hydrology::FluidBakeCallbacks callbacks{};
    callbacks.progress = [&](const hydrology::FluidBakeProgress&) {
        ++progress_calls;
    };
    callbacks.cancelled = [&]() { return progress_calls != 0u; };

    hydrology::PhysxRuntime runtime;
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              input, runtime, callbacks, output, error) &&
              error.code == hydrology::FluidBakeCode::Cancelled &&
              progress_calls == 1u,
          "native PBD cancellation is observed only at a batch boundary");
}

void test_pbd_capacity_fails_before_a_second_particle_write() {
    auto input = pbd_basin_input();
    input.settings.max_particles = 1u;
    input.sensor.bounds_m = {{1.0f, 0.0f, 1.0f}, {1.5f, 1.0f, 1.5f}};
    input.sensor.frame_origin_m = input.sensor.bounds_m.minimum;
    input.sensor.frame_extent_m = {0.5f, 1.0f, 0.5f};
    hydrology::PhysxRuntime runtime;
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              input, runtime, {}, output, error) &&
              error.code == hydrology::FluidBakeCode::CapacityExceeded &&
              error.message.find("requires 2 particles") != std::string::npos &&
              error.message.find("capacity is 1") != std::string::npos,
          "native PBD capacity is rejected before an out-of-range upload");
}

std::uint32_t inject_second_step_hardware_error(std::uint32_t step, void*) {
    return step == 2u ? 1u : 0u;
}

void test_pbd_hardware_error_is_device_lost() {
    hydrology::PhysxRuntimeOptions options{};
    options.hardware_error_injection_hook =
        &inject_second_step_hardware_error;
    hydrology::PhysxRuntime runtime(options);
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              pbd_basin_input(), runtime, {}, output, error) &&
              error.code == hydrology::FluidBakeCode::DeviceLost &&
              output.particles.empty(),
          "per-step PhysX hardware failure becomes DeviceLost without a partial snapshot");
}

std::uint32_t inject_first_step_cuda_oom(std::uint32_t step, void*) {
    return step == 1u ? 2u : 0u;  // CUDA_ERROR_OUT_OF_MEMORY
}

void test_pbd_cuda_oom_is_capacity_exceeded() {
    hydrology::PhysxRuntimeOptions options{};
    options.cuda_error_injection_hook = &inject_first_step_cuda_oom;
    hydrology::PhysxRuntime runtime(options);
    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::PhysxFluidBake::run(
              pbd_basin_input(), runtime, {}, output, error) &&
              error.code == hydrology::FluidBakeCode::CapacityExceeded &&
              output.particles.empty(),
          "CUDA out-of-memory becomes CapacityExceeded without a partial snapshot");
}

void test_gpu_collision_fixtures() {
    hydrology::PhysxRuntime runtime;
    hydrology::FluidCollisionProbeOutput output{};
    hydrology::FluidBakeError error{};

    auto input = probe_input(horizontal_triangle(10.0f),
                             {{0.0f, 3.0f, 0.0f}});
    CHECK(runtime.run_collision_probe(input, output, error),
          error.message.c_str());
    CHECK(output.final_positions_m.size() == 1u &&
              std::fabs(output.final_positions_m[0].y - 0.25f) <= 0.08f &&
              output.contact_events > 0u,
          "GPU probe settles on a single rendered triangle surface");

    input = probe_input(box_mesh({-2.0f, 0.0f, -2.0f},
                                 {2.0f, 1.0f, 2.0f}),
                        {{0.0f, 4.0f, 0.0f}});
    CHECK(runtime.run_collision_probe(input, output, error),
          error.message.c_str());
    CHECK(output.final_positions_m.size() == 1u &&
              std::fabs(output.final_positions_m[0].y - 1.25f) <= 0.08f,
          "GPU probe settles on the authored triangle-mesh box");

    input = probe_input(ramp_mesh(), {{0.0f, 4.0f, -3.0f}});
    input.max_steps = 150u;
    CHECK(runtime.run_collision_probe(input, output, error),
          error.message.c_str());
    CHECK(output.final_positions_m.size() == 1u &&
              output.final_positions_m[0].z > -2.5f &&
              finite(output.final_positions_m[0]) &&
              finite(output.final_velocities_mps[0]),
          "GPU probe remains finite and moves downhill on the sloped ramp");
}

void test_dry_collar_is_an_escape_sensor_not_a_collider() {
    auto input = probe_input(horizontal_quad(1.0f, -10.0f),
                             {{0.0f, 0.5f, 0.0f}});
    input.dry_collar_bounds_m = {{-2.0f, 0.0f, -2.0f},
                                 {2.0f, 2.0f, 2.0f}};
    input.max_steps = 120u;

    hydrology::PhysxRuntime runtime;
    hydrology::FluidCollisionProbeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(!runtime.run_collision_probe(input, output, error),
          "probe crossing the dry collar fails the collision fixture");
    CHECK(error.code == hydrology::FluidBakeCode::Escaped &&
              output.escaped_probes == 1u &&
              output.final_positions_m[0].y < 0.0f,
          "dry collar reports escape instead of reflecting from a hidden wall");
}

void test_invalid_triangle_cooking_is_a_categorized_failure() {
    hydrology::FluidCollisionMesh degenerate{};
    degenerate.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {2.0f, 0.0f, 0.0f},
    };
    degenerate.indices = {0u, 1u, 2u};
    auto input = probe_input(std::move(degenerate),
                             {{0.0f, 1.0f, 0.0f}});

    hydrology::PhysxRuntime runtime;
    hydrology::FluidCollisionProbeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(!runtime.run_collision_probe(input, output, error),
          "PhysX rejects an invalid cooked triangle mesh");
    CHECK(error.code == hydrology::FluidBakeCode::BackendFailure &&
              error.message.find("PhysX") != std::string::npos,
          "cooking warnings/errors become a categorized Matter failure");
}

void test_short_matter_chute_moves_probes_downhill_without_domain_walls() {
    auto input = probe_input(short_matter_chute_mesh(),
                             {{-0.5f, 6.5f, 2.0f},
                              {0.0f, 6.5f, 2.0f},
                              {0.5f, 6.5f, 2.0f}});
    input.dry_collar_bounds_m = {{-6.0f, 0.0f, -2.0f},
                                 {6.0f, 12.0f, 22.0f}};
    input.max_steps = 900u;

    hydrology::PhysxRuntime runtime;
    hydrology::FluidCollisionProbeOutput output{};
    hydrology::FluidBakeError error{};
    CHECK(runtime.run_collision_probe(input, output, error),
          error.message.c_str());
    float final_z_sum = 0.0f;
    bool all_finite = output.final_positions_m.size() == 3u &&
                      output.final_velocities_mps.size() == 3u;
    for (std::size_t index = 0; index < output.final_positions_m.size();
         ++index) {
        all_finite = all_finite && finite(output.final_positions_m[index]) &&
                     finite(output.final_velocities_mps[index]);
        final_z_sum += output.final_positions_m[index].z;
    }
    CHECK(all_finite && output.escaped_probes == 0u &&
              output.contact_events > 0u && final_z_sum / 3.0f > 17.0f,
          "short varying-width Matter chute moves finite probes downhill to its authored dam");
}

void test_checked_in_ravine_collision_covers_the_validation_collar() {
    namespace fs = std::filesystem;
    const fs::path project = fs::path("../../projects/world_demo");
    matter::WorldLoadDesc load{};
    load.world_path =
        (project / "scenes/RiverHydrology/RiverHydrology.js").string();
    load.objects_dir = (project / "objects").string();
    load.project_shared_lib_dir = (project / "shared-lib").string();
    load.engine_shared_lib_dir = "../shared-lib";
    matter::WorldDefinition definition{};
    matter::WorldLoadError load_error{};
    CHECK(matter::load_world_definition(load, definition, load_error),
          load_error.message.c_str());
    CHECK(definition.river_network.has_value(),
          "the real RiverHydrology scene publishes its authored network");
    if (!definition.river_network) return;

    const auto& network = *definition.river_network;
    const auto& river = network.rivers.front();
    float horizontal_length_m = 0.0f;
    for (std::size_t index = 1; index < river.curve.size(); ++index) {
        const float dx = river.curve[index].x - river.curve[index - 1u].x;
        const float dz = river.curve[index].z - river.curve[index - 1u].z;
        horizontal_length_m += std::hypot(dx, dz);
    }
    const float descent_fraction =
        (river.curve.front().y - river.curve.back().y) /
        horizontal_length_m;
    CHECK(horizontal_length_m >= 100.0f &&
              descent_fraction >= 0.13f && descent_fraction <= 0.17f &&
              river.channel_profile.size() >= 3u &&
              river.channel_profile.front().width_m !=
                  river.channel_profile[2u].width_m &&
              river.channel_profile.front().depth_m > 0.0f &&
              std::fabs(river.channel_profile.front().asymmetry) < 0.25f,
          "the real first section is a 100 m+, profile-varied, roughly 15% rounded-V ravine");
    const auto& authored_emitter = network.fluid.emitters.front();
    const float solid_rest_offset =
        0.5f * network.fluid.pbd.particle_spacing_m / 0.6f;
    const auto& inlet_profile = river.channel_profile.front();
    const float inlet_half_width = inlet_profile.width_m * 0.5f;
    const float lateral_fraction = std::min(
        1.0f, authored_emitter.radius_m / inlet_half_width);
    constexpr float kRavineRoundness = 0.08f;
    const float rounded_lateral =
        (std::sqrt(lateral_fraction * lateral_fraction +
                   kRavineRoundness * kRavineRoundness) -
         kRavineRoundness) /
        (std::sqrt(1.0f + kRavineRoundness * kRavineRoundness) -
         kRavineRoundness);
    const float maximum_inlet_bank_rise = inlet_profile.depth_m *
        (1.0f + 0.85f * std::fabs(inlet_profile.asymmetry)) *
        rounded_lateral;
    CHECK(authored_emitter.position_m.y - authored_emitter.radius_m >=
              river.curve.front().y + maximum_inlet_bank_rise +
                  solid_rest_offset,
          "the real inlet emitter disk starts fully above the rounded-V bank collision surface");

    viewer::FluidBakeRunContext context{};
    context.terrain = [](float, float, float& height) {
        height = 0.0f;
        return true;
    };
    viewer::FluidBakeRequest request{};
    hydrology::FluidBakeError error{};
    hydrology::RiverGeometry geometry{};
    std::string geometry_error;
    CHECK(hydrology::build_river_geometry(
              network, network.sections.front().river, geometry,
              geometry_error),
          geometry_error.c_str());
    CHECK(viewer::assemble_authored_fluid_section_request(
              network, geometry, network.sections.front(), {}, {}, context,
              "ravine-acceptance-cache", request, error),
          error.message.c_str());
    if (request.input.collision.vertices.empty()) return;

    float minimum_x = std::numeric_limits<float>::infinity();
    float minimum_z = std::numeric_limits<float>::infinity();
    float maximum_x = -std::numeric_limits<float>::infinity();
    float maximum_z = -std::numeric_limits<float>::infinity();
    for (const auto& vertex : request.input.collision.vertices) {
        minimum_x = std::min(minimum_x, vertex.x);
        minimum_z = std::min(minimum_z, vertex.z);
        maximum_x = std::max(maximum_x, vertex.x);
        maximum_z = std::max(maximum_z, vertex.z);
    }
    constexpr float kCoverageEpsilon = 0.01f;
    const auto& collar = request.input.dry_collar_bounds_m;
    CHECK(minimum_x <= collar.minimum.x - kCoverageEpsilon &&
              minimum_z <= collar.minimum.z - kCoverageEpsilon &&
              maximum_x >= collar.maximum.x + kCoverageEpsilon &&
              maximum_z >= collar.maximum.z + kCoverageEpsilon,
          "the physical terrain surface covers every side of the dry-collar escape sensor");
}

void test_real_two_section_river_reaches_ready() {
    namespace fs = std::filesystem;
    const fs::path project = fs::absolute("../../projects/world_demo");
    const fs::path cache = fs::temp_directory_path() /
                           "matter-real-two-section-river-acceptance";
    std::error_code filesystem_error;
    fs::remove_all(cache, filesystem_error);

    hydrology::FluidBackendProbe probe{};
    {
        hydrology::PhysxRuntime identity_runtime;
        probe = identity_runtime.probe();
    }
    CHECK(probe.available, probe.message.c_str());
    if (!probe.available) return;

    auto config = viewer::make_engine_local_provider_config(
        project.string(), "RiverHydrology",
        fs::absolute("../shared-lib").string(),
        [] { return std::make_shared<hydrology::PhysxRuntime>(); });
    config.cache_root = cache.string();
    config.fluid_renderer_device.luid = probe.device_luid;
    config.fluid_renderer_device.luid_valid = probe.device_luid_valid;
    config.fluid_renderer_device.vendor_id = 0x10deu;
    config.fluid_renderer_device.device_id = 1u;
    config.fluid_renderer_device.driver_version =
        static_cast<std::uint32_t>(probe.cuda_driver_version);
    config.gpu_run = [](const char*, std::function<bool(std::string&)> run,
                        std::string& error) { return run(error); };
    config.vk_particle_visual_bake = [](
        const gpu_meshing::ParticleJob& job,
        gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
        gpu_meshing::Error& error,
        const gpu_meshing::BuildControl&) {
        if (job.particle_count == 0u) {
            error = {gpu_meshing::ErrorCode::InvalidInput,
                     "acceptance visual requires fluid particles"};
            return false;
        }
        if (!hydrology::build_cpu_particle_visual(
                job, 0.65f, mesh, error))
            return false;
        stats.particles = job.particle_count;
        stats.triangles = static_cast<std::uint32_t>(
            mesh.indices.size() / 3u);
        error = {};
        return true;
    };

    viewer::LocalProvider provider(std::move(config));
    viewer::WorldManifest dry_manifest{};
    std::string provider_error;
    CHECK(provider.connect(dry_manifest, provider_error),
          provider_error.c_str());
    if (!provider_error.empty()) {
        fs::remove_all(cache, filesystem_error);
        return;
    }

    hydrology::RiverGeometry acceptance_geometry{};
    std::shared_ptr<const terrain_field::RiverHeightOverlay>
        acceptance_overlay;
    CHECK(provider.build_river_height_overlay(
              acceptance_geometry, acceptance_overlay, provider_error),
          provider_error.c_str());
    if (!acceptance_overlay) {
        fs::remove_all(cache, filesystem_error);
        return;
    }

    viewer::FluidBakeRunContext context{};
    context.terrain_revision = acceptance_overlay->hash();
    context.terrain = [acceptance_overlay](float x, float z, float& height) {
        // Represent the mountainous base field surrounding the rounded-V
        // overlay, then apply the provider-owned carve exactly as FieldRuntime
        // does in the editor.
        const float base_height = 118.0f - 0.15f * x;
        height = acceptance_overlay->height_at(x, z, base_height);
        return std::isfinite(height);
    };
    matter::HydrologyStatus status{};
    hydrology::FluidBakeError error{};
    hydrology::HydrologyNetworkBakeResult result{};
    const bool ready = provider.run_authored_fluid_bake(
        context, status, error, result);
    CHECK(ready, error.message.c_str());
    CHECK(result.manifest.state ==
              hydrology::HydrologyNetworkState::Ready,
          "real network reaches Ready");
    CHECK(result.sections.size() == 2u && result.handoffs.size() == 1u,
          "real network accepts both sections and one spillway");
    if (result.sections.size() == 2u) {
        CHECK(result.sections[0].sensor.complete &&
                  result.sections[1].sensor.complete,
              "both terminal pools satisfy their fill sensors");
        CHECK(result.sections[0].stats.non_finite_particles == 0u &&
                  result.sections[1].stats.non_finite_particles == 0u,
              "both snapshots remain finite");
        CHECK(result.sections[0].stats.escaped_particles <=
                      result.sections[0].stats.escape_budget &&
                  result.sections[1].stats.escaped_particles <=
                      result.sections[1].stats.escape_budget,
              "both sections satisfy the authored escape policy");
    }
    CHECK(!result.products.visual_mesh.indices.empty() &&
              !result.products.coarse_cpu_mesh.indices.empty() &&
              !result.products.gameplay_field.empty(),
          "real network publishes visual, query, and gameplay products");
    CHECK(result.timings.sections.size() == 2u &&
              result.timings.sections[0].simulate_ms > 0.0 &&
              result.timings.sections[0].gpu_mesh_ms > 0.0 &&
              result.timings.sections[0].cpu_mesh_ms > 0.0 &&
              result.timings.sections[1].simulate_ms > 0.0 &&
              result.timings.sections[1].gpu_mesh_ms > 0.0 &&
              result.timings.sections[1].cpu_mesh_ms > 0.0 &&
              result.timings.handoff_mesh_ms > 0.0 &&
              result.timings.serialize_ms > 0.0 &&
              result.timings.total_wall_ms > 0.0,
          "real acceptance records separated simulation, product, handoff, serialization, and wall timings");
    if (result.handoffs.size() == 1u) {
        hydrology::FluidBakeError seam_error{};
        CHECK(hydrology::validate_handoff_products(
                  result.handoffs.front(), result.products,
                  result.handoffs.front().handoff, seam_error),
              seam_error.message.c_str());
        const auto& dam = result.handoffs.front().handoff
                              .temporary_dam_exclusion_bounds_m;
        CHECK(result.products.visual_mesh.material == 4u &&
                  dam.minimum.x <= dam.maximum.x &&
                  dam.minimum.y <= dam.maximum.y &&
                  dam.minimum.z <= dam.maximum.z,
              "real aggregate remains a water-only mesh after the temporary dam is removed");
    }
    fs::remove_all(cache, filesystem_error);
}

void test_core_sdk_is_statically_linked() {
#ifdef _WIN32
    CHECK(GetModuleHandleA("PhysX_64.dll") == nullptr &&
              GetModuleHandleA("PhysXCommon_64.dll") == nullptr &&
              GetModuleHandleA("PhysXFoundation_64.dll") == nullptr &&
              GetModuleHandleA("PhysXCooking_64.dll") == nullptr,
          "PhysX core/cooking DLLs are absent from the process import graph");
#endif
}

void test_missing_gpu_runtime_is_a_stable_probe_failure() {
    hydrology::PhysxRuntimeOptions options{};
    options.gpu_runtime_path =
        (std::filesystem::temp_directory_path() /
         "matter-definitely-missing-PhysXGpu_64.dll")
            .string();
    hydrology::PhysxRuntime runtime(options);
    const auto probe = runtime.probe();
    CHECK(!probe.available,
          "missing PhysX GPU module is not reported as available");
    CHECK(probe.code == hydrology::FluidBakeCode::BackendUnavailable,
          "missing GPU module receives stable BackendUnavailable code");
    CHECK(probe.message.find("GPU") != std::string::npos ||
              probe.message.find("gpu") != std::string::npos,
          "missing GPU module retains an actionable diagnostic");
}

void throw_during_initialization() {
    throw std::runtime_error("injected PhysX initialization failure");
}

void test_initialization_exceptions_do_not_cross_the_backend_boundary() {
    hydrology::PhysxRuntimeOptions options{};
    options.initialization_hook = &throw_during_initialization;
    hydrology::PhysxRuntime runtime(options);

    hydrology::FluidBackendProbe probe{};
    bool probe_threw = false;
    try {
        probe = runtime.probe();
    } catch (...) {
        probe_threw = true;
    }
    CHECK(!probe_threw && !probe.available &&
              probe.code == hydrology::FluidBakeCode::BackendFailure &&
              probe.message.find("injected PhysX initialization failure") !=
                  std::string::npos,
          "probe translates initialization exceptions into BackendFailure");

    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    bool run_threw = false;
    try {
        (void)runtime.run({}, {}, output, error);
    } catch (...) {
        run_threw = true;
    }
    CHECK(!run_threw &&
              error.code == hydrology::FluidBakeCode::BackendFailure,
          "run does not leak a cached initialization exception");
}

void test_exact_sdk_cuda_context_and_rtx_identity() {
    for (int iteration = 0; iteration != 3; ++iteration) {
        hydrology::PhysxRuntime runtime;
        const auto probe = runtime.probe();
        CHECK(probe.available, probe.message.c_str());
        CHECK(probe.code == hydrology::FluidBakeCode::Ready,
              "successful native probe reports Ready");
        CHECK(probe.backend_name == "NVIDIA PhysX PBD" &&
                  probe.sdk_version == "5.6.1" &&
                  probe.sdk_version_hex == 0x05060100u,
              "compiled and runtime PhysX identity is exactly 5.6.1");
        CHECK(probe.cuda_context_valid && probe.device_ordinal >= 0,
              "native probe creates a valid CUDA context");
        CHECK(probe.device_name.find("RTX 4090") != std::string::npos,
              "native probe selects the reference RTX 4090");
        CHECK(probe.cuda_driver_version > 0 &&
                  probe.device_total_memory_bytes >
                      (static_cast<std::uint64_t>(20u) << 30u),
              "native probe reports CUDA driver and device memory");

        const auto repeated = runtime.probe();
        CHECK(repeated.available &&
                  repeated.device_name == probe.device_name &&
                  repeated.device_ordinal == probe.device_ordinal,
              "repeated probe is stable without recreating the runtime");
    }
}

}  // namespace

int main() {
    test_core_sdk_is_statically_linked();
    test_missing_gpu_runtime_is_a_stable_probe_failure();
    test_initialization_exceptions_do_not_cross_the_backend_boundary();
    test_exact_sdk_cuda_context_and_rtx_identity();
    test_pbd_batch_loop_uses_gpu_sensor_and_returns_one_snapshot();
    test_pbd_animation_capture_is_device_rolled_and_host_compacted_once();
    test_pbd_ribbon_emitter_activates_a_broad_grid();
    test_pbd_particles_reach_and_rest_on_authored_collision();
    test_native_quarantine_excludes_escaped_ids_from_sensor_and_snapshot();
    test_pbd_cancellation_is_sampled_between_batches();
    test_pbd_capacity_fails_before_a_second_particle_write();
    test_pbd_hardware_error_is_device_lost();
    test_pbd_cuda_oom_is_capacity_exceeded();
    test_gpu_collision_fixtures();
    test_dry_collar_is_an_escape_sensor_not_a_collider();
    test_invalid_triangle_cooking_is_a_categorized_failure();
    test_short_matter_chute_moves_probes_downhill_without_domain_walls();
    test_checked_in_ravine_collision_covers_the_validation_collar();
    test_real_two_section_river_reaches_ready();
    test_missing_gpu_runtime_is_a_stable_probe_failure();
    return check_summary();
}
