#include "check.h"

#include "hydrology/physx_runtime.h"

#include <cstdint>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>

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
    test_gpu_collision_fixtures();
    test_dry_collar_is_an_escape_sensor_not_a_collider();
    test_invalid_triangle_cooking_is_a_categorized_failure();
    test_short_matter_chute_moves_probes_downhill_without_domain_walls();
    test_missing_gpu_runtime_is_a_stable_probe_failure();
    return check_summary();
}
