#include "matter/gpu_visual_meshing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gpu_meshing {
namespace {

bool finite(float value) noexcept { return std::isfinite(value); }

bool finite(matter::Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool fail(Error& error, ErrorCode code, const char* message) {
    error.code = code;
    error.message = message;
    return false;
}

bool dimension_for_extent(float extent, float voxel, std::uint32_t& cells,
                          Error& error) {
    const double quotient = static_cast<double>(extent) /
                            static_cast<double>(voxel);
    const double rounded = std::ceil(quotient);
    if (!std::isfinite(rounded) || rounded < 1.0 ||
        rounded > static_cast<double>(
                      std::numeric_limits<std::uint32_t>::max() - 1u)) {
        return fail(error, ErrorCode::Overflow,
                    "particle grid dimensions overflow uint32");
    }
    cells = static_cast<std::uint32_t>(rounded);
    return true;
}

bool checked_product(const std::array<std::uint32_t, 3>& dims,
                     std::uint32_t& result) noexcept {
    std::uint64_t product = 1;
    for (const std::uint32_t dim : dims) {
        product *= dim;
        if (product > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    result = static_cast<std::uint32_t>(product);
    return true;
}

class Digest64 {
public:
    void byte(std::uint8_t value) noexcept {
        state_ ^= value;
        state_ *= 1099511628211ull;
    }

    void u32(std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift != 32; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }

    void u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift != 64; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }

    void floats(const std::vector<float>& values) noexcept {
        u64(static_cast<std::uint64_t>(values.size()));
        for (float value : values) {
            std::uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(value));
            std::memcpy(&bits, &value, sizeof(bits));
            u32(bits);
        }
    }

    void uints(const std::vector<std::uint32_t>& values) noexcept {
        u64(static_cast<std::uint64_t>(values.size()));
        for (const std::uint32_t value : values) u32(value);
    }

    std::uint64_t finish() const noexcept {
        return state_ != 0 ? state_ : 0xcbf29ce484222325ull;
    }

private:
    std::uint64_t state_ = 1469598103934665603ull;
};

}  // namespace

bool validate_particle_job(const ParticleJob& job, GridLayout& layout,
                           Error& error) {
    layout = {};
    error = {};
    if (!finite(job.bounds_m.min_m) || !finite(job.bounds_m.max_m))
        return fail(error, ErrorCode::InvalidInput,
                    "particle mesh bounds must be finite");
    const matter::Float3 extent{
        job.bounds_m.max_m.x - job.bounds_m.min_m.x,
        job.bounds_m.max_m.y - job.bounds_m.min_m.y,
        job.bounds_m.max_m.z - job.bounds_m.min_m.z,
    };
    if (!finite(extent) || extent.x <= 0.0f || extent.y <= 0.0f ||
        extent.z <= 0.0f) {
        return fail(error, ErrorCode::InvalidInput,
                    "particle mesh bounds must have positive finite extent");
    }
    if (!finite(job.voxel_m) || job.voxel_m <= 0.0f)
        return fail(error, ErrorCode::InvalidInput,
                    "particle mesh voxel size must be positive and finite");
    if (!finite(job.blend_width_m) || job.blend_width_m < 0.0f)
        return fail(error, ErrorCode::InvalidInput,
                    "particle mesh blend width must be nonnegative and finite");
    const ParticlePhaseBlend& phase = job.phase_blend;
    if (phase.split_index > job.particle_count)
        return fail(error, ErrorCode::InvalidInput,
                    "particle phase split exceeds particle count");
    if (!finite(phase.primary_weight) ||
        !finite(phase.secondary_weight) ||
        phase.primary_weight < 0.0f || phase.secondary_weight < 0.0f)
        return fail(error, ErrorCode::InvalidInput,
                    "particle phase weights must be finite and nonnegative");
    if (std::fabs((phase.primary_weight + phase.secondary_weight) - 1.0f) >
        1e-5f)
        return fail(error, ErrorCode::InvalidInput,
                    "particle phase weights must sum to one");
    if (phase.secondary_weight > 0.0f && job.blend_width_m <= 1e-5f)
        return fail(error, ErrorCode::InvalidInput,
                    "dual-phase particle meshing requires a positive blend width");
    if (!finite(job.iso_value))
        return fail(error, ErrorCode::InvalidInput,
                    "particle mesh isolevel must be finite");
    if (job.particle_count != 0 && job.particles == nullptr)
        return fail(error, ErrorCode::InvalidInput,
                    "particle mesh input pointer is null");
    if (job.particle_count > job.limits.max_particles)
        return fail(error, ErrorCode::LimitExceeded,
                    "particle mesh input exceeds max_particles");

    std::array<std::uint32_t, 3> cells{};
    if (!dimension_for_extent(extent.x, job.voxel_m, cells[0], error) ||
        !dimension_for_extent(extent.y, job.voxel_m, cells[1], error) ||
        !dimension_for_extent(extent.z, job.voxel_m, cells[2], error)) {
        return false;
    }
    layout.origin_m = job.bounds_m.min_m;
    layout.cell_dims = cells;
    for (std::size_t axis = 0; axis != 3; ++axis)
        layout.sample_dims[axis] = cells[axis] + 1u;
    layout.spacing_m = {extent.x / static_cast<float>(cells[0]),
                        extent.y / static_cast<float>(cells[1]),
                        extent.z / static_cast<float>(cells[2])};
    if (!checked_product(layout.sample_dims, layout.grid_vertices) ||
        !checked_product(layout.cell_dims, layout.grid_cells)) {
        return fail(error, ErrorCode::Overflow,
                    "particle grid element count overflows uint32");
    }
    if (layout.grid_vertices > job.limits.max_grid_vertices)
        return fail(error, ErrorCode::LimitExceeded,
                    "particle grid exceeds max_grid_vertices");
    if (job.particle_count != 0 &&
        (job.limits.max_mesh_vertices == 0 ||
         job.limits.max_mesh_indices == 0)) {
        return fail(error, ErrorCode::LimitExceeded,
                    "particle mesh output limits must be nonzero");
    }

    float max_radius = 0.0f;
    for (std::uint32_t index = 0; index != job.particle_count; ++index) {
        const ParticleSample& particle = job.particles[index];
        if (!finite(particle.position_m) || !finite(particle.radius_m) ||
            particle.radius_m <= 0.0f) {
            return fail(error, ErrorCode::InvalidInput,
                        "particle samples must contain finite positions and positive radii");
        }
        max_radius = std::max(max_radius, particle.radius_m);
    }
    if (job.particle_count == 0) return true;

    const double query_radius =
        static_cast<double>(max_radius) * 2.5 +
        static_cast<double>(job.blend_width_m) * 4.0;
    if (!std::isfinite(query_radius) || query_radius <= 0.0 ||
        query_radius > std::numeric_limits<float>::max()) {
        return fail(error, ErrorCode::Overflow,
                    "particle field query radius overflow");
    }
    layout.query_radius_m = static_cast<float>(query_radius);
    layout.bin_size_m = layout.query_radius_m;
    layout.bin_origin_m = {
        job.bounds_m.min_m.x - layout.query_radius_m,
        job.bounds_m.min_m.y - layout.query_radius_m,
        job.bounds_m.min_m.z - layout.query_radius_m,
    };
    if (!finite(layout.bin_origin_m))
        return fail(error, ErrorCode::Overflow,
                    "particle bin origin overflow");

    const float expanded[] = {
        extent.x + 2.0f * layout.query_radius_m,
        extent.y + 2.0f * layout.query_radius_m,
        extent.z + 2.0f * layout.query_radius_m,
    };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        const double count = std::ceil(
            static_cast<double>(expanded[axis]) / layout.bin_size_m);
        if (!std::isfinite(count) || count < 1.0 ||
            count > std::numeric_limits<std::uint32_t>::max()) {
            return fail(error, ErrorCode::Overflow,
                        "particle bin dimensions overflow uint32");
        }
        layout.bin_dims[axis] = static_cast<std::uint32_t>(count);
    }
    if (!checked_product(layout.bin_dims, layout.bins))
        return fail(error, ErrorCode::Overflow,
                    "particle bin count overflows uint32");
    if (layout.bins > job.limits.max_grid_vertices)
        return fail(error, ErrorCode::LimitExceeded,
                    "particle bins exceed max_grid_vertices");
    return true;
}

