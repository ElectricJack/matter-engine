#pragma once

#include "math_types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gpu_meshing {

enum class ErrorCode : std::uint8_t {
    None,
    InvalidInput,
    LimitExceeded,
    Overflow,
    Unavailable,
    Cancelled,
    StaleGeneration,
    DeviceLost,
    VulkanFailure,
    ArtifactFailure,
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
};

struct Aabb {
    matter::Float3 min_m{};
    matter::Float3 max_m{};
};

struct ParticleSample {
    matter::Float3 position_m{};
    float radius_m = 0.0f;
};

struct Limits {
    std::uint32_t max_particles = 0;
    std::uint32_t max_grid_vertices = 0;
    std::uint32_t max_mesh_vertices = 0;
    std::uint32_t max_mesh_indices = 0;
};

struct ParticlePhaseBlend {
    std::uint32_t split_index = 0;
    float primary_weight = 1.0f;
    float secondary_weight = 0.0f;
};

struct ParticleSourcePhaseSpan {
    std::uint32_t primary_begin = 0;
    std::uint32_t primary_count = 0;
    std::uint32_t secondary_begin = 0;
    std::uint32_t secondary_count = 0;
};

struct ParticleLongitudinalFieldBlend {
    std::array<ParticleSourcePhaseSpan, 2> source{};
    matter::Float3 origin_m{};
    matter::Float3 direction{};
    float upstream_full_m = 0.0f;
    float downstream_full_m = 0.0f;
    bool enabled = false;
};

struct ParticleSamplingLattice {
    matter::Float3 origin_m{};
    float voxel_m = 0.0f;
    std::uint32_t version = 0;  // 0 legacy/tight; 1 canonical
};

struct ParticleJob {
    const ParticleSample* particles = nullptr;
    std::uint32_t particle_count = 0;
    Aabb bounds_m{};
    float voxel_m = 0.0f;
    float blend_width_m = 0.0f;
    float iso_value = 0.0f;
    std::uint32_t material = 4;
    Limits limits{};
    std::uint64_t generation = 0;
    ParticlePhaseBlend phase_blend{};
    ParticleSamplingLattice sampling_lattice{};
    ParticleLongitudinalFieldBlend longitudinal_field_blend{};
};

struct BuildControl {
    std::function<bool()> cancelled;
    std::function<bool(std::uint64_t generation)> generation_is_current;
};

struct GridLayout {
    matter::Float3 origin_m{};
    matter::Float3 spacing_m{};
    matter::Float3 bin_origin_m{};
    std::array<std::uint32_t, 3> sample_dims{};
    std::array<std::uint32_t, 3> cell_dims{};
    std::array<std::uint32_t, 3> bin_dims{};
    std::uint32_t grid_vertices = 0;
    std::uint32_t grid_cells = 0;
    std::uint32_t bins = 0;
    float bin_size_m = 0.0f;
    float query_radius_m = 0.0f;
    std::array<std::int64_t, 3> cell_min{};
};

struct MeshResult {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint32_t> indices;
    std::uint32_t material = 0;
    std::uint64_t content_digest = 0;
};

struct Stats {
    std::uint32_t particles = 0;
    std::uint32_t bins = 0;
    std::uint32_t grid_vertices = 0;
    std::uint32_t grid_cells = 0;
    std::uint32_t active_cells = 0;
    std::uint32_t triangles = 0;
    std::uint64_t device_bytes = 0;
    double bin_ms = 0.0;
    double field_ms = 0.0;
    double classify_ms = 0.0;
    double emit_ms = 0.0;
};

bool validate_particle_job(const ParticleJob& job, GridLayout& layout,
                           Error& error);

bool make_particle_sampling_lattice(
    const matter::Float3& world_anchor_m, float voxel_m,
    ParticleSamplingLattice& lattice, Error& error);

bool particle_field_support_radius_m(
    float particle_radius_m, float blend_width_m,
    float& support_radius_m, Error& error);

std::uint32_t resolved_particle_phase_split(
    const ParticleJob& job) noexcept;

float evaluate_particle_field_reference(const ParticleSample* particles,
                                        std::uint32_t particle_count,
                                        float blend_width_m,
                                        matter::Float3 point_m);

float evaluate_particle_field_reference(
    const ParticleSample* particles,
    std::uint32_t particle_count,
    float blend_width_m,
    ParticlePhaseBlend phase_blend,
    matter::Float3 point_m);

float evaluate_particle_field_reference(
    const ParticleSample* particles,
    std::uint32_t particle_count,
    float blend_width_m,
    ParticlePhaseBlend phase_blend,
    const ParticleLongitudinalFieldBlend& longitudinal_blend,
    matter::Float3 point_m);

bool exclusive_scan_reference(const std::vector<std::uint32_t>& input,
                              std::vector<std::uint32_t>& output,
                              std::uint32_t& total);

std::uint64_t mesh_content_digest(const MeshResult& mesh);

}  // namespace gpu_meshing
