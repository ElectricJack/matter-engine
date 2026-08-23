#include "gpu_visual_mesher_vk_tests.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "matter/gpu_visual_meshing.h"
#include "render/gpu_meshing/gpu_visual_mesher_vk.h"
#include "surface.h"

namespace {

int failures = 0;

#define GPU_CHECK(condition, message)                                         \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::printf("FAIL: %s\n", (message));                           \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

void check_scan(gpu_meshing::GpuVisualMesher& mesher,
                const std::vector<uint32_t>& input,
                const std::vector<uint32_t>& expected,
                uint32_t expected_total, const char* label) {
    std::vector<uint32_t> output;
    uint32_t total = 0;
    gpu_meshing::Error error{};
    const bool ok = mesher.debug_exclusive_scan(input, output, total, error);
    GPU_CHECK(ok, error.message.empty() ? label : error.message.c_str());
    GPU_CHECK(output == expected, label);
    GPU_CHECK(total == expected_total, label);
}

std::vector<uint32_t> reference_scan(const std::vector<uint32_t>& input,
                                     uint32_t& total) {
    std::vector<uint32_t> output;
    const bool ok = gpu_meshing::exclusive_scan_reference(input, output, total);
    GPU_CHECK(ok, "reference scan fixture is representable");
    return output;
}

void check_field_fixture(gpu_meshing::GpuVisualMesher& mesher,
                         const std::vector<gpu_meshing::ParticleSample>& samples,
                         gpu_meshing::Aabb bounds, float blend,
                         const char* label) {
    gpu_meshing::ParticleJob job{};
    job.particles = samples.data();
    job.particle_count = static_cast<uint32_t>(samples.size());
    job.bounds_m = bounds;
    job.voxel_m = 0.25f;
    job.blend_width_m = blend;
    job.limits = {64u, 1u << 20u, 1u << 20u, 1u << 20u};

    std::vector<float> first;
    std::vector<float> second;
    gpu_meshing::GridLayout layout{};
    gpu_meshing::Error error{};
    const bool first_ok =
        mesher.debug_evaluate_particle_field(job, first, layout, error);
    GPU_CHECK(first_ok, error.message.empty() ? label : error.message.c_str());
    error = {};
    gpu_meshing::GridLayout repeated_layout{};
    const bool second_ok = mesher.debug_evaluate_particle_field(
        job, second, repeated_layout, error);
    GPU_CHECK(second_ok,
              error.message.empty() ? "repeat GPU field dispatch succeeds"
                                    : error.message.c_str());
    if (!first_ok) return;
    GPU_CHECK(first.size() == layout.grid_vertices,
              "GPU field emits exactly one value per grid sample");
    if (second_ok)
        GPU_CHECK(first == second,
                  "repeated GPU scalar readbacks are byte-identical");

    std::vector<Particle> surface_particles(samples.size());
    float max_radius = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
        surface_particles[i] = {
            {samples[i].position_m.x, samples[i].position_m.y,
             samples[i].position_m.z},
            samples[i].radius_m, 4};
        max_radius = std::max(max_radius, samples[i].radius_m);
    }
    SurfaceScratch* scratch = CreateSurfaceScratch();
    GPU_CHECK(scratch != nullptr, "create MatterSurface field oracle scratch");
    for (uint32_t z = 0; z < layout.sample_dims[2]; ++z) {
        for (uint32_t y = 0; y < layout.sample_dims[1]; ++y) {
            for (uint32_t x = 0; x < layout.sample_dims[0]; ++x) {
                const uint32_t index = x + layout.sample_dims[0] *
                    (y + layout.sample_dims[1] * z);
                const matter::Float3 point{
                    layout.origin_m.x + layout.spacing_m.x * x,
                    layout.origin_m.y + layout.spacing_m.y * y,
                    layout.origin_m.z + layout.spacing_m.z * z};
                const float reference =
                    gpu_meshing::evaluate_particle_field_reference(
                        samples.data(), static_cast<uint32_t>(samples.size()),
                        blend, point);
                const float oracle = ProbeFieldScalar(
                    scratch, surface_particles.data(), max_radius,
                    static_cast<int>(surface_particles.size()), blend, nullptr,
                    nullptr, 0, nullptr, 0, 0.0f,
                    {point.x, point.y, point.z});
                if (std::isfinite(reference)) {
                    GPU_CHECK(std::isfinite(first[index]),
                              "GPU field finite classification matches reference");
                    GPU_CHECK(std::fabs(first[index] - reference) <= 2e-5f,
                              "GPU field matches compiler-neutral reference");
                    GPU_CHECK(std::fabs(first[index] - oracle) <= 2e-5f,
                              "GPU field matches MatterSurface ProbeFieldScalar");
                } else {
                    GPU_CHECK(!std::isfinite(first[index]) &&
                                  !std::isfinite(oracle),
                              "GPU field outside classification matches both oracles");
                }
            }
        }
    }
    DestroySurfaceScratch(scratch);
}