std::uint32_t resolved_particle_phase_split(
    const ParticleJob& job) noexcept {
    return job.phase_blend.split_index == 0u &&
            job.phase_blend.primary_weight == 1.0f &&
            job.phase_blend.secondary_weight == 0.0f
        ? job.particle_count
        : job.phase_blend.split_index;
}

float evaluate_particle_field_reference(const ParticleSample* particles,
                                        std::uint32_t particle_count,
                                        float blend_width_m,
                                        matter::Float3 point_m) {
    if (particles == nullptr || particle_count == 0)
        return std::numeric_limits<float>::infinity();
    float max_radius = 0.0f;
    for (std::uint32_t index = 0; index != particle_count; ++index)
        max_radius = std::max(max_radius, particles[index].radius_m);
    const float query_radius = max_radius * 2.5f + blend_width_m * 4.0f;
    const float query_radius_squared = query_radius * query_radius;

    float minimum = std::numeric_limits<float>::infinity();
    std::uint32_t neighbors = 0;
    for (std::uint32_t index = 0; index != particle_count; ++index) {
        const matter::Float3 center = particles[index].position_m;
        const float dx = point_m.x - center.x;
        const float dy = point_m.y - center.y;
        const float dz = point_m.z - center.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared > query_radius_squared) continue;
        const float distance =
            std::sqrt(distance_squared) - particles[index].radius_m;
        minimum = std::min(minimum, distance);
        ++neighbors;
    }
    if (neighbors == 0) return std::numeric_limits<float>::infinity();
    if (blend_width_m <= 1e-5f || neighbors == 1) return minimum;

    float sum = 0.0f;
    for (std::uint32_t index = 0; index != particle_count; ++index) {
        const matter::Float3 center = particles[index].position_m;
        const float dx = point_m.x - center.x;
        const float dy = point_m.y - center.y;
        const float dz = point_m.z - center.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared > query_radius_squared) continue;
        const float distance =
            std::sqrt(distance_squared) - particles[index].radius_m;
        sum += std::exp(-(distance - minimum) / blend_width_m);
    }
    return minimum - blend_width_m * std::log(sum);
}

