#pragma once

#include "matter/gpu_visual_meshing.h"
#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <vector>

namespace hydrology {

struct ProductKeys {
    std::uint64_t visual = 0;
    std::uint64_t coarse_cpu = 0;
    std::uint64_t gameplay = 0;
};

inline bool operator==(const ProductKeys& a, const ProductKeys& b) noexcept {
    return a.visual == b.visual && a.coarse_cpu == b.coarse_cpu &&
           a.gameplay == b.gameplay;
}

struct GameplaySample {
    float height_m = 0.0f;
    float depth_m = 0.0f;
    float velocity_x_mps = 0.0f;
    float velocity_y_mps = 0.0f;
    float velocity_z_mps = 0.0f;
    bool wet_valid = false;
};

inline bool operator==(const GameplaySample& a,
                       const GameplaySample& b) noexcept {
    return a.height_m == b.height_m && a.depth_m == b.depth_m &&
           a.velocity_x_mps == b.velocity_x_mps &&
           a.velocity_y_mps == b.velocity_y_mps &&
           a.velocity_z_mps == b.velocity_z_mps &&
           a.wet_valid == b.wet_valid;
}

struct ProductIdentitySettings {
    float coarse_voxel_m = 0.5f;
    std::uint32_t field_contract_version = 1;
    std::uint32_t extraction_contract_version = 1;
    std::uint32_t output_contract_version = 1;
    std::uint32_t normal_contract_version = 1;
    bool smoothing_enabled = false;
    bool anisotropy_enabled = false;
    std::vector<std::uint64_t> shader_digests;
    float gameplay_cell_m = 0.5f;
    std::uint64_t terrain_revision = 0;
    std::uint64_t semantic_key = 0;
};

// Versions and canonical revisions that define a hydrology simulation, as
// opposed to a renderer-only appearance choice.
struct HydrologySemanticInputs {
    std::uint64_t physx_sdk_version = 0;
    std::uint64_t adapter_version = 0;
    std::uint64_t pbd_settings_version = 0;
    std::uint64_t collision_revision = 0;
    std::uint64_t network_hash = 0;
    std::uint64_t terrain_revision = 0;
    std::uint64_t virtual_dam_revision = 0;
    std::uint64_t sensor_revision = 0;
    std::uint64_t mesher_contract_version = 0;
};

std::uint64_t particle_snapshot_digest(
    const gpu_meshing::ParticleSample* particles, std::uint32_t count);

std::uint64_t fluid_particle_snapshot_digest(
    const std::vector<FluidParticle>& particles, float radius_m);

std::uint64_t derive_hydrology_semantic_key(
    const HydrologySemanticInputs& inputs);

bool make_fluid_particle_job(
    const std::vector<FluidParticle>& particles, float radius_m,
    const gpu_meshing::ParticleJob& template_job,
    std::vector<gpu_meshing::ParticleSample>& owned_particles,
    gpu_meshing::ParticleJob& job, gpu_meshing::Error& error);

ProductKeys derive_product_keys(
    const gpu_meshing::ParticleJob& job,
    std::uint64_t particle_snapshot_digest,
    const ProductIdentitySettings& settings);

bool build_cpu_particle_visual(const gpu_meshing::ParticleJob& job,
                               float coarse_voxel_m,
                               gpu_meshing::MeshResult& result,
                               gpu_meshing::Error& error);

}  // namespace hydrology