gpu_meshing::ParticleJob mesh_job(
    const std::vector<gpu_meshing::ParticleSample>& particles, float blend,
    gpu_meshing::Limits limits = {64u, 1u << 20u, 1u << 20u,
                                  1u << 20u}) {
    gpu_meshing::ParticleJob job{};
    job.particles = particles.data();
    job.particle_count = static_cast<uint32_t>(particles.size());
    job.bounds_m = {{-1.5f, -1.5f, -1.5f}, {1.5f, 1.5f, 1.5f}};
    job.voxel_m = 0.2f;
    job.blend_width_m = blend;
    job.iso_value = 0.0f;
    job.material = 4u;
    job.limits = limits;
    job.generation = 41u;
    return job;
}

gpu_meshing::MeshResult check_mesh_fixture(
    gpu_meshing::GpuVisualMesher& mesher,
    const std::vector<gpu_meshing::ParticleSample>& particles, float blend,
    const char* label) {
    gpu_meshing::MeshResult first{};
    gpu_meshing::MeshResult second{};
    gpu_meshing::Stats stats{};
    gpu_meshing::Error error{};
    const auto job = mesh_job(particles, blend);
    const bool first_ok =
        mesher.build_particle_visual(job, first, stats, error);
    GPU_CHECK(first_ok, error.message.empty() ? label : error.message.c_str());
    error = {};
    gpu_meshing::Stats repeated_stats{};
    const bool second_ok =
        mesher.build_particle_visual(job, second, repeated_stats, error);
    GPU_CHECK(second_ok,
              error.message.empty() ? "repeat GPU mesh extraction succeeds"
                                    : error.message.c_str());
    if (!first_ok) return {};
    GPU_CHECK(!first.positions.empty() && !first.indices.empty(),
              "GPU mesh fixture produces non-empty geometry");
    GPU_CHECK(first.positions.size() == first.normals.size() &&
                  first.positions.size() % 3u == 0u,
              "GPU mesh positions and normals have matching vec3 streams");
    GPU_CHECK(first.indices.size() % 3u == 0u,
              "GPU mesh index stream contains complete triangles");
    GPU_CHECK(first.material == job.material && first.content_digest != 0u,
              "GPU mesh retains material and content digest");
    for (size_t i = 0; i < first.positions.size(); ++i)
        GPU_CHECK(std::isfinite(first.positions[i]) &&
                      std::isfinite(first.normals[i]),
                  "GPU mesh output is finite");
    for (size_t i = 0; i < first.indices.size(); ++i)
        GPU_CHECK(first.indices[i] < first.positions.size() / 3u,
                  "GPU mesh indices remain in range");
    if (second_ok)
        GPU_CHECK(first.positions == second.positions &&
                      first.normals == second.normals &&
                      first.indices == second.indices &&
                      first.content_digest == second.content_digest,
                  "repeated GPU mesh extraction is byte-identical");
    return first;
}

}  // namespace

int run_gpu_visual_mesher_pure_vk_tests() {
    failures = 0;
    GPU_CHECK(sizeof(gpu_meshing::ParticleSample) == sizeof(float) * 4,
              "particle GPU ABI is one vec4");
    return failures;
}

