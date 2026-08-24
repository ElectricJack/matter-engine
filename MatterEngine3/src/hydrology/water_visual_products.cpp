#include "hydrology/water_visual_products.h"

#include "surface.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace hydrology {
namespace {

class Digest {
public:
    explicit Digest(std::uint64_t domain) { u64(domain); }
    void byte(std::uint8_t value) {
        value_ ^= value;
        value_ *= 1099511628211ull;
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void floating(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void point(matter::Float3 point) {
        floating(point.x);
        floating(point.y);
        floating(point.z);
    }
    std::uint64_t finish() const {
        return value_ == 0 ? 1u : value_;
    }

private:
    std::uint64_t value_ = 1469598103934665603ull;
};

bool fail(gpu_meshing::Error& error, gpu_meshing::ErrorCode code,
          const char* message) {
    error.code = code;
    error.message = message;
    return false;
}

void free_surface_mesh(Mesh& mesh) {
    std::free(mesh.vertices);
    std::free(mesh.normals);
    std::free(mesh.indices);
    std::free(mesh.colors);
    mesh = {};
}

bool finite_mesh(const gpu_meshing::MeshResult& mesh) {
    if (mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u || mesh.indices.size() % 3u != 0u)
        return false;
    for (float value : mesh.positions)
        if (!std::isfinite(value)) return false;
    for (float value : mesh.normals)
        if (!std::isfinite(value)) return false;
    const std::size_t vertices = mesh.positions.size() / 3u;
    for (std::uint32_t index : mesh.indices)
        if (index >= vertices) return false;
    return true;
}

bool validate_cpu_mesh_positions_topology(
    const gpu_meshing::MeshResult& mesh, std::string* reason = nullptr) {
    const auto reject = [reason](std::string message) {
        if (reason != nullptr) *reason = std::move(message);
        return false;
    };
    if (mesh.positions.size() % 3u != 0u)
        return reject("position stream is not float3-aligned");
    if (mesh.indices.size() % 3u != 0u)
        return reject("index stream is not triangle-aligned");
    for (std::size_t component = 0; component < mesh.positions.size();
         ++component) {
        if (!std::isfinite(mesh.positions[component]))
            return reject("non-finite position component " +
                          std::to_string(component) + " of " +
                          std::to_string(mesh.positions.size()));
    }
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    for (std::size_t offset = 0; offset < mesh.indices.size(); ++offset) {
        if (mesh.indices[offset] >= vertex_count)
            return reject("out-of-range index " +
                          std::to_string(mesh.indices[offset]) + " at " +
                          std::to_string(offset) + " for " +
                          std::to_string(vertex_count) + " vertices");
    }
    return true;
}

}  // namespace

bool repair_nonfinite_cpu_mesh_normals(
    gpu_meshing::MeshResult& mesh) noexcept {
    if (!validate_cpu_mesh_positions_topology(mesh)) return false;
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    const bool normals_need_repair =
        mesh.normals.size() != mesh.positions.size() ||
        std::any_of(mesh.normals.begin(), mesh.normals.end(),
                    [](const float component) {
                        return !std::isfinite(component);
                    });
    if (!normals_need_repair) return true;

    mesh.normals.assign(mesh.positions.size(), 0.0f);
    for (std::size_t triangle = 0; triangle < mesh.indices.size();
         triangle += 3u) {
        const std::uint32_t ia = mesh.indices[triangle + 0u];
        const std::uint32_t ib = mesh.indices[triangle + 1u];
        const std::uint32_t ic = mesh.indices[triangle + 2u];

        const float ax = mesh.positions[ia * 3u + 0u];
        const float ay = mesh.positions[ia * 3u + 1u];
        const float az = mesh.positions[ia * 3u + 2u];
        const float abx = mesh.positions[ib * 3u + 0u] - ax;
        const float aby = mesh.positions[ib * 3u + 1u] - ay;
        const float abz = mesh.positions[ib * 3u + 2u] - az;
        const float acx = mesh.positions[ic * 3u + 0u] - ax;
        const float acy = mesh.positions[ic * 3u + 1u] - ay;
        const float acz = mesh.positions[ic * 3u + 2u] - az;
        const float nx = aby * acz - abz * acy;
        const float ny = abz * acx - abx * acz;
        const float nz = abx * acy - aby * acx;

        const std::uint32_t triangle_indices[] = {ia, ib, ic};
        for (const std::uint32_t index : triangle_indices) {
            mesh.normals[index * 3u + 0u] += nx;
            mesh.normals[index * 3u + 1u] += ny;
            mesh.normals[index * 3u + 2u] += nz;
        }
    }

    constexpr float kNormalLengthSquaredEpsilon = 1.0e-20f;
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        float& nx = mesh.normals[vertex * 3u + 0u];
        float& ny = mesh.normals[vertex * 3u + 1u];
        float& nz = mesh.normals[vertex * 3u + 2u];
        const float length_squared = nx * nx + ny * ny + nz * nz;
        if (std::isfinite(length_squared) &&
            length_squared > kNormalLengthSquaredEpsilon) {
            const float inverse_length = 1.0f / std::sqrt(length_squared);
            nx *= inverse_length;
            ny *= inverse_length;
            nz *= inverse_length;
        } else {
            nx = 0.0f;
            ny = 1.0f;
            nz = 0.0f;
        }
    }
    return true;
}

std::uint64_t particle_snapshot_digest(
    const gpu_meshing::ParticleSample* particles, std::uint32_t count) {
    Digest digest(0x5041525449434c45ull);
    digest.u32(count);
    if (particles != nullptr) {
        for (std::uint32_t i = 0; i != count; ++i) {
            digest.point(particles[i].position_m);
            digest.floating(particles[i].radius_m);
        }
    }
    return digest.finish();
}

std::uint64_t fluid_particle_snapshot_digest(
    const std::vector<FluidParticle>& particles, float radius_m) {
    Digest digest(0x46504f53534e4150ull);
    digest.floating(radius_m);
    digest.u64(particles.size());
    for (const FluidParticle& particle : particles) {
        digest.u64(particle.id);
        digest.point(particle.position_m);
        digest.point(particle.velocity_mps);
    }
    return digest.finish();
}

std::uint64_t derive_hydrology_semantic_key(
    const HydrologySemanticInputs& inputs) {
    Digest digest(0x485944524f4b4559ull);
    digest.u64(inputs.physx_sdk_version);
    digest.u64(inputs.adapter_version);
    digest.u64(inputs.pbd_settings_version);
    digest.u64(inputs.collision_revision);
    digest.u64(inputs.network_hash);
    digest.u64(inputs.terrain_revision);
    digest.u64(inputs.virtual_dam_revision);
    digest.u64(inputs.sensor_revision);
    digest.u64(inputs.mesher_contract_version);
    return digest.finish();
}

bool make_fluid_particle_job(
    const std::vector<FluidParticle>& particles, float radius_m,
    const gpu_meshing::ParticleJob& template_job,
    std::vector<gpu_meshing::ParticleSample>& owned_particles,
    gpu_meshing::ParticleJob& job, gpu_meshing::Error& error) {
    owned_particles.clear();
    job = {};
    error = {};
    if (!std::isfinite(radius_m) || radius_m <= 0.0f)
        return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                    "fluid particle render radius must be positive and finite");
    if (particles.size() > std::numeric_limits<std::uint32_t>::max())
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "fluid particle snapshot exceeds the visual particle limit");
    owned_particles.reserve(particles.size());
    for (const FluidParticle& particle : particles) {
        if (!std::isfinite(particle.position_m.x) ||
            !std::isfinite(particle.position_m.y) ||
            !std::isfinite(particle.position_m.z) ||
            !std::isfinite(particle.velocity_mps.x) ||
            !std::isfinite(particle.velocity_mps.y) ||
            !std::isfinite(particle.velocity_mps.z))
            return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                        "fluid particle snapshot contains non-finite values");
        owned_particles.push_back({particle.position_m, radius_m});
    }
    job = template_job;
    job.particles = owned_particles.empty() ? nullptr : owned_particles.data();
    job.particle_count = static_cast<std::uint32_t>(owned_particles.size());
    job.material = 4u;
    gpu_meshing::GridLayout ignored{};
    return gpu_meshing::validate_particle_job(job, ignored, error);
}

