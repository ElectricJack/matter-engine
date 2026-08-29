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

float coordinate(matter::Float3 value, std::size_t axis) noexcept {
    return axis == 0u ? value.x : axis == 1u ? value.y : value.z;
}

void set_coordinate(matter::Float3& value, std::size_t axis,
                    float coordinate_value) noexcept {
    if (axis == 0u) value.x = coordinate_value;
    else if (axis == 1u) value.y = coordinate_value;
    else value.z = coordinate_value;
}

bool same_float_bits(float left, float right) noexcept {
    return std::memcmp(&left, &right, sizeof(float)) == 0;
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

float lattice_face_coordinate(float anchor_m, float voxel_m,
                              std::int64_t cell_index) noexcept {
    return anchor_m + voxel_m * static_cast<float>(cell_index);
}

bool lattice_cell_index(float coordinate_m, float anchor_m, float voxel_m,
                        bool upper, std::int64_t& index, Error& error) {
    constexpr double kInt64Minimum = -0x1p63;
    constexpr double kInt64Limit = 0x1p63;
    const double relative =
        (static_cast<double>(coordinate_m) -
         static_cast<double>(anchor_m)) /
        static_cast<double>(voxel_m);
    if (!std::isfinite(relative))
        return fail(error, ErrorCode::Overflow,
                    "particle lattice cell index overflows int64");

    // A face emitted by this lattice is a float value. Dividing that value
    // back by the exact stored float voxel may land just to either side of
    // its integer (0.75f / 0.15f is below 5). Recognize only a bit-identical
    // reconstruction of the nearest integer face. This is bounded by one
    // representable value: the adjacent floats remain genuinely outside and
    // continue through outward floor/ceil below.
    const double nearest = std::round(relative);
    if (std::isfinite(nearest) && nearest >= kInt64Minimum &&
        nearest < kInt64Limit) {
        const auto candidate = static_cast<std::int64_t>(nearest);
        if (same_float_bits(
                coordinate_m,
                lattice_face_coordinate(anchor_m, voxel_m, candidate))) {
            index = candidate;
            return true;
        }
    }

    const double rounded = upper ? std::ceil(relative) : std::floor(relative);
    if (!std::isfinite(rounded) || rounded < kInt64Minimum ||
        rounded >= kInt64Limit) {
        return fail(error, ErrorCode::Overflow,
                    "particle lattice cell index overflows int64");
    }
    index = static_cast<std::int64_t>(rounded);
    return true;
}

bool lattice_cell_span(std::int64_t minimum, std::int64_t maximum,
                       std::uint32_t& cells, Error& error) {
    constexpr std::int64_t kMaximumCells =
        static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max() - 1u);
    if (maximum <= minimum)
        return fail(error, ErrorCode::InvalidInput,
                    "particle lattice bounds do not contain a cell");
    if (minimum <= std::numeric_limits<std::int64_t>::max() - kMaximumCells &&
        maximum > minimum + kMaximumCells) {
        return fail(error, ErrorCode::Overflow,
                    "particle lattice dimensions overflow uint32");
    }
    const std::int64_t span = maximum - minimum;
    if (span <= 0 || span > kMaximumCells)
        return fail(error, ErrorCode::Overflow,
                    "particle lattice dimensions overflow uint32");
    cells = static_cast<std::uint32_t>(span);
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

bool make_particle_sampling_lattice(
    const matter::Float3& world_anchor_m, float voxel_m,
    ParticleSamplingLattice& lattice, Error& error) {
    lattice = {};
    error = {};
    if (!finite(world_anchor_m))
        return fail(error, ErrorCode::InvalidInput,
                    "particle sampling lattice anchor must be finite");
    if (!finite(voxel_m) || voxel_m <= 0.0f)
        return fail(error, ErrorCode::InvalidInput,
                    "particle sampling lattice voxel must be positive and finite");
    lattice = {world_anchor_m, voxel_m, 1u};
    return true;
}

bool particle_field_support_radius_m(
    float particle_radius_m, float blend_width_m,
    float& support_radius_m, Error& error) {
    support_radius_m = 0.0f;
    error = {};
    if (!finite(particle_radius_m) || particle_radius_m <= 0.0f ||
        !finite(blend_width_m) || blend_width_m < 0.0f) {
        return fail(error, ErrorCode::InvalidInput,
                    "particle field support requires a positive radius and nonnegative blend width");
    }
    const double support = static_cast<double>(particle_radius_m) * 2.5 +
                           static_cast<double>(blend_width_m) * 4.0;
    if (!std::isfinite(support) || support <= 0.0 ||
        support > static_cast<double>(std::numeric_limits<float>::max())) {
        return fail(error, ErrorCode::Overflow,
                    "particle field support radius overflow");
    }
    support_radius_m = static_cast<float>(support);
    return true;
}

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
    if (job.sampling_lattice.version == 0u) {
        if (!dimension_for_extent(extent.x, job.voxel_m, cells[0], error) ||
            !dimension_for_extent(extent.y, job.voxel_m, cells[1], error) ||
            !dimension_for_extent(extent.z, job.voxel_m, cells[2], error)) {
            return false;
        }
        layout.origin_m = job.bounds_m.min_m;
        layout.spacing_m = {extent.x / static_cast<float>(cells[0]),
                            extent.y / static_cast<float>(cells[1]),
                            extent.z / static_cast<float>(cells[2])};
    } else if (job.sampling_lattice.version == 1u) {
        const ParticleSamplingLattice& lattice = job.sampling_lattice;
        if (!finite(lattice.origin_m) || !finite(lattice.voxel_m) ||
            lattice.voxel_m <= 0.0f ||
            !same_float_bits(job.voxel_m, lattice.voxel_m)) {
            return fail(error, ErrorCode::InvalidInput,
                        "canonical particle job and lattice metadata conflict");
        }
        for (std::size_t axis = 0u; axis != 3u; ++axis) {
            std::int64_t upper = 0;
            if (!lattice_cell_index(
                    coordinate(job.bounds_m.min_m, axis),
                    coordinate(lattice.origin_m, axis),
                    lattice.voxel_m, false,
                    layout.cell_min[axis], error) ||
                !lattice_cell_index(
                    coordinate(job.bounds_m.max_m, axis),
                    coordinate(lattice.origin_m, axis),
                    lattice.voxel_m, true, upper, error) ||
                !lattice_cell_span(layout.cell_min[axis], upper,
                                   cells[axis], error)) {
                return false;
            }
            const double snapped =
                static_cast<double>(coordinate(lattice.origin_m, axis)) +
                static_cast<double>(layout.cell_min[axis]) *
                    static_cast<double>(lattice.voxel_m);
            if (!std::isfinite(snapped) ||
                snapped < -static_cast<double>(
                              std::numeric_limits<float>::max()) ||
                snapped > static_cast<double>(
                              std::numeric_limits<float>::max())) {
                return fail(error, ErrorCode::Overflow,
                            "particle lattice origin overflows float");
            }
            const float snapped_face = lattice_face_coordinate(
                coordinate(lattice.origin_m, axis), lattice.voxel_m,
                layout.cell_min[axis]);
            if (!finite(snapped_face))
                return fail(error, ErrorCode::Overflow,
                            "particle lattice origin overflows float");
            set_coordinate(layout.origin_m, axis, snapped_face);
        }
        layout.spacing_m = {
            lattice.voxel_m, lattice.voxel_m, lattice.voxel_m};
    } else {
        return fail(error, ErrorCode::InvalidInput,
                    "particle sampling lattice version is unsupported");
    }
    layout.cell_dims = cells;
    for (std::size_t axis = 0; axis != 3; ++axis)
        layout.sample_dims[axis] = cells[axis] + 1u;
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

    if (!particle_field_support_radius_m(
            max_radius, job.blend_width_m, layout.query_radius_m, error))
        return false;
    layout.bin_size_m = layout.query_radius_m;
    layout.bin_origin_m = {
        layout.origin_m.x - layout.query_radius_m,
        layout.origin_m.y - layout.query_radius_m,
        layout.origin_m.z - layout.query_radius_m,
    };
    if (!finite(layout.bin_origin_m))
        return fail(error, ErrorCode::Overflow,
                    "particle bin origin overflow");

    const double expanded[] = {
        static_cast<double>(layout.spacing_m.x) * layout.cell_dims[0] +
            2.0 * layout.query_radius_m,
        static_cast<double>(layout.spacing_m.y) * layout.cell_dims[1] +
            2.0 * layout.query_radius_m,
        static_cast<double>(layout.spacing_m.z) * layout.cell_dims[2] +
            2.0 * layout.query_radius_m,
    };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        const double count = std::ceil(
            expanded[axis] / layout.bin_size_m);
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
    float query_radius = 0.0f;
    Error support_error{};
    if (!particle_field_support_radius_m(
            max_radius, blend_width_m, query_radius, support_error))
        return std::numeric_limits<float>::infinity();
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
    float query_radius = 0.0f;
    Error support_error{};
    if (!particle_field_support_radius_m(
            max_radius, blend_width_m, query_radius, support_error))
        return std::numeric_limits<float>::infinity();
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