int run_gpu_visual_mesher_vk_tests(matter::VulkanDevice& vulkan) {
    failures = 0;
    gpu_meshing::GpuVisualMesher mesher(vulkan);

    check_scan(mesher, {3, 0, 2, 5}, {0, 3, 3, 5}, 10,
               "GPU scan handles a partial workgroup");

    for (uint32_t count : {257u, 65537u}) {
        std::vector<uint32_t> input(count);
        for (uint32_t i = 0; i < count; ++i) input[i] = i % 7u;
        uint32_t expected_total = 0;
        const std::vector<uint32_t> expected =
            reference_scan(input, expected_total);
        check_scan(mesher, input, expected, expected_total,
                   count == 257u ? "GPU scan crosses one block boundary"
                                 : "GPU scan recursively scans block sums");
    }

    const std::vector<gpu_meshing::ParticleSample> particles{
        {{-3.0f, -1.0f, -1.0f}, 0.5f},
        {{-2.0f, -1.0f, -1.0f}, 0.5f},
        {{-1.0f, -2.0f, -1.0f}, 0.5f},
        {{0.0f, -1.0f, -2.0f}, 0.5f},
        {{1.0f, 0.0f, 0.0f}, 0.5f},
        {{-1.0f, -1.0f, -1.0f}, 0.5f},
    };
    gpu_meshing::ParticleJob job{};
    job.particles = particles.data();
    job.particle_count = static_cast<uint32_t>(particles.size());
    job.bounds_m = {{-3.0f, -3.0f, -3.0f}, {2.0f, 1.0f, 1.0f}};
    job.voxel_m = 0.25f;
    job.blend_width_m = 0.1f;
    job.limits = {64u, 1u << 20u, 1u << 20u, 1u << 20u};

    gpu_meshing::GpuParticleBins first{};
    gpu_meshing::GpuParticleBins second{};
    gpu_meshing::Error error{};
    const bool first_ok = mesher.debug_build_particle_bins(job, first, error);
    GPU_CHECK(first_ok,
              error.message.empty() ? "GPU particle bin build succeeds"
                                    : error.message.c_str());
    error = {};
    const bool second_ok = mesher.debug_build_particle_bins(job, second, error);
    GPU_CHECK(second_ok,
              error.message.empty() ? "repeated GPU particle bin build succeeds"
                                    : error.message.c_str());

    if (first_ok) {
        GPU_CHECK(first.counts.size() == first.offsets.size(),
                  "bin count and offset arrays have equal length");
        GPU_CHECK(std::is_sorted(first.offsets.begin(), first.offsets.end()),
                  "particle bin offsets are monotonic");
        GPU_CHECK(first.particle_ids.size() == particles.size(),
                  "every contributing particle is scattered exactly once");
        std::vector<uint32_t> all_ids = first.particle_ids;
        std::sort(all_ids.begin(), all_ids.end());
        std::vector<uint32_t> expected_ids(particles.size());
        std::iota(expected_ids.begin(), expected_ids.end(), 0u);
        GPU_CHECK(all_ids == expected_ids,
                  "particle bins contain every input id exactly once");
        for (size_t bin = 0; bin < first.counts.size(); ++bin) {
            const size_t begin = first.offsets[bin];
            const size_t end = begin + first.counts[bin];
            GPU_CHECK(end <= first.particle_ids.size(),
                      "particle bin range remains bounded");
            if (end <= first.particle_ids.size()) {
                GPU_CHECK(std::is_sorted(first.particle_ids.begin() + begin,
                                         first.particle_ids.begin() + end),
                          "particle ids are ascending inside each bin");
            }
        }
    }
    if (first_ok && second_ok) {
        GPU_CHECK(first.counts == second.counts &&
                      first.offsets == second.offsets &&
                      first.particle_ids == second.particle_ids,
                  "repeated particle bin readbacks are byte-identical");
    }

    check_field_fixture(mesher, {{{0.0f, 0.0f, 0.0f}, 0.5f}},
                        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}, 0.0f,
                        "single-sphere GPU field dispatch succeeds");
    check_field_fixture(
        mesher,
        {{{-0.55f, 0.0f, 0.0f}, 0.4f}, {{0.55f, 0.0f, 0.0f}, 0.4f}},
        {{-1.5f, -1.0f, -1.0f}, {1.5f, 1.0f, 1.0f}}, 0.0f,
        "separated-sphere GPU field dispatch succeeds");
    check_field_fixture(
        mesher,
        {{{-2.2f, -1.1f, -0.7f}, 0.55f},
         {{-1.5f, -1.1f, -0.7f}, 0.45f}},
        {{-3.0f, -2.0f, -1.5f}, {-0.5f, 0.0f, 0.5f}}, 0.18f,
        "translated blended-sphere GPU field dispatch succeeds");

    const std::vector<gpu_meshing::ParticleSample> one_sphere{
        {{0.0f, 0.0f, 0.0f}, 0.65f}};
    const gpu_meshing::MeshResult sphere = check_mesh_fixture(
        mesher, one_sphere, 0.0f, "single-sphere GPU extraction succeeds");
    for (size_t vertex = 0; vertex < sphere.positions.size() / 3u; ++vertex) {
        const float x = sphere.positions[vertex * 3u + 0u];
        const float y = sphere.positions[vertex * 3u + 1u];
        const float z = sphere.positions[vertex * 3u + 2u];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length <= 1e-6f) continue;
        const float dot =
            (sphere.normals[vertex * 3u + 0u] * x +
             sphere.normals[vertex * 3u + 1u] * y +
             sphere.normals[vertex * 3u + 2u] * z) /
            length;
        GPU_CHECK(dot >= 0.999f,
                  "single-sphere GPU normals follow the analytic gradient");
        GPU_CHECK(std::fabs(length - 0.65f) <= 0.05f,
                  "GPU vertices remain near the authored isosurface");
    }
    for (size_t triangle = 0; triangle < sphere.indices.size() / 3u;
         ++triangle) {
        const auto position = [&](uint32_t corner, uint32_t axis) {
            const uint32_t vertex = sphere.indices[triangle * 3u + corner];
            return sphere.positions[vertex * 3u + axis];
        };
        const float ax = position(1u, 0u) - position(0u, 0u);
        const float ay = position(1u, 1u) - position(0u, 1u);
        const float az = position(1u, 2u) - position(0u, 2u);
        const float bx = position(2u, 0u) - position(0u, 0u);
        const float by = position(2u, 1u) - position(0u, 1u);
        const float bz = position(2u, 2u) - position(0u, 2u);
        const float nx = ay * bz - az * by;
        const float ny = az * bx - ax * bz;
        const float nz = ax * by - ay * bx;
        const float cx = (position(0u, 0u) + position(1u, 0u) +
                          position(2u, 0u)) /
                         3.0f;
        const float cy = (position(0u, 1u) + position(1u, 1u) +
                          position(2u, 1u)) /
                         3.0f;
        const float cz = (position(0u, 2u) + position(1u, 2u) +
                          position(2u, 2u)) /
                         3.0f;
        GPU_CHECK(nx * cx + ny * cy + nz * cz > 0.0f,
                  "GPU triangle winding faces outward");
    }
    check_mesh_fixture(
        mesher,
        {{{-0.62f, 0.0f, 0.0f}, 0.45f},
         {{0.62f, 0.0f, 0.0f}, 0.45f}},
        0.0f, "separated-sphere GPU extraction succeeds");
    check_mesh_fixture(
        mesher,
        {{{-0.35f, 0.0f, 0.0f}, 0.55f},
         {{0.35f, 0.0f, 0.0f}, 0.55f}},
        0.18f, "blended-sphere GPU extraction succeeds");

    if (!sphere.positions.empty()) {
        gpu_meshing::Limits too_small = {
            64u, 1u << 20u,
            static_cast<uint32_t>(sphere.positions.size() / 3u - 1u),
            static_cast<uint32_t>(sphere.indices.size() - 1u)};
        auto limited_job = mesh_job(one_sphere, 0.0f, too_small);
        gpu_meshing::MeshResult rejected{{1.0f}, {1.0f}, {0u}, 9u, 9u};
        gpu_meshing::Stats rejected_stats{};
        error = {};
        GPU_CHECK(!mesher.build_particle_visual(limited_job, rejected,
                                                rejected_stats, error) &&
                      error.code == gpu_meshing::ErrorCode::LimitExceeded,
                  "GPU extraction rejects output capacity before emission");
        GPU_CHECK(rejected.positions.empty() && rejected.normals.empty() &&
                      rejected.indices.empty(),
                  "capacity rejection exposes no partial mesh");
    }

    gpu_meshing::MeshResult cancelled{{1.0f}, {}, {}, 0u, 0u};
    gpu_meshing::Stats cancelled_stats{};
    error = {};
    const gpu_meshing::BuildControl cancel_control{
        [] { return true; }, {}};
    GPU_CHECK(!mesher.build_particle_visual(mesh_job(one_sphere, 0.0f),
                                            cancelled, cancelled_stats, error,
                                            cancel_control) &&
                  error.code == gpu_meshing::ErrorCode::Cancelled &&
                  cancelled.positions.empty(),
              "cancelled GPU mesh build fails transactionally");
    gpu_meshing::MeshResult stale{{1.0f}, {}, {}, 0u, 0u};
    error = {};
    const gpu_meshing::BuildControl stale_control{
        {}, [](uint64_t) { return false; }};
    GPU_CHECK(!mesher.build_particle_visual(mesh_job(one_sphere, 0.0f), stale,
                                            cancelled_stats, error,
                                            stale_control) &&
                  error.code == gpu_meshing::ErrorCode::StaleGeneration &&
                  stale.positions.empty(),
              "stale GPU mesh generation fails transactionally");
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const auto check_fault = [&](const char* phase,
                                 gpu_meshing::ErrorCode expected) {
        const gpu_meshing::GpuMesherMemorySnapshot before =
            gpu_meshing::debug_gpu_mesher_memory_snapshot();
        _putenv_s("MATTER_GPU_MESH_TEST_FAULT", phase);
        {
            gpu_meshing::GpuVisualMesher fault_mesher(vulkan);
            gpu_meshing::MeshResult faulted{{1.0f}, {}, {}, 0u, 0u};
            gpu_meshing::Stats faulted_stats{};
            gpu_meshing::Error fault_error{};
            GPU_CHECK(!fault_mesher.build_particle_visual(
                          mesh_job(one_sphere, 0.0f), faulted, faulted_stats,
                          fault_error) &&
                          fault_error.code == expected &&
                          faulted.positions.empty() && faulted.indices.empty(),
                      "forced GPU mesher fault exposes no partial mesh");
        }
        _putenv_s("MATTER_GPU_MESH_TEST_FAULT", "");
        const gpu_meshing::GpuMesherMemorySnapshot after =
            gpu_meshing::debug_gpu_mesher_memory_snapshot();
        GPU_CHECK(after.allocations == before.allocations &&
                      after.bytes == before.bytes,
                  "forced GPU mesher fault releases tracked allocations");
    };
    check_fault("allocation", gpu_meshing::ErrorCode::VulkanFailure);
    check_fault("upload", gpu_meshing::ErrorCode::VulkanFailure);
    check_fault("readback", gpu_meshing::ErrorCode::VulkanFailure);
    check_fault("device-lost", gpu_meshing::ErrorCode::DeviceLost);

    _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE",
              "dispatch-moved-buffer");
    {
        gpu_meshing::GpuVisualMesher fault_mesher(vulkan);
        gpu_meshing::MeshResult dispatch_failed{{1.0f}, {}, {}, 0u, 0u};
        error = {};
        GPU_CHECK(!fault_mesher.build_particle_visual(
                      mesh_job(one_sphere, 0.0f), dispatch_failed,
                      cancelled_stats, error) &&
                      error.code == gpu_meshing::ErrorCode::VulkanFailure &&
                      dispatch_failed.positions.empty(),
                  "completed Vulkan dispatch failure exposes no partial mesh");
    }
    _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE", "");
#endif
    return failures;
}