ProductKeys derive_product_keys(
    const gpu_meshing::ParticleJob& job, std::uint64_t snapshot,
    const ProductIdentitySettings& settings, float coarse_voxel_m,
    const GameplayFieldLayout& gameplay_layout) {
    const auto simulation_common = [&](Digest& digest) {
        digest.u64(snapshot);
        digest.u64(settings.semantic_key);
        digest.u32(job.material);
    };
    Digest visual(0x56495355414c3031ull);
    simulation_common(visual);
    visual.point(job.bounds_m.min_m);
    visual.point(job.bounds_m.max_m);
    visual.floating(job.voxel_m);
    visual.floating(job.blend_width_m);
    visual.floating(job.iso_value);
    visual.u32(settings.field_contract_version);
    visual.u32(settings.extraction_contract_version);
    visual.u32(settings.output_contract_version);
    visual.u32(settings.normal_contract_version);
    visual.u32(settings.smoothing_enabled ? 1u : 0u);
    visual.u32(settings.anisotropy_enabled ? 1u : 0u);
    visual.u64(settings.shader_digests.size());
    for (std::uint64_t shader : settings.shader_digests) visual.u64(shader);

    Digest coarse(0x434f415253453031ull);
    simulation_common(coarse);
    coarse.point(job.bounds_m.min_m);
    coarse.point(job.bounds_m.max_m);
    coarse.floating(job.blend_width_m);
    coarse.floating(coarse_voxel_m);

    Digest gameplay(0x47414d45504c4159ull);
    simulation_common(gameplay);
    gameplay.point(gameplay_layout.origin_m);
    gameplay.floating(gameplay_layout.cell_size_m);
    gameplay.u32(gameplay_layout.width);
    gameplay.u32(gameplay_layout.depth);
    gameplay.u32(settings.field_contract_version);

    const PresentationDerivationSettings& presentation_settings =
        settings.presentation;
    Digest presentation(0x50524553454e5431ull);
    presentation.u64(snapshot);
    presentation.u64(settings.semantic_key);
    presentation.point(gameplay_layout.origin_m);
    presentation.floating(gameplay_layout.cell_size_m);
    presentation.u32(gameplay_layout.width);
    presentation.u32(gameplay_layout.depth);
    presentation.u32(presentation_settings.contract_version);
    presentation.floating(presentation_settings.velocity_variance_weight);
    presentation.floating(presentation_settings.divergence_weight);
    presentation.floating(presentation_settings.vorticity_weight);
    presentation.floating(presentation_settings.vertical_speed_weight);
    presentation.floating(presentation_settings.surface_slope_weight);
    presentation.floating(presentation_settings.shallows_weight);
    presentation.floating(presentation_settings.wake_distance_weight);
    presentation.floating(presentation_settings.waterfall_weight);
    presentation.floating(presentation_settings.impact_weight);
    presentation.floating(presentation_settings.spillway_weight);
    presentation.floating(presentation_settings.pool_weight);
    presentation.floating(presentation_settings.velocity_variance_scale_mps2);
    presentation.floating(presentation_settings.divergence_scale_per_m);
    presentation.floating(presentation_settings.vorticity_scale_per_m);
    presentation.floating(presentation_settings.vertical_speed_scale_mps);
    presentation.floating(presentation_settings.surface_slope_scale);
    presentation.floating(presentation_settings.shallow_depth_m);
    presentation.floating(presentation_settings.wake_distance_scale_m);
    presentation.floating(presentation_settings.current_speed_mps);
    presentation.floating(presentation_settings.rapid_speed_mps);
    presentation.u64(settings.presentation_local_overrides.size());
    for (const PresentationLocalOverride& local_override :
         settings.presentation_local_overrides) {
        presentation.floating(local_override.turbulence_multiplier);
        presentation.floating(local_override.aeration_multiplier);
        presentation.floating(local_override.foam_multiplier);
    }
    return {visual.finish(), coarse.finish(), gameplay.finish(),
            presentation.finish()};
}