float evaluate_particle_field_reference(
    const ParticleSample* particles,
    std::uint32_t particle_count,
    float blend_width_m,
    ParticlePhaseBlend phase_blend,
    matter::Float3 point_m) {
    if (particles == nullptr || particle_count == 0)
        return std::numeric_limits<float>::infinity();
    const std::uint32_t split = phase_blend.split_index == 0u &&
            phase_blend.primary_weight == 1.0f &&
            phase_blend.secondary_weight == 0.0f
        ? particle_count
        : phase_blend.split_index;
    const auto particle_weight = [&](std::uint32_t index) noexcept {
        return index < split ? phase_blend.primary_weight
                             : phase_blend.secondary_weight;
    };

    float max_radius = 0.0f;
    for (std::uint32_t index = 0; index != particle_count; ++index) {
        if (particle_weight(index) == 0.0f) continue;
        max_radius = std::max(max_radius, particles[index].radius_m);
    }
    const float query_radius = max_radius * 2.5f + blend_width_m * 4.0f;
    const float query_radius_squared = query_radius * query_radius;

    float minimum = std::numeric_limits<float>::infinity();
    std::uint32_t neighbors = 0;
    float only_weight = 0.0f;
    for (std::uint32_t index = 0; index != particle_count; ++index) {
        const float weight = particle_weight(index);
        if (weight == 0.0f) continue;
        const matter::Float3 center = particles[index].position_m;
        const float dx = point_m.x - center.x;
        const float dy = point_m.y - center.y;
        const float dz = point_m.z - center.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared > query_radius_squared) continue;
        const float distance =
            std::sqrt(distance_squared) - particles[index].radius_m;
        minimum = std::min(minimum, distance);
        only_weight = weight;
        ++neighbors;
    }
    if (neighbors == 0) return std::numeric_limits<float>::infinity();
    if (blend_width_m <= 1e-5f ||
        (neighbors == 1u && only_weight == 1.0f))
        return minimum;

    float sum = 0.0f;
    for (std::uint32_t index = 0; index != particle_count; ++index) {
        const float weight = particle_weight(index);
        if (weight == 0.0f) continue;
        const matter::Float3 center = particles[index].position_m;
        const float dx = point_m.x - center.x;
        const float dy = point_m.y - center.y;
        const float dz = point_m.z - center.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared > query_radius_squared) continue;
        const float distance =
            std::sqrt(distance_squared) - particles[index].radius_m;
        sum += weight *
            std::exp(-(distance - minimum) / blend_width_m);
    }
    if (sum <= 0.0f) return std::numeric_limits<float>::infinity();
    return minimum - blend_width_m * std::log(sum);
}

bool exclusive_scan_reference(const std::vector<std::uint32_t>& input,
                              std::vector<std::uint32_t>& output,
                              std::uint32_t& total) {
    output.clear();
    total = 0;
    std::vector<std::uint32_t> candidate;
    candidate.reserve(input.size());
    std::uint64_t running = 0;
    for (const std::uint32_t value : input) {
        candidate.push_back(static_cast<std::uint32_t>(running));
        running += value;
        if (running > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    total = static_cast<std::uint32_t>(running);
    output = std::move(candidate);
    return true;
}

std::uint64_t mesh_content_digest(const MeshResult& mesh) {
    Digest64 digest;
    digest.u32(1u);
    digest.floats(mesh.positions);
    digest.floats(mesh.normals);
    digest.uints(mesh.indices);
    digest.u32(mesh.material);
    return digest.finish();
}

}  // namespace gpu_meshing
