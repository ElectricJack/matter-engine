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

}  // namespace

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
    return {visual.finish(), coarse.finish(), gameplay.finish()};
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
    SurfaceScratch* scratch = CreateSurfaceScratch();
    if (scratch == nullptr)
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    "failed to create MatterSurface CPU scratch");
    Mesh mesh = GenerateMeshWithScratch(
        scratch, particles.data(), max_radius,
        static_cast<int>(particles.size()), bounds, job.blend_width_m, nullptr,
        0, nullptr, 0, 0.0f);
    if (mesh.vertexCount > 0)
        ComputeSurfaceNormalsWithScratch(
            scratch, &mesh, particles.data(), max_radius,
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
    if (!finite_mesh(candidate))
        return fail(error, gpu_meshing::ErrorCode::Unavailable,
                    "MatterSurface CPU fallback emitted invalid geometry");
    candidate.content_digest = gpu_meshing::mesh_content_digest(candidate);
    result = std::move(candidate);
    return true;
}

}  // namespace hydrology