bool build_cpu_particle_visual(const gpu_meshing::ParticleJob& job,
                               float coarse_voxel_m,
                               gpu_meshing::MeshResult& result,
                               gpu_meshing::Error& error) {
    result = {};
    error = {};
    gpu_meshing::GridLayout ignored{};
    if (!gpu_meshing::validate_particle_job(job, ignored, error)) return false;
    if (!std::isfinite(coarse_voxel_m) || coarse_voxel_m <= 0.0f)
        return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                    "coarse CPU voxel size must be positive and finite");
    if (job.particle_count == 0u) {
        result.material = job.material;
        result.content_digest = gpu_meshing::mesh_content_digest(result);
        return true;
    }

    std::vector<Particle> particles(job.particle_count);
    float max_radius = 0.0f;
    for (std::uint32_t i = 0; i != job.particle_count; ++i) {
        const auto& input = job.particles[i];
        particles[i] = {{input.position_m.x, input.position_m.y,
                         input.position_m.z},
                        input.radius_m, static_cast<int>(job.material)};
        max_radius = std::max(max_radius, input.radius_m);
    }
    const matter::Float3 extent{
        job.bounds_m.max_m.x - job.bounds_m.min_m.x,
        job.bounds_m.max_m.y - job.bounds_m.min_m.y,
        job.bounds_m.max_m.z - job.bounds_m.min_m.z};
    const float side = std::max({extent.x, extent.y, extent.z});
    int division_power = 1;
    while (division_power < 8) {
        const int next_samples = 1 << (division_power + 1);
        const float next_cell = side / static_cast<float>(next_samples - 1);
        if (next_cell < coarse_voxel_m) break;
        ++division_power;
    }
    const Bounds bounds{
        {(job.bounds_m.min_m.x + job.bounds_m.max_m.x) * 0.5f,
         (job.bounds_m.min_m.y + job.bounds_m.max_m.y) * 0.5f,
         (job.bounds_m.min_m.z + job.bounds_m.max_m.z) * 0.5f},
        {side, side, side}, division_power};
    const float coarse_cell_m =
        side / static_cast<float>((1 << division_power) - 1);
    // MatterSurface uses its reference radius both to bound field queries and
    // to apply a coarse-LOD isovalue offset. Passing the fine particle radius
    // to a much coarser lattice shrinks the implicit surface far enough to
    // erase dense water completely. A conservative reference radius at least
    // as large as the actual lattice cell disables that shrink without
    // changing any particle's authored SDF radius.
    const float field_reference_radius = std::max(max_radius, coarse_cell_m);
    SurfaceScratch* scratch = CreateSurfaceScratch();
    if (scratch == nullptr)
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    "failed to create MatterSurface CPU scratch");
    Mesh mesh = GenerateMeshWithScratch(
        scratch, particles.data(), field_reference_radius,
        static_cast<int>(particles.size()), bounds, job.blend_width_m, nullptr,
        0, nullptr, 0, 0.0f);
    if (mesh.vertexCount > 0)
        ComputeSurfaceNormalsWithScratch(
            scratch, &mesh, particles.data(), field_reference_radius,
            static_cast<int>(particles.size()), job.blend_width_m, nullptr, 0,
            nullptr, 0, 0.0f);

    gpu_meshing::MeshResult candidate{};
    candidate.material = job.material;
    if (mesh.vertexCount < 0 || mesh.triangleCount < 0 ||
        (mesh.vertexCount != 0 &&
         (mesh.vertices == nullptr || mesh.normals == nullptr)) ||
        (mesh.triangleCount != 0 && mesh.indices == nullptr)) {
        free_surface_mesh(mesh);
        DestroySurfaceScratch(scratch);
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    "MatterSurface CPU fallback returned an invalid mesh");
    }
    if (mesh.vertexCount > 0) {
        candidate.positions.assign(mesh.vertices,
                                   mesh.vertices + mesh.vertexCount * 3u);
        candidate.normals.assign(mesh.normals,
                                 mesh.normals + mesh.vertexCount * 3u);
    }
    candidate.indices.reserve(mesh.triangleCount * 3u);
    for (int i = 0; i != mesh.triangleCount * 3; ++i)
        candidate.indices.push_back(mesh.indices[i]);
    free_surface_mesh(mesh);
    DestroySurfaceScratch(scratch);
    std::string invalid_geometry_reason;
    if (!validate_cpu_mesh_positions_topology(candidate,
                                              &invalid_geometry_reason))
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    ("MatterSurface CPU fallback emitted invalid positions or topology: " +
                     invalid_geometry_reason)
                        .c_str());
    if (!repair_nonfinite_cpu_mesh_normals(candidate))
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    "MatterSurface CPU fallback emitted invalid positions or topology");
    if (!finite_mesh(candidate))
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    "MatterSurface CPU fallback emitted invalid geometry");
    candidate.content_digest = gpu_meshing::mesh_content_digest(candidate);
    result = std::move(candidate);
    return true;
}

}  // namespace hydrology
