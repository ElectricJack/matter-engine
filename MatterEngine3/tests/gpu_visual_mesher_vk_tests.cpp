#include "gpu_visual_mesher_vk_tests.h"

#include "matter/windows_compat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "fixtures/gpu_mesher_synthetic_pbf.h"
#include "hydrology/hydrology_artifact.h"
#include "hydrology/physx_fluid_bake.h"
#include "hydrology/water_visual_products.h"
#include "matter/gpu_visual_meshing.h"
#include "render/gpu_meshing/gpu_visual_mesher_vk.h"
#include "render/gpu_meshing/water_scene_part.h"
#include "render/frame_matrices.h"
#include "render/matrix_math.h"
#include "render/vk_resources.h"
#include "render/vk_scene_renderer.h"
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
                         const char* label,
                         gpu_meshing::ParticlePhaseBlend phase_blend = {},
                         gpu_meshing::ParticleSamplingLattice lattice = {},
                         gpu_meshing::ParticleLongitudinalFieldBlend
                             longitudinal_blend = {}) {
    gpu_meshing::ParticleJob job{};
    job.particles = samples.data();
    job.particle_count = static_cast<uint32_t>(samples.size());
    job.bounds_m = bounds;
    job.voxel_m = 0.25f;
    if (lattice.version != 0u) job.voxel_m = lattice.voxel_m;
    job.blend_width_m = blend;
    job.phase_blend = phase_blend;
    job.sampling_lattice = lattice;
    job.longitudinal_field_blend = longitudinal_blend;
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
    if (second_ok) {
        GPU_CHECK(
            repeated_layout.cell_min == layout.cell_min &&
                repeated_layout.origin_m.x == layout.origin_m.x &&
                repeated_layout.origin_m.y == layout.origin_m.y &&
                repeated_layout.origin_m.z == layout.origin_m.z &&
                repeated_layout.spacing_m.x == layout.spacing_m.x &&
                repeated_layout.spacing_m.y == layout.spacing_m.y &&
                repeated_layout.spacing_m.z == layout.spacing_m.z &&
                repeated_layout.sample_dims == layout.sample_dims &&
                repeated_layout.cell_dims == layout.cell_dims &&
                repeated_layout.grid_vertices == layout.grid_vertices &&
                repeated_layout.grid_cells == layout.grid_cells,
            "repeated GPU field dispatch returns an identical CPU lattice layout");
    }

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
                const matter::Float3 point = lattice.version == 1u
                    ? matter::Float3{
                          lattice.origin_m.x + lattice.voxel_m *
                              static_cast<float>(layout.cell_min[0] + x),
                          lattice.origin_m.y + lattice.voxel_m *
                              static_cast<float>(layout.cell_min[1] + y),
                          lattice.origin_m.z + lattice.voxel_m *
                              static_cast<float>(layout.cell_min[2] + z)}
                    : matter::Float3{
                          layout.origin_m.x + layout.spacing_m.x * x,
                          layout.origin_m.y + layout.spacing_m.y * y,
                          layout.origin_m.z + layout.spacing_m.z * z};
                const float reference =
                    gpu_meshing::evaluate_particle_field_reference(
                        samples.data(), static_cast<uint32_t>(samples.size()),
                        blend, phase_blend, longitudinal_blend, point);
                const float oracle = longitudinal_blend.enabled
                    ? std::numeric_limits<float>::infinity()
                    : ProbeFieldScalar(
                          scratch, surface_particles.data(), max_radius,
                          static_cast<int>(surface_particles.size()), blend,
                          nullptr, nullptr, 0, nullptr, 0, 0.0f,
                          {point.x, point.y, point.z});
                if (std::isfinite(reference)) {
                    GPU_CHECK(std::isfinite(first[index]),
                              "GPU field finite classification matches reference");
                    GPU_CHECK(std::fabs(first[index] - reference) <= 2e-5f,
                              "GPU field matches compiler-neutral reference");
                    if (!longitudinal_blend.enabled &&
                        phase_blend.primary_weight == 1.0f &&
                        phase_blend.secondary_weight == 0.0f) {
                        GPU_CHECK(std::fabs(first[index] - oracle) <= 2e-5f,
                                  "GPU field matches MatterSurface ProbeFieldScalar");
                    }
                } else {
                    GPU_CHECK(!std::isfinite(first[index]) &&
                                  (longitudinal_blend.enabled ||
                                   !std::isfinite(oracle)),
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

using QuantizedVertex = std::array<std::int64_t, 3>;
using QuantizedTriangle = std::array<QuantizedVertex, 3>;

std::vector<QuantizedTriangle> canonical_triangles(
    const gpu_meshing::MeshResult& mesh, float tolerance_m) {
    std::vector<QuantizedTriangle> triangles;
    triangles.reserve(mesh.indices.size() / 3u);
    for (std::size_t triangle = 0u;
         triangle != mesh.indices.size() / 3u; ++triangle) {
        QuantizedTriangle canonical{};
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const std::uint32_t index = mesh.indices[triangle * 3u + corner];
            for (std::size_t axis = 0u; axis != 3u; ++axis) {
                canonical[corner][axis] = static_cast<std::int64_t>(std::llround(
                    mesh.positions[index * 3u + axis] / tolerance_m));
            }
        }
        std::sort(canonical.begin(), canonical.end());
        triangles.push_back(canonical);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

void check_longitudinal_mesh_normals_and_chunk_parity(
    gpu_meshing::GpuVisualMesher& mesher) {
    const std::vector<gpu_meshing::ParticleSample> particles{
        {{0.0f, -0.38f, 0.0f}, 0.72f},
        {{0.0f, 0.38f, 0.0f}, 0.72f},
    };
    gpu_meshing::ParticleJob root = mesh_job(
        particles, 0.12f,
        {64u, 1u << 20u, 1u << 20u, 1u << 20u});
    root.bounds_m = {{-1.2f, -1.4f, -1.2f}, {1.2f, 1.4f, 1.2f}};
    root.voxel_m = 0.15f;
    root.longitudinal_field_blend.source[0].primary_begin = 0u;
    root.longitudinal_field_blend.source[0].primary_count = 1u;
    root.longitudinal_field_blend.source[1].primary_begin = 1u;
    root.longitudinal_field_blend.source[1].primary_count = 1u;
    root.longitudinal_field_blend.origin_m = {0.0f, 0.0f, 0.0f};
    root.longitudinal_field_blend.direction = {1.0f, 0.0f, 0.0f};
    root.longitudinal_field_blend.upstream_full_m = -0.65f;
    root.longitudinal_field_blend.downstream_full_m = 0.65f;
    root.longitudinal_field_blend.enabled = true;

    const hydrology::PhysxFluidBake::VisualMesher gpu_mesher =
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
            gpu_meshing::Error& error,
            const gpu_meshing::BuildControl& control) {
            return mesher.build_particle_visual(
                job, mesh, stats, error, control);
        };
    gpu_meshing::MeshResult unchunked{};
    gpu_meshing::Error error{};
    GPU_CHECK(hydrology::PhysxFluidBake::build_visual_job_chunks(
                  root, gpu_mesher, unchunked, error),
              error.message.empty()
                  ? "longitudinal GPU mesh builds without forced chunks"
                  : error.message.c_str());
    if (unchunked.positions.empty()) return;

    std::size_t checked_normals = 0u;
    constexpr float epsilon = 1.0e-3f;
    for (std::size_t vertex = 0u;
         vertex != unchunked.positions.size() / 3u; ++vertex) {
        const matter::Float3 point{
            unchunked.positions[vertex * 3u + 0u],
            unchunked.positions[vertex * 3u + 1u],
            unchunked.positions[vertex * 3u + 2u]};
        if (point.x <= -0.45f || point.x >= 0.45f) continue;
        const auto field = [&](matter::Float3 probe) {
            return gpu_meshing::evaluate_particle_field_reference(
                particles.data(), static_cast<std::uint32_t>(particles.size()),
                root.blend_width_m, root.phase_blend,
                root.longitudinal_field_blend, probe);
        };
        const float xp = field({point.x + epsilon, point.y, point.z});
        const float xm = field({point.x - epsilon, point.y, point.z});
        const float yp = field({point.x, point.y + epsilon, point.z});
        const float ym = field({point.x, point.y - epsilon, point.z});
        const float zp = field({point.x, point.y, point.z + epsilon});
        const float zm = field({point.x, point.y, point.z - epsilon});
        if (!std::isfinite(xp) || !std::isfinite(xm) ||
            !std::isfinite(yp) || !std::isfinite(ym) ||
            !std::isfinite(zp) || !std::isfinite(zm)) {
            continue;
        }
        const float gx = xp - xm;
        const float gy = yp - ym;
        const float gz = zp - zm;
        const float length = std::sqrt(gx * gx + gy * gy + gz * gz);
        if (length <= 1.0e-6f) continue;
        const float dot =
            (unchunked.normals[vertex * 3u + 0u] * gx +
             unchunked.normals[vertex * 3u + 1u] * gy +
             unchunked.normals[vertex * 3u + 2u] * gz) / length;
        GPU_CHECK(dot >= 0.985f,
                  "longitudinal GPU normals differentiate the blended scalar field");
        ++checked_normals;
    }
    GPU_CHECK(checked_normals >= 24u,
              "longitudinal normal fixture exercises the blend interior");

    gpu_meshing::GridLayout layout{};
    error = {};
    GPU_CHECK(gpu_meshing::validate_particle_job(root, layout, error),
              error.message.empty()
                  ? "longitudinal chunk fixture has a valid root lattice"
                  : error.message.c_str());
    gpu_meshing::ParticleJob chunked_job = root;
    chunked_job.limits.max_grid_vertices =
        std::max(384u, layout.grid_vertices / 4u);
    std::uint32_t chunk_calls = 0u;
    const hydrology::PhysxFluidBake::VisualMesher counted_gpu_mesher =
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
            gpu_meshing::Error& chunk_error,
            const gpu_meshing::BuildControl& control) {
            ++chunk_calls;
            std::vector<float> gpu_field;
            gpu_meshing::GridLayout chunk_layout{};
            if (!mesher.debug_evaluate_particle_field(
                    job, gpu_field, chunk_layout, chunk_error)) {
                return false;
            }
            for (std::uint32_t z = 0u; z != chunk_layout.sample_dims[2]; ++z)
                for (std::uint32_t y = 0u; y != chunk_layout.sample_dims[1]; ++y)
                    for (std::uint32_t x = 0u; x != chunk_layout.sample_dims[0]; ++x) {
                        const std::uint32_t index = x +
                            chunk_layout.sample_dims[0] *
                                (y + chunk_layout.sample_dims[1] * z);
                        const matter::Float3 point{
                            chunk_layout.origin_m.x + chunk_layout.spacing_m.x * x,
                            chunk_layout.origin_m.y + chunk_layout.spacing_m.y * y,
                            chunk_layout.origin_m.z + chunk_layout.spacing_m.z * z};
                        const float cpu =
                            gpu_meshing::evaluate_particle_field_reference(
                                job.particles, job.particle_count,
                                job.blend_width_m, job.phase_blend,
                                job.longitudinal_field_blend, point);
                        if (std::isfinite(cpu)) {
                            if (!std::isfinite(gpu_field[index]) ||
                                std::fabs(cpu - gpu_field[index]) > 2.0e-5f) {
                                chunk_error = {
                                    gpu_meshing::ErrorCode::VulkanFailure,
                                    "forced chunk GPU field diverged from CPU reference"};
                                return false;
                            }
                        } else if (std::isfinite(gpu_field[index])) {
                            chunk_error = {
                                gpu_meshing::ErrorCode::VulkanFailure,
                                "forced chunk GPU field wet/dry classification diverged from CPU reference"};
                            return false;
                        }
                    }
            return mesher.build_particle_visual(
                job, mesh, stats, chunk_error, control);
        };
    gpu_meshing::MeshResult chunked{};
    error = {};
    GPU_CHECK(hydrology::PhysxFluidBake::build_visual_job_chunks(
                  chunked_job, counted_gpu_mesher, chunked, error),
              error.message.empty()
                  ? "longitudinal GPU mesh builds with forced chunks"
                  : error.message.c_str());
    GPU_CHECK(chunk_calls > 1u,
              "longitudinal parity fixture actually forces multiple GPU chunks");
    GPU_CHECK(canonical_triangles(chunked, 1.0e-4f) ==
                  canonical_triangles(unchunked, 1.0e-4f),
              "forced-chunk longitudinal GPU geometry matches the unchunked field topology");
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
    check_field_fixture(
        mesher,
        {{{-30.75f, 7.85f, 11.95f}, 0.45f},
         {{-30.10f, 7.95f, 12.20f}, 0.35f}},
        {{-31.03f, 7.46f, 11.67f}, {-29.71f, 8.11f, 12.46f}},
        0.12f,
        "translated canonical-lattice GPU field samples match the world-anchored CPU reference",
        {}, {{-31.2f, 7.4f, 11.8f}, 0.15f, 1u});
    check_field_fixture(
        mesher,
        {{{-0.35f, 0.0f, 0.0f}, 0.5f},
         {{0.45f, 0.0f, 0.0f}, 0.5f}},
        {{-1.5f, -1.0f, -1.0f}, {1.5f, 1.0f, 1.0f}}, 0.18f,
        "dual-phase weighted GPU field dispatch succeeds",
        {1u, 0.35f, 0.65f});
    gpu_meshing::ParticleLongitudinalFieldBlend source_blend{};
    source_blend.source[0] = {0u, 1u, 2u, 1u};
    source_blend.source[1] = {1u, 1u, 3u, 1u};
    source_blend.origin_m = {-30.4f, 7.9f, 12.0f};
    source_blend.direction = {1.0f, 0.0f, 0.0f};
    source_blend.upstream_full_m = -0.20f;
    source_blend.downstream_full_m = 0.20f;
    source_blend.enabled = true;
    check_field_fixture(
        mesher,
        {{{-30.4f, 7.9f, 12.0f}, 0.45f},
         {{-30.4f, 7.9f, 12.0f}, 0.45f},
         {{std::numeric_limits<float>::quiet_NaN(), 7.9f, 12.0f}, 0.45f},
         {{std::numeric_limits<float>::quiet_NaN(), 7.9f, 12.0f}, 0.45f}},
        {{-31.03f, 7.46f, 11.67f}, {-29.71f, 8.11f, 12.46f}},
        0.12f,
        "translated longitudinal source blend matches CPU endpoints, wet/dry, and midpoint without union bulging",
        {2u, 1.0f, 0.0f},
        {{-31.2f, 7.4f, 11.8f}, 0.15f, 1u}, source_blend);
    source_blend.source[0] = {0u, 1u, 0u, 0u};
    source_blend.source[1] = {1u, 1u, 0u, 0u};
    source_blend.downstream_full_m = 3.0f;
    check_field_fixture(
        mesher,
        {{{-30.4f, 7.9f, 12.0f}, 0.45f},
         {{-27.4f, 7.9f, 12.0f}, 0.45f}},
        {{-31.03f, 7.46f, 11.67f}, {-26.71f, 8.11f, 12.46f}},
        0.12f,
        "translated longitudinal source blend matches CPU when only upstream, only downstream, or neither source is wet",
        {2u, 1.0f, 0.0f},
        {{-31.2f, 7.4f, 11.8f}, 0.15f, 1u}, source_blend);
    source_blend.origin_m = {0.0f, 0.0f, 0.0f};
    source_blend.upstream_full_m = -1.0f;
    source_blend.downstream_full_m = 1.0f;
    check_field_fixture(
        mesher,
        {{{1.0f, 0.0f, 0.0f}, 0.2f},
         {{-1.0f, 0.0f, 0.0f}, 0.8f}},
        {{-1.25f, -0.25f, -0.25f}, {1.25f, 0.25f, 0.25f}},
        0.12f,
        "full-source endpoints preserve designated dry state and mixed-radius source support matches CPU",
        {2u, 1.0f, 0.0f},
        {{-1.25f, -0.25f, -0.25f}, 0.25f, 1u}, source_blend);
    source_blend.upstream_full_m = -1.25f;
    source_blend.downstream_full_m = 1.25f;
    check_field_fixture(
        mesher,
        {{{1.25f, 0.0f, 0.0f}, 0.2f},
         {{-1.25f, 0.0f, 0.0f}, 0.2f}},
        {{-1.5f, -0.25f, -0.25f}, {1.5f, 0.25f, 0.25f}},
        0.12f,
        "full-source endpoints stay dry when only the opposite source is wet",
        {2u, 1.0f, 0.0f},
        {{-1.5f, -0.25f, -0.25f}, 0.25f, 1u}, source_blend);

    check_longitudinal_mesh_normals_and_chunk_parity(mesher);

    const std::vector<gpu_meshing::ParticleSample> one_sphere{
        {{0.0f, 0.0f, 0.0f}, 0.65f}};
    const gpu_meshing::MeshResult sphere = check_mesh_fixture(
        mesher, one_sphere, 0.0f, "single-sphere GPU extraction succeeds");
    GPU_CHECK(sphere.content_digest == 0x374942c8f77e4eb9ull,
              "default static GPU mesh retains its accepted content digest");
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

int run_gpu_visual_mesher_acceptance(matter::VulkanDevice& vulkan) {
    failures = 0;
    const auto particles =
        gpu_meshing::fixtures::synthetic_flowing_water_particles();
    const auto job =
        gpu_meshing::fixtures::synthetic_flowing_water_job(particles);

    std::string renderer_error;
    viewer::VkSceneRenderer renderer(vulkan);
    gpu_meshing::MeshResult pre_pipeline{};
    gpu_meshing::Stats pre_pipeline_stats{};
    gpu_meshing::Error pre_pipeline_error{};
    GPU_CHECK(renderer.build_particle_visual(
                  job, pre_pipeline, pre_pipeline_stats,
                  pre_pipeline_error),
              pre_pipeline_error.message.empty()
                  ? "renderer-owned water mesher is available before the full scene pipeline"
                  : pre_pipeline_error.message.c_str());
    GPU_CHECK(!pre_pipeline.positions.empty() &&
                  !pre_pipeline.indices.empty(),
              "pre-pipeline hydrology meshing produces a complete transient surface");
    GPU_CHECK(renderer.init(renderer_error),
              renderer_error.empty()
                  ? "initialize renderer-owned GPU water mesher"
                  : renderer_error.c_str());

    gpu_meshing::MeshResult first{};
    gpu_meshing::MeshResult second{};
    gpu_meshing::Stats first_stats{};
    gpu_meshing::Stats second_stats{};
    gpu_meshing::Error error{};
    const bool first_ok = renderer.build_particle_visual(
        job, first, first_stats, error);
    GPU_CHECK(first_ok,
              error.message.empty() ? "bake synthetic flowing water"
                                    : error.message.c_str());
    error = {};
    const bool second_ok = renderer.build_particle_visual(
        job, second, second_stats, error);
    GPU_CHECK(second_ok,
              error.message.empty() ? "repeat synthetic flowing-water bake"
                                    : error.message.c_str());
    if (!first_ok || !second_ok) return failures;
    GPU_CHECK(!first.positions.empty() && !first.indices.empty(),
              "synthetic flowing water produces a non-empty visual surface");
    GPU_CHECK(first.positions == second.positions &&
                  first.normals == second.normals &&
                  first.indices == second.indices &&
                  first.content_digest == second.content_digest,
              "same-device synthetic water bakes are byte-identical");
    GPU_CHECK(first_stats.bin_ms > 0.0 && first_stats.field_ms > 0.0 &&
                  first_stats.classify_ms > 0.0 && first_stats.emit_ms > 0.0,
              "synthetic acceptance records nonzero stage timings");

    const std::uint64_t snapshot_digest =
        hydrology::particle_snapshot_digest(
            particles.data(), static_cast<std::uint32_t>(particles.size()));
    hydrology::ProductIdentitySettings identity{};
    identity.shader_digests = {
        0x62696e2d636f756eull, 0x6669656c642d7061ull,
        0x636c617373696679ull, 0x656d69742d763175ull};

    hydrology::HydrologyArtifact artifact{};
    artifact.section = {"synthetic", "main", 0.0f, 15.0f,
                        0.0f, 15.0f};
    artifact.particle_snapshot_digest = snapshot_digest;
    artifact.semantic_key = 0x73796e74682d7062ull;
    artifact.accepted = true;
    artifact.particle_radius_m = particles.front().radius_m;
    artifact.particles.reserve(particles.size());
    for (std::size_t i = 0; i < particles.size(); ++i) {
        artifact.particles.push_back(
            {particles[i].position_m, {}, static_cast<std::uint64_t>(i + 1u)});
    }
    artifact.stats.simulated_steps = 1u;
    artifact.stats.active_particles =
        static_cast<std::uint32_t>(artifact.particles.size());
    artifact.stats.peak_particles = artifact.stats.active_particles;
    artifact.stats.emitted_particles = artifact.stats.active_particles;
    artifact.stats.escape_budget = hydrology::fluid_escape_budget(
        artifact.stats.emitted_particles);
    artifact.sensor = {0.8f, 1u, 1u, true, 0.8f, 0.8f, 0.8f, 1u};
    constexpr float kCoarseVoxelM = 0.48f;
    const hydrology::GameplayFieldLayout gameplay_layout{
        job.bounds_m.min_m, 0.48f, 15u, 1u};
    artifact.product_keys =
        hydrology::derive_product_keys(job, snapshot_digest, identity,
                                       kCoarseVoxelM, gameplay_layout);
    artifact.visual_mesh = first;
    gpu_meshing::MeshResult cpu_repeat{};
    const auto cpu_first_start = std::chrono::steady_clock::now();
    error = {};
    GPU_CHECK(hydrology::build_cpu_particle_visual(
                  job, kCoarseVoxelM, artifact.coarse_cpu_mesh,
                  error),
              error.message.empty() ? "build coarse CPU water fallback"
                                    : error.message.c_str());
    const double cpu_first_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() -
                                    cpu_first_start)
                                    .count();
    const auto cpu_repeat_start = std::chrono::steady_clock::now();
    error = {};
    GPU_CHECK(hydrology::build_cpu_particle_visual(
                  job, kCoarseVoxelM, cpu_repeat, error),
              error.message.empty() ? "repeat coarse CPU water fallback"
                                    : error.message.c_str());
    const double cpu_repeat_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     cpu_repeat_start)
                                     .count();
    GPU_CHECK(artifact.coarse_cpu_mesh.positions == cpu_repeat.positions &&
                  artifact.coarse_cpu_mesh.normals == cpu_repeat.normals &&
                  artifact.coarse_cpu_mesh.indices == cpu_repeat.indices &&
                  artifact.coarse_cpu_mesh.content_digest ==
                      cpu_repeat.content_digest,
              "repeated CPU fallback meshes are byte-identical");

    // MatterSurface uses a power-of-two cubic lattice. A 0.12 m request on
    // this 15.4 m fixture selects a roughly 0.121 m cell, making it the
    // closest CPU visual-quality comparison to the GPU job's exact 0.16 m
    // rectangular grid (the next coarser CPU lattice is roughly 0.244 m).
    constexpr float kCpuVisualComparisonVoxelM = 0.12f;
    gpu_meshing::MeshResult cpu_visual{};
    const auto cpu_visual_start = std::chrono::steady_clock::now();
    error = {};
    GPU_CHECK(hydrology::build_cpu_particle_visual(
                  job, kCpuVisualComparisonVoxelM, cpu_visual, error),
              error.message.empty() ? "build CPU visual-quality comparison"
                                    : error.message.c_str());
    const double cpu_visual_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     cpu_visual_start)
                                     .count();
    artifact.gameplay_field =
        gpu_meshing::fixtures::synthetic_flowing_water_gameplay();
    artifact.presentation_field.resize(artifact.gameplay_field.size());
    for (std::size_t index = 0u;
         index != artifact.gameplay_field.size(); ++index) {
        artifact.presentation_field[index].wet_valid =
            artifact.gameplay_field[index].wet_valid;
    }
    artifact.gameplay_layout = gameplay_layout;
    artifact.provenance = {0x10deu, 0x2684u, 1u, 0x05060100u, 3u};

    std::filesystem::path artifact_path =
        std::filesystem::temp_directory_path() /
        "matter-gpu-mesher-synthetic-pbf.mhyd";
    if (const char* output =
            std::getenv("MATTER_GPU_MESHER_ACCEPTANCE_OUTPUT")) {
        if (*output != '\0') artifact_path = output;
    }
    error = {};
    GPU_CHECK(hydrology::save_artifact_atomic(artifact_path, artifact, error),
              error.message.empty() ? "save synthetic hydrology artifact"
                                    : error.message.c_str());

    if (const char* output =
            std::getenv("MATTER_CPU_MESHER_ACCEPTANCE_OUTPUT")) {
        if (*output != '\0') {
            hydrology::HydrologyArtifact cpu_artifact = artifact;
            cpu_artifact.visual_mesh = cpu_artifact.coarse_cpu_mesh;
            error = {};
            GPU_CHECK(hydrology::save_artifact_atomic(
                          std::filesystem::path(output), cpu_artifact, error),
                      error.message.empty()
                          ? "save CPU comparison hydrology artifact"
                          : error.message.c_str());
        }
    }
    if (const char* output =
            std::getenv("MATTER_CPU_VISUAL_MESHER_ACCEPTANCE_OUTPUT")) {
        if (*output != '\0') {
            hydrology::HydrologyArtifact cpu_visual_artifact = artifact;
            cpu_visual_artifact.visual_mesh = cpu_visual;
            error = {};
            GPU_CHECK(hydrology::save_artifact_atomic(
                          std::filesystem::path(output), cpu_visual_artifact,
                          error),
                      error.message.empty()
                          ? "save CPU visual-quality comparison artifact"
                          : error.message.c_str());
        }
    }

    const std::uint64_t submits_before_reload =
        matter::immediate_submit_count();
    hydrology::HydrologyArtifact loaded{};
    error = {};
    const bool load_ok = hydrology::load_artifact_validated(
        artifact_path, artifact.product_keys.visual, loaded, error);
    GPU_CHECK(load_ok,
              error.message.empty() ? "reload synthetic hydrology artifact"
                                    : error.message.c_str());
    GPU_CHECK(matter::immediate_submit_count() == submits_before_reload,
              "artifact reload performs no Vulkan mesher submission");
    if (!load_ok) return failures;
    GPU_CHECK(loaded.visual_mesh.positions == first.positions &&
                  loaded.visual_mesh.normals == first.normals &&
                  loaded.visual_mesh.indices == first.indices &&
                  loaded.visual_mesh.content_digest == first.content_digest,
              "artifact reload retains exact accepted GPU visual bytes");
    GPU_CHECK(loaded.visual_mesh.positions.data() !=
                  loaded.coarse_cpu_mesh.positions.data() &&
                  loaded.visual_mesh.indices != loaded.coarse_cpu_mesh.indices &&
                  !loaded.gameplay_field.empty(),
              "visual, coarse CPU, and gameplay products remain independent");

    std::shared_ptr<const viewer::VkScenePart> part;
    std::uint64_t instance_id = 0;
    error = {};
    GPU_CHECK(gpu_meshing::build_water_scene_part(
                  loaded.visual_mesh, loaded.payload_digest, 7u, part,
                  instance_id, error),
              error.message.empty() ? "convert cached water to glass part"
                                    : error.message.c_str());
    if (!part) return failures;
    GPU_CHECK(renderer.ensure_part(*part, renderer_error) >= 0,
              renderer_error.empty() ? "register cached water raster part"
                                     : renderer_error.c_str());
    const matter::Mat4f identity_transform = viewer::mat4_identity();
    viewer::VkSceneInstance instance{
        part->part_hash, identity_transform, instance_id};
    instance.ray_traced = false;
    GPU_CHECK(renderer.update_instances({instance}, renderer_error),
              renderer_error.empty() ? "register cached water instance"
                                     : renderer_error.c_str());

    std::uint32_t vertex_start = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    GPU_CHECK(renderer.part_raster_range(
                  part->part_hash, vertex_start, vertex_count,
                  index_start, index_count) &&
                  vertex_count == part->vertices.size() &&
                  index_count == part->indices.size(),
              "cached water occupies the ordinary indexed raster arenas");

    matter::CameraDesc camera{};
    camera.position = {7.0f, 6.0f, 11.0f};
    camera.target = {0.0f, 0.2f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 80.0f;
    viewer::FrameMatrices matrices{};
    GPU_CHECK(viewer::build_frame_matrices(camera, 320u, 200u, matrices,
                                           renderer_error) &&
                  renderer.dispatch_culling(matrices, camera.position, 1.0f,
                                             renderer_error),
              renderer_error.empty() ? "cull cached water through raster path"
                                     : renderer_error.c_str());
    viewer::VkCullStats cull{};
    GPU_CHECK(renderer.cull_stats(cull, renderer_error) && cull.emitted == 1u &&
                  cull.triangles == part->indices.size() / 3u,
              renderer_error.empty() ? "cached water emits its raster geometry"
                                     : renderer_error.c_str());

    std::vector<viewer::VkSceneRenderer::RtInstance> rt_instances;
    GPU_CHECK(renderer.fill_rt_instances(rt_instances) == 0,
              "cached water instance stays out of the native-RT registration lane");

    std::printf(
        "gpu-mesher acceptance: particles=%u grid=%u cells=%u active=%u "
        "triangles=%u digest=%016llx artifact=%s\n",
        first_stats.particles, first_stats.grid_vertices,
        first_stats.grid_cells, first_stats.active_cells,
        first_stats.triangles,
        static_cast<unsigned long long>(first.content_digest),
        artifact_path.string().c_str());
    const double gpu_first_ms = first_stats.bin_ms + first_stats.field_ms +
                                first_stats.classify_ms +
                                first_stats.emit_ms;
    std::printf(
        "gpu-mesher cold timings-ms: bin=%.3f field=%.3f classify=%.3f "
        "emit=%.3f total=%.3f\n",
        first_stats.bin_ms, first_stats.field_ms,
        first_stats.classify_ms, first_stats.emit_ms, gpu_first_ms);
    const double gpu_repeat_ms = second_stats.bin_ms + second_stats.field_ms +
                                 second_stats.classify_ms +
                                 second_stats.emit_ms;
    std::printf(
        "gpu-mesher warm timings-ms: bin=%.3f field=%.3f classify=%.3f "
        "emit=%.3f total=%.3f\n",
        second_stats.bin_ms, second_stats.field_ms,
        second_stats.classify_ms, second_stats.emit_ms, gpu_repeat_ms);
    std::printf(
        "iso-mesher coarse comparison: gpu-warm=%.3f ms gpu-triangles=%u "
        "cpu-cold=%.3f ms cpu-warm=%.3f ms cpu-triangles=%zu "
        "cpu/gpu-time=%.2fx\n",
        gpu_repeat_ms, second_stats.triangles, cpu_first_ms, cpu_repeat_ms,
        artifact.coarse_cpu_mesh.indices.size() / 3u,
        gpu_repeat_ms > 0.0 ? cpu_repeat_ms / gpu_repeat_ms : 0.0);
    std::printf(
        "iso-mesher visual comparison: gpu=%.3f ms at %.3f m (%u triangles) "
        "cpu=%.3f ms at requested %.3f m (%zu triangles) gpu-speedup=%.2fx\n",
        gpu_repeat_ms, job.voxel_m, second_stats.triangles, cpu_visual_ms,
        kCpuVisualComparisonVoxelM, cpu_visual.indices.size() / 3u,
        gpu_repeat_ms > 0.0 ? cpu_visual_ms / gpu_repeat_ms : 0.0);
    return failures;
}

namespace {

constexpr std::uint32_t kWaterfallCameraWidth = 1280u;
constexpr std::uint32_t kWaterfallCameraHeight = 720u;
constexpr float kWaterfallVerticalFov = 0.78539816339f;
constexpr double kWaterfallWorldBoundToleranceM = 0.001;
constexpr double kWaterfallWorldNumericalMarginM = 0.00001;
constexpr std::uint64_t kMeasuredRiverFloatNetworkBytes = 670161186ull;
constexpr std::uint64_t kMeasuredUpperSectionBytes = 349956449ull;
constexpr matter::Float3 kWaterfallSprayCenter{112.95f, 54.4f, 5.85f};

struct WaterfallQualityCandidate {
    const char* id = nullptr;
    float voxel_m = 0.0f;
    float radius_m = 0.0f;
    float blend_m = 0.0f;
};

struct WaterfallTopology {
    std::uint32_t connected_components = 0u;
    std::uint32_t intentional_spray_components = 0u;
    std::uint32_t open_edges = 0u;
    std::uint32_t holes = 0u;
    double normal_variation_degrees = 0.0;
};

struct WaterfallQualityRow {
    WaterfallQualityCandidate candidate{};
    gpu_meshing::MeshResult mesh{};
    gpu_meshing::Stats stats{};
    WaterfallTopology topology{};
    std::vector<std::uint8_t> silhouette;
    double silhouette_hausdorff_pixels = 0.0;
    double silhouette_hausdorff_m = 0.0;
    double off_sheet_coverage_fraction = 0.0;
    std::uint64_t projected_complete_animation_file_bytes = 0u;
};

matter::Float3 waterfall_add(matter::Float3 a, matter::Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

matter::Float3 waterfall_sub(matter::Float3 a, matter::Float3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

matter::Float3 waterfall_scale(matter::Float3 value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float waterfall_dot(matter::Float3 a, matter::Float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

matter::Float3 waterfall_cross(matter::Float3 a, matter::Float3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

matter::Float3 waterfall_normalize(matter::Float3 value) {
    const float length = std::sqrt(waterfall_dot(value, value));
    return length > 1.0e-8f ? waterfall_scale(value, 1.0f / length)
                            : matter::Float3{};
}

std::vector<gpu_meshing::ParticleSample> waterfall_fixture_particles(
    float radius_m) {
    std::vector<gpu_meshing::ParticleSample> particles;
    constexpr std::uint32_t kFallSamples = 61u;
    constexpr int kHalfWidthSamples = 10;
    particles.reserve(kFallSamples * (2 * kHalfWidthSamples + 1) + 8u);
    constexpr float kPi = 3.14159265358979323846f;
    for (std::uint32_t fall = 0u; fall != kFallSamples; ++fall) {
        const float t = static_cast<float>(fall) /
                        static_cast<float>(kFallSamples - 1u);
        const matter::Float3 center{
            110.0f + 0.55f * std::sin(1.35f * kPi * t),
            61.0f - 12.0f * t,
            5.0f + 0.35f * std::sin(2.0f * kPi * t)};
        for (int across = -kHalfWidthSamples;
             across <= kHalfWidthSamples; ++across) {
            const float offset = 0.20f * static_cast<float>(across);
            particles.push_back({
                {center.x + offset,
                 center.y + 0.025f * std::sin(0.7f * across + 4.0f * t),
                 center.z + 0.035f * std::sin(0.45f * across + 7.0f * t)},
                radius_m});
        }
    }

    // One deliberately disconnected spray bead gives the topology gate a
    // real intentional secondary component to preserve across all rows.
    for (std::uint32_t corner = 0u; corner != 8u; ++corner) {
        particles.push_back({
            {kWaterfallSprayCenter.x +
                 ((corner & 1u) ? 0.055f : -0.055f),
             kWaterfallSprayCenter.y +
                 ((corner & 2u) ? 0.055f : -0.055f),
             kWaterfallSprayCenter.z +
                 ((corner & 4u) ? 0.055f : -0.055f)},
            radius_m});
    }
    return particles;
}

gpu_meshing::ParticleJob waterfall_fixture_job(
    const std::vector<gpu_meshing::ParticleSample>& particles,
    const WaterfallQualityCandidate& candidate,
    gpu_meshing::Error& error) {
    gpu_meshing::ParticleJob job{};
    job.particles = particles.data();
    job.particle_count = static_cast<std::uint32_t>(particles.size());
    job.bounds_m = {{106.9f, 48.55f, 4.25f},
                    {113.45f, 61.45f, 6.35f}};
    job.voxel_m = candidate.voxel_m;
    job.blend_width_m = candidate.blend_m;
    job.iso_value = 0.0f;
    job.material = 4u;
    job.limits = {4096u, 4u << 20u, 16u << 20u, 16u << 20u};
    job.generation = 0x776174657266616cull;
    gpu_meshing::make_particle_sampling_lattice(
        {0.0f, 0.0f, 0.0f}, candidate.voxel_m,
        job.sampling_lattice, error);
    return job;
}

struct WaterfallDisjointSet {
    explicit WaterfallDisjointSet(std::size_t count) : parent(count) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    std::size_t find(std::size_t value) {
        while (parent[value] != value) {
            parent[value] = parent[parent[value]];
            value = parent[value];
        }
        return value;
    }
    void unite(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
    std::vector<std::size_t> parent;
};

WaterfallTopology waterfall_topology(
    const gpu_meshing::MeshResult& mesh) {
    WaterfallTopology result{};
    using Key = std::array<std::int64_t, 3>;
    // Adjacent GPU marching-cubes cells independently interpolate their
    // shared edge. Their float positions may differ by a few 1e-4 m. Search
    // neighbouring 1 mm buckets by distance so a bin boundary cannot turn
    // packing roundoff into a topology hole.
    constexpr double kWeldToleranceM = 0.001;
    constexpr double kWeldScale = 1.0 / kWeldToleranceM;
    std::map<Key, std::vector<std::uint32_t>> buckets;
    std::vector<matter::Float3> representatives;
    std::vector<std::uint32_t> welded(mesh.positions.size() / 3u);
    std::vector<std::vector<matter::Float3>> normals;
    for (std::size_t vertex = 0u; vertex != welded.size(); ++vertex) {
        const matter::Float3 position{
            mesh.positions[vertex * 3u + 0u],
            mesh.positions[vertex * 3u + 1u],
            mesh.positions[vertex * 3u + 2u]};
        const Key key{
            static_cast<std::int64_t>(std::llround(
                position.x * kWeldScale)),
            static_cast<std::int64_t>(std::llround(
                position.y * kWeldScale)),
            static_cast<std::int64_t>(std::llround(
                position.z * kWeldScale))};
        std::uint32_t matched = std::numeric_limits<std::uint32_t>::max();
        double matched_distance_squared =
            kWeldToleranceM * kWeldToleranceM;
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const auto bucket = buckets.find(
                        {key[0] + dx, key[1] + dy, key[2] + dz});
                    if (bucket == buckets.end()) continue;
                    for (std::uint32_t candidate : bucket->second) {
                        const matter::Float3 delta = waterfall_sub(
                            position, representatives[candidate]);
                        const double distance_squared =
                            waterfall_dot(delta, delta);
                        if (distance_squared <= matched_distance_squared) {
                            matched_distance_squared = distance_squared;
                            matched = candidate;
                        }
                    }
                }
            }
        }
        if (matched == std::numeric_limits<std::uint32_t>::max()) {
            matched = static_cast<std::uint32_t>(representatives.size());
            representatives.push_back(position);
            normals.emplace_back();
            buckets[key].push_back(matched);
        }
        welded[vertex] = matched;
        normals[matched].push_back({mesh.normals[vertex * 3u + 0u],
                                    mesh.normals[vertex * 3u + 1u],
                                    mesh.normals[vertex * 3u + 2u]});
    }
    WaterfallDisjointSet components(representatives.size());
    using Edge = std::array<std::uint32_t, 2>;
    std::map<Edge, std::uint32_t> edge_incidence;
    std::vector<bool> used(representatives.size(), false);
    for (std::size_t triangle = 0u;
         triangle != mesh.indices.size() / 3u; ++triangle) {
        std::array<std::uint32_t, 3> v{};
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            v[corner] = welded[mesh.indices[triangle * 3u + corner]];
            used[v[corner]] = true;
        }
        components.unite(v[0], v[1]);
        components.unite(v[1], v[2]);
        components.unite(v[2], v[0]);
        for (std::size_t edge = 0u; edge != 3u; ++edge) {
            std::uint32_t a = v[edge];
            std::uint32_t b = v[(edge + 1u) % 3u];
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            ++edge_incidence[{a, b}];
        }
    }
    std::map<std::size_t, std::size_t> component_sizes;
    std::map<std::size_t, matter::Float3> component_position_sums;
    for (std::size_t vertex = 0u; vertex != used.size(); ++vertex) {
        if (!used[vertex]) continue;
        const std::size_t root = components.find(vertex);
        ++component_sizes[root];
        component_position_sums[root] = waterfall_add(
            component_position_sums[root], representatives[vertex]);
    }
    result.connected_components =
        static_cast<std::uint32_t>(component_sizes.size());
    constexpr double kSprayCentroidRadiusM = 0.5;
    for (const auto& [root, count] : component_sizes) {
        const matter::Float3 centroid = waterfall_scale(
            component_position_sums[root], 1.0f / static_cast<float>(count));
        const matter::Float3 delta = waterfall_sub(
            centroid, kWaterfallSprayCenter);
        if (waterfall_dot(delta, delta) <=
            kSprayCentroidRadiusM * kSprayCentroidRadiusM) {
            ++result.intentional_spray_components;
        }
    }

    std::vector<std::vector<std::uint32_t>> boundary(representatives.size());
    for (const auto& [edge, incidence] : edge_incidence) {
        if (incidence != 1u) continue;
        ++result.open_edges;
        boundary[edge[0]].push_back(edge[1]);
        boundary[edge[1]].push_back(edge[0]);
    }
    std::vector<bool> visited(representatives.size(), false);
    for (std::size_t start = 0u; start != boundary.size(); ++start) {
        if (boundary[start].empty() || visited[start]) continue;
        ++result.holes;
        std::queue<std::uint32_t> pending;
        pending.push(static_cast<std::uint32_t>(start));
        visited[start] = true;
        while (!pending.empty()) {
            const std::uint32_t current = pending.front();
            pending.pop();
            for (std::uint32_t next : boundary[current]) {
                if (visited[next]) continue;
                visited[next] = true;
                pending.push(next);
            }
        }
    }

    std::vector<matter::Float3> mean_normals;
    mean_normals.reserve(normals.size());
    for (const auto& group : normals) {
        matter::Float3 mean{};
        for (matter::Float3 normal : group)
            mean = waterfall_add(mean, waterfall_normalize(normal));
        mean_normals.push_back(waterfall_normalize(mean));
    }
    std::vector<double> deviations;
    deviations.reserve(edge_incidence.size());
    for (const auto& [edge, incidence] : edge_incidence) {
        if (incidence < 1u) continue;
        const double dot = std::clamp<double>(
            waterfall_dot(mean_normals[edge[0]], mean_normals[edge[1]]),
            -1.0, 1.0);
        deviations.push_back(std::acos(dot) * 57.2957795130823208768);
    }
    if (!deviations.empty()) {
        std::sort(deviations.begin(), deviations.end());
        const std::size_t index = static_cast<std::size_t>(
            std::floor(0.95 * static_cast<double>(deviations.size() - 1u)));
        result.normal_variation_degrees = deviations[index];
    }
    return result;
}

struct WaterfallProjectedPoint {
    double x = 0.0;
    double y = 0.0;
    bool valid = false;
};

WaterfallProjectedPoint waterfall_project(matter::Float3 point) {
    constexpr matter::Float3 camera{119.0f, 72.0f, 2.0f};
    constexpr matter::Float3 target{110.0f, 49.0f, 5.0f};
    constexpr matter::Float3 world_up{0.0f, 1.0f, 0.0f};
    const matter::Float3 forward = waterfall_normalize(
        waterfall_sub(target, camera));
    const matter::Float3 right = waterfall_normalize(
        waterfall_cross(forward, world_up));
    const matter::Float3 up = waterfall_cross(right, forward);
    const matter::Float3 relative = waterfall_sub(point, camera);
    const double depth = waterfall_dot(relative, forward);
    if (depth <= 0.1) return {};
    const double tan_half = std::tan(0.5 * kWaterfallVerticalFov);
    const double aspect = static_cast<double>(kWaterfallCameraWidth) /
                          static_cast<double>(kWaterfallCameraHeight);
    const double ndc_x = waterfall_dot(relative, right) /
                         (depth * tan_half * aspect);
    const double ndc_y = waterfall_dot(relative, up) /
                         (depth * tan_half);
    return {(ndc_x * 0.5 + 0.5) * kWaterfallCameraWidth,
            (0.5 - ndc_y * 0.5) * kWaterfallCameraHeight, true};
}

double waterfall_edge(double ax, double ay, double bx, double by,
                      double px, double py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

std::vector<std::uint8_t> waterfall_silhouette(
    const gpu_meshing::MeshResult& mesh) {
    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(kWaterfallCameraWidth) *
        kWaterfallCameraHeight, 0u);
    std::vector<WaterfallProjectedPoint> projected(mesh.positions.size() / 3u);
    for (std::size_t vertex = 0u; vertex != projected.size(); ++vertex) {
        projected[vertex] = waterfall_project(
            {mesh.positions[vertex * 3u + 0u],
             mesh.positions[vertex * 3u + 1u],
             mesh.positions[vertex * 3u + 2u]});
    }
    for (std::size_t triangle = 0u;
         triangle != mesh.indices.size() / 3u; ++triangle) {
        const auto& a = projected[mesh.indices[triangle * 3u + 0u]];
        const auto& b = projected[mesh.indices[triangle * 3u + 1u]];
        const auto& c = projected[mesh.indices[triangle * 3u + 2u]];
        if (!a.valid || !b.valid || !c.valid) continue;
        const int min_x = std::max(0, static_cast<int>(
            std::floor(std::min({a.x, b.x, c.x}))));
        const int max_x = std::min(
            static_cast<int>(kWaterfallCameraWidth) - 1,
            static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int min_y = std::max(0, static_cast<int>(
            std::floor(std::min({a.y, b.y, c.y}))));
        const int max_y = std::min(
            static_cast<int>(kWaterfallCameraHeight) - 1,
            static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        const double area = waterfall_edge(a.x, a.y, b.x, b.y, c.x, c.y);
        if (std::fabs(area) <= 1.0e-10) continue;
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const double px = x + 0.5;
                const double py = y + 0.5;
                const double ab = waterfall_edge(a.x, a.y, b.x, b.y, px, py);
                const double bc = waterfall_edge(b.x, b.y, c.x, c.y, px, py);
                const double ca = waterfall_edge(c.x, c.y, a.x, a.y, px, py);
                const bool nonnegative = ab >= 0.0 && bc >= 0.0 && ca >= 0.0;
                const bool nonpositive = ab <= 0.0 && bc <= 0.0 && ca <= 0.0;
                if (nonnegative || nonpositive)
                    mask[static_cast<std::size_t>(y) *
                             kWaterfallCameraWidth + x] = 1u;
            }
        }
    }
    return mask;
}

using WaterfallPixel = std::array<int, 2>;

std::vector<WaterfallPixel> waterfall_outline(
    const std::vector<std::uint8_t>& mask) {
    std::vector<WaterfallPixel> outline;
    for (std::uint32_t y = 0u; y != kWaterfallCameraHeight; ++y) {
        for (std::uint32_t x = 0u; x != kWaterfallCameraWidth; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * kWaterfallCameraWidth + x;
            if (!mask[index]) continue;
            const bool boundary =
                x == 0u || y == 0u || x + 1u == kWaterfallCameraWidth ||
                y + 1u == kWaterfallCameraHeight ||
                !mask[index - 1u] || !mask[index + 1u] ||
                !mask[index - kWaterfallCameraWidth] ||
                !mask[index + kWaterfallCameraWidth];
            if (boundary)
                outline.push_back(
                    {static_cast<int>(x), static_cast<int>(y)});
        }
    }
    return outline;
}

double waterfall_directed_hausdorff(
    const std::vector<WaterfallPixel>& first,
    const std::vector<WaterfallPixel>& second) {
    if (first.empty() || second.empty())
        return std::numeric_limits<double>::infinity();
    std::int64_t maximum_squared = 0;
    for (const auto& point : first) {
        std::int64_t nearest_squared =
            std::numeric_limits<std::int64_t>::max();
        for (const auto& other : second) {
            const std::int64_t dx = point[0] - other[0];
            const std::int64_t dy = point[1] - other[1];
            nearest_squared = std::min(
                nearest_squared, dx * dx + dy * dy);
        }
        maximum_squared = std::max(maximum_squared, nearest_squared);
    }
    return std::sqrt(static_cast<double>(maximum_squared));
}

double waterfall_silhouette_hausdorff_pixels(
    const std::vector<std::uint8_t>& first,
    const std::vector<std::uint8_t>& second) {
    const auto first_outline = waterfall_outline(first);
    const auto second_outline = waterfall_outline(second);
    return std::max(waterfall_directed_hausdorff(first_outline, second_outline),
                    waterfall_directed_hausdorff(second_outline, first_outline));
}

struct WaterfallTriangle {
    matter::Float3 a{};
    matter::Float3 b{};
    matter::Float3 c{};
    matter::Float3 minimum{};
    matter::Float3 maximum{};
    matter::Float3 centroid{};
};

struct WaterfallBvhNode {
    matter::Float3 minimum{};
    matter::Float3 maximum{};
    std::uint32_t begin = 0u;
    std::uint32_t count = 0u;
    std::uint32_t left = 0u;
    std::uint32_t right = 0u;
};

matter::Float3 waterfall_min(matter::Float3 a, matter::Float3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

matter::Float3 waterfall_max(matter::Float3 a, matter::Float3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

struct WaterfallTriangleBvh {
    explicit WaterfallTriangleBvh(const gpu_meshing::MeshResult& mesh) {
        triangles.reserve(mesh.indices.size() / 3u);
        for (std::size_t triangle = 0u;
             triangle != mesh.indices.size() / 3u; ++triangle) {
            std::array<matter::Float3, 3> positions{};
            for (std::size_t corner = 0u; corner != 3u; ++corner) {
                const std::uint32_t index =
                    mesh.indices[triangle * 3u + corner];
                positions[corner] = {
                    mesh.positions[index * 3u + 0u],
                    mesh.positions[index * 3u + 1u],
                    mesh.positions[index * 3u + 2u]};
            }
            WaterfallTriangle value{};
            value.a = positions[0];
            value.b = positions[1];
            value.c = positions[2];
            value.minimum = waterfall_min(
                positions[0], waterfall_min(positions[1], positions[2]));
            value.maximum = waterfall_max(
                positions[0], waterfall_max(positions[1], positions[2]));
            value.centroid = waterfall_scale(
                waterfall_add(
                    waterfall_add(positions[0], positions[1]), positions[2]),
                1.0f / 3.0f);
            triangles.push_back(value);
        }
        order.resize(triangles.size());
        std::iota(order.begin(), order.end(), 0u);
        if (!order.empty()) build(0u, static_cast<std::uint32_t>(order.size()));
    }

    std::uint32_t build(std::uint32_t begin, std::uint32_t count) {
        WaterfallBvhNode node{};
        node.begin = begin;
        node.count = count;
        node.minimum = {std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
        node.maximum = {-std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()};
        matter::Float3 centroid_min = node.minimum;
        matter::Float3 centroid_max = node.maximum;
        for (std::uint32_t offset = 0u; offset != count; ++offset) {
            const auto& triangle = triangles[order[begin + offset]];
            node.minimum = waterfall_min(node.minimum, triangle.minimum);
            node.maximum = waterfall_max(node.maximum, triangle.maximum);
            centroid_min = waterfall_min(centroid_min, triangle.centroid);
            centroid_max = waterfall_max(centroid_max, triangle.centroid);
        }
        const std::uint32_t node_index =
            static_cast<std::uint32_t>(nodes.size());
        nodes.push_back(node);
        constexpr std::uint32_t kLeafTriangles = 12u;
        if (count <= kLeafTriangles) return node_index;
        const matter::Float3 span = waterfall_sub(centroid_max, centroid_min);
        const int axis = span.x >= span.y && span.x >= span.z
            ? 0 : (span.y >= span.z ? 1 : 2);
        const auto coordinate = [axis](const matter::Float3& value) {
            return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
        };
        const std::uint32_t middle = begin + count / 2u;
        std::nth_element(
            order.begin() + begin, order.begin() + middle,
            order.begin() + begin + count,
            [&](std::uint32_t a, std::uint32_t b) {
                const float first = coordinate(triangles[a].centroid);
                const float second = coordinate(triangles[b].centroid);
                return first == second ? a < b : first < second;
            });
        nodes[node_index].count = 0u;
        nodes[node_index].left = build(begin, middle - begin);
        nodes[node_index].right = build(middle, begin + count - middle);
        return node_index;
    }

    std::vector<WaterfallTriangle> triangles;
    std::vector<std::uint32_t> order;
    std::vector<WaterfallBvhNode> nodes;
};

double waterfall_point_aabb_distance_squared(
    matter::Float3 point, matter::Float3 minimum, matter::Float3 maximum) {
    const auto axis_distance = [](float value, float low, float high) {
        return value < low ? static_cast<double>(low - value)
                           : (value > high
                                  ? static_cast<double>(value - high)
                                  : 0.0);
    };
    const double dx = axis_distance(point.x, minimum.x, maximum.x);
    const double dy = axis_distance(point.y, minimum.y, maximum.y);
    const double dz = axis_distance(point.z, minimum.z, maximum.z);
    return dx * dx + dy * dy + dz * dz;
}

double waterfall_point_triangle_distance_squared(
    matter::Float3 point, const WaterfallTriangle& triangle) {
    const matter::Float3 ab = waterfall_sub(triangle.b, triangle.a);
    const matter::Float3 ac = waterfall_sub(triangle.c, triangle.a);
    const matter::Float3 ap = waterfall_sub(point, triangle.a);
    const double d1 = waterfall_dot(ab, ap);
    const double d2 = waterfall_dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return waterfall_dot(ap, ap);
    const matter::Float3 bp = waterfall_sub(point, triangle.b);
    const double d3 = waterfall_dot(ab, bp);
    const double d4 = waterfall_dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return waterfall_dot(bp, bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double v = d1 / (d1 - d3);
        const matter::Float3 nearest = waterfall_add(
            triangle.a, waterfall_scale(ab, static_cast<float>(v)));
        const matter::Float3 delta = waterfall_sub(point, nearest);
        return waterfall_dot(delta, delta);
    }
    const matter::Float3 cp = waterfall_sub(point, triangle.c);
    const double d5 = waterfall_dot(ab, cp);
    const double d6 = waterfall_dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return waterfall_dot(cp, cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double w = d2 / (d2 - d6);
        const matter::Float3 nearest = waterfall_add(
            triangle.a, waterfall_scale(ac, static_cast<float>(w)));
        const matter::Float3 delta = waterfall_sub(point, nearest);
        return waterfall_dot(delta, delta);
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const matter::Float3 bc = waterfall_sub(triangle.c, triangle.b);
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const matter::Float3 nearest = waterfall_add(
            triangle.b, waterfall_scale(bc, static_cast<float>(w)));
        const matter::Float3 delta = waterfall_sub(point, nearest);
        return waterfall_dot(delta, delta);
    }
    const double inverse = 1.0 / (va + vb + vc);
    const double v = vb * inverse;
    const double w = vc * inverse;
    const matter::Float3 nearest = waterfall_add(
        triangle.a,
        waterfall_add(waterfall_scale(ab, static_cast<float>(v)),
                      waterfall_scale(ac, static_cast<float>(w))));
    const matter::Float3 delta = waterfall_sub(point, nearest);
    return waterfall_dot(delta, delta);
}

double waterfall_nearest_triangle_distance_squared(
    matter::Float3 point, const WaterfallTriangleBvh& bvh,
    std::uint32_t node_index, double best) {
    const auto& node = bvh.nodes[node_index];
    if (waterfall_point_aabb_distance_squared(
            point, node.minimum, node.maximum) >= best) {
        return best;
    }
    if (node.count != 0u) {
        for (std::uint32_t offset = 0u; offset != node.count; ++offset) {
            best = std::min(best, waterfall_point_triangle_distance_squared(
                point, bvh.triangles[bvh.order[node.begin + offset]]));
        }
        return best;
    }
    const auto& left = bvh.nodes[node.left];
    const auto& right = bvh.nodes[node.right];
    const double left_distance = waterfall_point_aabb_distance_squared(
        point, left.minimum, left.maximum);
    const double right_distance = waterfall_point_aabb_distance_squared(
        point, right.minimum, right.maximum);
    const std::uint32_t first =
        left_distance <= right_distance ? node.left : node.right;
    const std::uint32_t second =
        left_distance <= right_distance ? node.right : node.left;
    best = waterfall_nearest_triangle_distance_squared(
        point, bvh, first, best);
    return waterfall_nearest_triangle_distance_squared(
        point, bvh, second, best);
}

double waterfall_nearest_triangle_distance(
    matter::Float3 point, const WaterfallTriangleBvh& bvh) {
    return std::sqrt(waterfall_nearest_triangle_distance_squared(
        point, bvh, 0u, std::numeric_limits<double>::infinity()));
}

struct WaterfallHausdorffPatch {
    std::array<matter::Float3, 3> vertices{};
    double upper_bound_m = 0.0;
};

struct WaterfallHausdorffPatchLess {
    bool operator()(const WaterfallHausdorffPatch& a,
                    const WaterfallHausdorffPatch& b) const noexcept {
        return a.upper_bound_m < b.upper_bound_m;
    }
};

WaterfallHausdorffPatch waterfall_hausdorff_patch(
    const std::array<matter::Float3, 3>& vertices,
    const WaterfallTriangleBvh& destination,
    double& lower_bound_m) {
    const matter::Float3 centroid = waterfall_scale(
        waterfall_add(waterfall_add(vertices[0], vertices[1]), vertices[2]),
        1.0f / 3.0f);
    const double center_distance =
        waterfall_nearest_triangle_distance(centroid, destination);
    lower_bound_m = std::max(lower_bound_m, center_distance);
    double radius = 0.0;
    for (matter::Float3 vertex : vertices) {
        const matter::Float3 delta = waterfall_sub(vertex, centroid);
        radius = std::max(
            radius,
            std::sqrt(static_cast<double>(waterfall_dot(delta, delta))));
    }
    // Distance to a closed set is 1-Lipschitz. Every point in the flat
    // triangle lies within `radius` of its centroid, therefore this is a
    // conservative bound for the entire patch, not a sampled lower bound.
    return {vertices, center_distance + radius + 1.0e-6};
}

std::array<std::array<matter::Float3, 3>, 4>
waterfall_split_hausdorff_patch(const WaterfallHausdorffPatch& patch) {
    const auto midpoint = [](matter::Float3 a, matter::Float3 b) {
        return waterfall_scale(waterfall_add(a, b), 0.5f);
    };
    const matter::Float3 ab = midpoint(
        patch.vertices[0], patch.vertices[1]);
    const matter::Float3 bc = midpoint(
        patch.vertices[1], patch.vertices[2]);
    const matter::Float3 ca = midpoint(
        patch.vertices[2], patch.vertices[0]);
    return {{{patch.vertices[0], ab, ca},
             {ab, patch.vertices[1], bc},
             {ca, bc, patch.vertices[2]},
             {ab, bc, ca}}};
}

double waterfall_directed_world_hausdorff(
    const gpu_meshing::MeshResult& source,
    const WaterfallTriangleBvh& destination) {
    if (destination.nodes.empty())
        return std::numeric_limits<double>::infinity();
    double lower_bound_m = 0.0;
    for (std::size_t vertex = 0u;
         vertex != source.positions.size() / 3u; ++vertex) {
        lower_bound_m = std::max(
            lower_bound_m, waterfall_nearest_triangle_distance(
                {source.positions[vertex * 3u + 0u],
                 source.positions[vertex * 3u + 1u],
                 source.positions[vertex * 3u + 2u]},
                destination));
    }
    std::priority_queue<
        WaterfallHausdorffPatch,
        std::vector<WaterfallHausdorffPatch>,
        WaterfallHausdorffPatchLess> pending;
    for (std::size_t triangle = 0u;
         triangle != source.indices.size() / 3u; ++triangle) {
        std::array<matter::Float3, 3> vertices{};
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const std::uint32_t index =
                source.indices[triangle * 3u + corner];
            vertices[corner] = {
                 source.positions[index * 3u + 0u],
                 source.positions[index * 3u + 1u],
                 source.positions[index * 3u + 2u]};
        }
        pending.push(waterfall_hausdorff_patch(
            vertices, destination, lower_bound_m));
    }
    while (!pending.empty()) {
        const WaterfallHausdorffPatch patch = pending.top();
        if (patch.upper_bound_m <=
            lower_bound_m + kWaterfallWorldBoundToleranceM) {
            return patch.upper_bound_m + kWaterfallWorldNumericalMarginM;
        }
        pending.pop();
        for (const auto& child : waterfall_split_hausdorff_patch(patch)) {
            pending.push(waterfall_hausdorff_patch(
                child, destination, lower_bound_m));
        }
    }
    return lower_bound_m + kWaterfallWorldNumericalMarginM;
}

double waterfall_world_geometry_hausdorff(
    const gpu_meshing::MeshResult& first,
    const gpu_meshing::MeshResult& second) {
    if (&first == &second) return 0.0;
    const WaterfallTriangleBvh first_bvh(first);
    const WaterfallTriangleBvh second_bvh(second);
    return std::max(waterfall_directed_world_hausdorff(first, second_bvh),
                    waterfall_directed_world_hausdorff(second, first_bvh));
}

double waterfall_off_sheet_coverage(
    const std::vector<std::uint8_t>& candidate,
    const std::vector<std::uint8_t>& oracle) {
    std::uint64_t outside = 0u;
    std::uint64_t oracle_coverage = 0u;
    for (std::size_t index = 0u; index != oracle.size(); ++index) {
        if (oracle[index]) ++oracle_coverage;
        if (candidate[index] && !oracle[index]) ++outside;
    }
    return oracle_coverage == 0u
        ? std::numeric_limits<double>::infinity()
        : static_cast<double>(outside) /
              static_cast<double>(oracle_coverage);
}

std::uint64_t waterfall_projected_animation_file_bytes(
    const gpu_meshing::MeshResult& mesh) {
    constexpr std::uint64_t kHeaderBytes = 32u;
    constexpr std::uint64_t kIdentityBytes = 27u;
    constexpr std::uint64_t kMetadataAfterIdentityBytes =
        24u + 16u + 8u + 16u + 4u + 24u + 8u;
    constexpr std::uint64_t kFrameDirectoryBytes = 30u * 56u;
    constexpr std::uint64_t kPayloadSizeFieldBytes = 8u;
    const std::uint64_t vertex_count = mesh.positions.size() / 3u;
    const std::uint64_t index_count = mesh.indices.size();
    const std::uint64_t frame_payload_bytes =
        vertex_count * 12u + index_count * 4u;
    return kHeaderBytes + 4u + kIdentityBytes +
           kMetadataAfterIdentityBytes + kFrameDirectoryBytes +
           kPayloadSizeFieldBytes + 30u * frame_payload_bytes;
}

bool waterfall_write_report(
    const std::filesystem::path& path,
    const std::vector<WaterfallQualityRow>& rows,
    std::uint32_t validation_errors) {
    if (path.empty() || rows.size() != 5u) return false;
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(
            path.parent_path(), filesystem_error);
        if (filesystem_error) return false;
    }
    const auto current = std::find_if(
        rows.begin(), rows.end(), [](const WaterfallQualityRow& row) {
            return std::string(row.candidate.id) == "v150-r013-b010";
        });
    if (current == rows.end() ||
        current->projected_complete_animation_file_bytes >
            kMeasuredRiverFloatNetworkBytes ||
        current->projected_complete_animation_file_bytes >
            kMeasuredUpperSectionBytes) {
        return false;
    }
    const std::uint64_t network_other =
        kMeasuredRiverFloatNetworkBytes -
        current->projected_complete_animation_file_bytes;
    const std::uint64_t section_other =
        kMeasuredUpperSectionBytes -
        current->projected_complete_animation_file_bytes;

    std::ostringstream json;
    json << std::setprecision(9) << std::fixed;
    json << "{\n"
         << "  \"schemaVersion\": 1,\n"
         << "  \"fixture\": {\"fallDistanceM\": 12.0, "
            "\"sheetDiameterM\": 0.26, \"frameCount\": 30, "
         << "\"cameraWidthPixels\": " << kWaterfallCameraWidth << ", "
         << "\"cameraHeightPixels\": " << kWaterfallCameraHeight << ", "
         << "\"worldHausdorffBoundToleranceM\": "
         << kWaterfallWorldBoundToleranceM << ", "
         << "\"worldHausdorffNumericalMarginM\": "
         << kWaterfallWorldNumericalMarginM << "},\n"
         << "  \"oracleRowId\": \"v075-r013-b010\",\n"
         << "  \"measuredRiverFloatLabNetworkBytes\": "
         << kMeasuredRiverFloatNetworkBytes << ",\n"
         << "  \"measuredContainingSectionFileBytes\": "
         << kMeasuredUpperSectionBytes << ",\n"
         << "  \"networkOtherCompleteFileBytes\": " << network_other
         << ",\n"
         << "  \"containingSectionOtherCompleteFileBytes\": "
         << section_other << ",\n"
         << "  \"validationErrors\": " << validation_errors << ",\n"
         << "  \"rows\": [\n";
    for (std::size_t index = 0u; index != rows.size(); ++index) {
        const auto& row = rows[index];
        const double gpu_ms = row.stats.bin_ms + row.stats.field_ms +
                              row.stats.classify_ms + row.stats.emit_ms;
        json << "    {\"id\": \"" << row.candidate.id << "\", "
             << "\"voxelM\": " << row.candidate.voxel_m << ", "
             << "\"radiusM\": " << row.candidate.radius_m << ", "
             << "\"blendWidthM\": " << row.candidate.blend_m << ", "
             << "\"vertices\": " << row.mesh.positions.size() / 3u << ", "
             << "\"triangles\": " << row.mesh.indices.size() / 3u << ", "
             << "\"connectedComponents\": "
             << row.topology.connected_components << ", "
             << "\"intentionalSprayComponents\": "
             << row.topology.intentional_spray_components << ", "
             << "\"openEdges\": " << row.topology.open_edges << ", "
             << "\"holes\": " << row.topology.holes << ", "
             << "\"silhouetteHausdorffM\": "
             << row.silhouette_hausdorff_m << ", "
             << "\"silhouetteHausdorffPixels\": "
             << row.silhouette_hausdorff_pixels << ", "
             << "\"normalVariationDegrees\": "
             << row.topology.normal_variation_degrees << ", "
             << "\"offSheetCoverageFraction\": "
             << row.off_sheet_coverage_fraction << ", "
             << "\"projectedCompleteAnimationFileBytes\": "
             << row.projected_complete_animation_file_bytes << ", "
             << "\"gpuMeshMs\": " << gpu_ms << ", "
             << "\"meshDigest\": \"" << std::hex << std::setw(16)
             << std::setfill('0') << row.mesh.content_digest << std::dec
             << std::setfill(' ') << "\"}";
        json << (index + 1u == rows.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";

    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << json.str();
        if (!output) return false;
    }
    std::filesystem::remove(path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

}  // namespace

int run_gpu_visual_mesher_waterfall_quality(
    matter::VulkanDevice& vulkan, const std::filesystem::path& report) {
    failures = 0;
    GPU_CHECK(!report.empty(),
              "waterfall-mesher requires MATTER_WATERFALL_QUALITY_REPORT");
    if (report.empty()) return failures;
    constexpr std::array<WaterfallQualityCandidate, 5> kCandidates{{
        {"v150-r013-b010", 0.15f, 0.13f, 0.10f},
        {"v100-r013-b010", 0.10f, 0.13f, 0.10f},
        {"v075-r013-b010", 0.075f, 0.13f, 0.10f},
        {"v150-r014-b010", 0.15f, 0.14f, 0.10f},
        {"v150-r013-b012", 0.15f, 0.13f, 0.12f},
    }};
    gpu_meshing::GpuVisualMesher mesher(vulkan);

    // One unreported finest-grid dispatch removes pipeline first-use and
    // grows reusable allocations to the matrix high-water mark before any
    // per-row GPU timing is recorded.
    {
        auto warm_particles = waterfall_fixture_particles(0.13f);
        gpu_meshing::Error warm_error{};
        auto warm_job = waterfall_fixture_job(
            warm_particles, kCandidates[2], warm_error);
        gpu_meshing::MeshResult warm_mesh{};
        gpu_meshing::Stats warm_stats{};
        GPU_CHECK(warm_error.code == gpu_meshing::ErrorCode::None &&
                      mesher.build_particle_visual(
                          warm_job, warm_mesh, warm_stats, warm_error),
                  warm_error.message.empty()
                      ? "warm waterfall GPU measurement pipeline"
                      : warm_error.message.c_str());
        if (failures != 0) return failures;
    }

    std::vector<WaterfallQualityRow> rows;
    rows.reserve(kCandidates.size());
    for (const auto& candidate : kCandidates) {
        auto particles = waterfall_fixture_particles(candidate.radius_m);
        gpu_meshing::Error error{};
        const auto job = waterfall_fixture_job(particles, candidate, error);
        WaterfallQualityRow row{};
        row.candidate = candidate;
        const bool built = error.code == gpu_meshing::ErrorCode::None &&
            mesher.build_particle_visual(job, row.mesh, row.stats, error);
        GPU_CHECK(built,
                  error.message.empty()
                      ? "build waterfall quality candidate"
                      : error.message.c_str());
        if (!built) return failures;
        GPU_CHECK(!row.mesh.positions.empty() && !row.mesh.indices.empty(),
                  "waterfall quality candidate emits geometry");
        row.topology = waterfall_topology(row.mesh);
        GPU_CHECK(row.topology.open_edges == 0u && row.topology.holes == 0u,
                  "closed waterfall fixture has no topology holes");
        GPU_CHECK(row.topology.connected_components == 2u &&
                      row.topology.intentional_spray_components == 1u,
                  "waterfall fixture preserves one intentional spray bead");
        GPU_CHECK(std::isfinite(row.topology.normal_variation_degrees) &&
                      row.topology.normal_variation_degrees > 0.0,
                  "waterfall topology reports finite nonzero normal variation");
        row.silhouette = waterfall_silhouette(row.mesh);
        GPU_CHECK(std::any_of(row.silhouette.begin(), row.silhouette.end(),
                              [](std::uint8_t value) { return value != 0u; }),
                  "waterfall quality candidate projects into retained camera");
        row.projected_complete_animation_file_bytes =
            waterfall_projected_animation_file_bytes(row.mesh);
        rows.push_back(std::move(row));
    }
    if (failures != 0) return failures;

    const auto oracle = std::find_if(
        rows.begin(), rows.end(), [](const WaterfallQualityRow& row) {
            return std::string(row.candidate.id) == "v075-r013-b010";
        });
    GPU_CHECK(oracle != rows.end(), "waterfall quality matrix has 0.075 oracle");
    if (oracle == rows.end()) return failures;
    for (auto& row : rows) {
        row.silhouette_hausdorff_pixels =
            waterfall_silhouette_hausdorff_pixels(
                row.silhouette, oracle->silhouette);
        row.silhouette_hausdorff_m = waterfall_world_geometry_hausdorff(
            row.mesh, oracle->mesh);
        row.off_sheet_coverage_fraction =
            waterfall_off_sheet_coverage(row.silhouette, oracle->silhouette);
        const double gpu_ms = row.stats.bin_ms + row.stats.field_ms +
                              row.stats.classify_ms + row.stats.emit_ms;
        std::printf(
            "waterfall-quality %s: vertices=%zu triangles=%zu components=%u "
            "open=%u holes=%u silhouette=%.6f m/%.3f px normal=%.3f deg "
            "coverage=%.6f file=%llu gpu=%.3f ms digest=%016llx\n",
            row.candidate.id, row.mesh.positions.size() / 3u,
            row.mesh.indices.size() / 3u,
            row.topology.connected_components, row.topology.open_edges,
            row.topology.holes, row.silhouette_hausdorff_m,
            row.silhouette_hausdorff_pixels,
            row.topology.normal_variation_degrees,
            row.off_sheet_coverage_fraction,
            static_cast<unsigned long long>(
                row.projected_complete_animation_file_bytes),
            gpu_ms,
            static_cast<unsigned long long>(row.mesh.content_digest));
    }
    const auto current = std::find_if(
        rows.begin(), rows.end(), [](const WaterfallQualityRow& row) {
            return std::string(row.candidate.id) == "v150-r013-b010";
        });
    GPU_CHECK(current != rows.end(), "waterfall matrix has current 0.15 row");
    if (current != rows.end()) {
        GPU_CHECK(current->silhouette_hausdorff_m > 0.0 &&
                      current->silhouette_hausdorff_pixels > 0.0 &&
                      current->mesh.content_digest !=
                          oracle->mesh.content_digest,
                  "0.15 fixture metrics distinguish it from 0.075 oracle");
        GPU_CHECK(oracle->silhouette_hausdorff_m == 0.0 &&
                      oracle->silhouette_hausdorff_pixels == 0.0 &&
                      oracle->off_sheet_coverage_fraction == 0.0,
                  "oracle self-comparison is exactly zero");
    }
    GPU_CHECK(waterfall_write_report(
                  report, rows, vulkan.validation_error_count()),
              "write waterfall visual quality JSON report");
    return failures;
}
