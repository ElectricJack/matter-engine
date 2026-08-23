#include "check.h"

#include "hydrology/physx_fluid_bake.h"
#include "hydrology/physx_runtime.h"

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
#include <windows.h>
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
    input.network.first_section_river = "main";
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{0.0f, 1.5f, 0.0f}, 1.0f};
    river.spline = {{0.0f, 1.5f, -0.5f}, {0.0f, 0.5f, 1.0f}};
    input.network.rivers.push_back(river);
    input.geometry.centreline = {
        {{0.0f, 1.5f, -0.5f}, {0.0f, -0.5f, 1.0f},
         {-1.0f, 0.0f, 0.0f}, 0.0f, 0.1f, 0.0f, 1.0f},
        {{0.0f, 0.5f, 1.0f}, {0.0f, -0.5f, 1.0f},
         {-1.0f, 0.0f, 0.0f}, 2.0f, 0.1f, 0.0f, 1.0f},
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
        {7u, {0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, 1.0f},
         {0.0f, 0.0f, 0.25f}, particle_volume * 60.0f,
         0.35f, 0u, 24u},
    };
    input.sensor = {{{-0.75f, 0.0f, -0.75f},
                     {0.75f, 3.0f, 0.75f}},
                    {1u, 1u, 1u}, 1.0f, 2u, 1u};
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

void test_pbd_particles_reach_and_rest_on_authored_collision() {
    auto input = pbd_basin_input();
    input.settings.max_steps = 120u;
    input.emitters[0].stop_step = 4u;
    input.sensor.bounds_m = {{-1.0f, 0.0f, -1.0f},
                             {1.0f, 0.6f, 1.0f}};
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

void test_pbd_cancellation_is_sampled_between_batches() {
    auto input = pbd_basin_input();
    input.sensor.bounds_m = {{1.0f, 0.0f, 1.0f}, {1.5f, 1.0f, 1.5f}};
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
    test_pbd_particles_reach_and_rest_on_authored_collision();
    test_pbd_cancellation_is_sampled_between_batches();
    test_pbd_capacity_fails_before_a_second_particle_write();
    test_pbd_hardware_error_is_device_lost();
    test_pbd_cuda_oom_is_capacity_exceeded();
    test_gpu_collision_fixtures();
    test_dry_collar_is_an_escape_sensor_not_a_collider();
    test_invalid_triangle_cooking_is_a_categorized_failure();
    test_short_matter_chute_moves_probes_downhill_without_domain_walls();
    test_missing_gpu_runtime_is_a_stable_probe_failure();
    return check_summary();
}
