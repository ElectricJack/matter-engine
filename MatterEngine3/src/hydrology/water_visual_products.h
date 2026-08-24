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
    std::uint64_t presentation = 0;
};

inline bool operator==(const ProductKeys& a, const ProductKeys& b) noexcept {
    return a.visual == b.visual && a.coarse_cpu == b.coarse_cpu &&
           a.gameplay == b.gameplay && a.presentation == b.presentation;
}

// Hydrology artifact v4 serializes only visual/coarse_cpu/gameplay. This
// compatibility predicate is intentionally temporary; Task 2 removes it once
// v5 persists ProductKeys::presentation.
inline bool v4_persisted_product_keys_match(const ProductKeys& persisted,
                                            const ProductKeys& expected) noexcept {
    return persisted.visual == expected.visual &&
           persisted.coarse_cpu == expected.coarse_cpu &&
           persisted.gameplay == expected.gameplay;
}

// Provider-neutral authored values are resolved onto the presentation lattice.
// The native/QuickJS authoring producer is intentionally a later concern.
struct PresentationMarkers {
    bool waterfall = false;
    bool impact = false;
    bool spillway = false;
    bool pool = false;
};

struct PresentationLocalOverride {
    float turbulence_multiplier = 1.0f;
    float aeration_multiplier = 1.0f;
    float foam_multiplier = 1.0f;
};

enum class RiverFeature : std::uint8_t {
    Calm = 0, Current = 1, Rapid = 2, Waterfall = 3,
    Impact = 4, Spillway = 5, Pool = 6
};

struct PresentationSample {
    float normal_x = 0.0f;
    float normal_z = 0.0f;
    float turbulence = 0.0f;
    float aeration = 0.0f;
    float foam_potential = 0.0f;
    RiverFeature feature = RiverFeature::Calm;
    bool wet_valid = false;
};

inline bool operator==(const PresentationSample& a,
                       const PresentationSample& b) noexcept {
    return a.normal_x == b.normal_x && a.normal_z == b.normal_z &&
           a.turbulence == b.turbulence && a.aeration == b.aeration &&
           a.foam_potential == b.foam_potential && a.feature == b.feature &&
           a.wet_valid == b.wet_valid;
}

struct PresentationDerivationSettings {
    std::uint32_t contract_version = 1;
    float velocity_variance_weight = 1.0f;
    float divergence_weight = 1.0f;
    float vorticity_weight = 1.0f;
    float vertical_speed_weight = 1.0f;
    float surface_slope_weight = 1.0f;
    float shallows_weight = 1.0f;
    float wake_distance_weight = 1.0f;
    float waterfall_weight = 1.0f;
    float impact_weight = 1.0f;
    float spillway_weight = 1.0f;
    float velocity_variance_scale_mps2 = 1.0f;
    float divergence_scale_per_m = 1.0f;
    float vorticity_scale_per_m = 1.0f;
    float vertical_speed_scale_mps = 1.0f;
    float surface_slope_scale = 1.0f;
    float shallow_depth_m = 0.5f;
    float wake_distance_scale_m = 1.0f;
    float current_speed_mps = 0.25f;
    float rapid_speed_mps = 1.5f;
};

struct GameplaySample {
    float height_m = 0.0f;
    float depth_m = 0.0f;
    float velocity_x_mps = 0.0f;
    float velocity_y_mps = 0.0f;
    float velocity_z_mps = 0.0f;
    bool wet_valid = false;
};

// This is deliberately the single layout type used both by the field builder
// and by the gameplay product key.  Keeping it here prevents a second,
// cache-only approximation of the field parameters from drifting away from
// the values that actually determine gameplay samples.
struct GameplayFieldLayout {
    matter::Float3 origin_m{};
    float cell_size_m = 0.0f;
    std::uint32_t width = 0;
    std::uint32_t depth = 0;
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
    std::uint32_t field_contract_version = 1;
    std::uint32_t extraction_contract_version = 1;
    std::uint32_t output_contract_version = 1;
    std::uint32_t normal_contract_version = 1;
    bool smoothing_enabled = false;
    bool anisotropy_enabled = false;
    std::vector<std::uint64_t> shader_digests;
    std::uint64_t semantic_key = 0;
    PresentationDerivationSettings presentation{};
    std::vector<PresentationLocalOverride> presentation_local_overrides;
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
    const ProductIdentitySettings& settings, float coarse_voxel_m,
    const GameplayFieldLayout& gameplay_layout);

// Repairs only missing or non-finite CPU mesh normals from valid triangle
// geometry. Invalid positions or triangle indices remain hard failures.
bool repair_nonfinite_cpu_mesh_normals(
    gpu_meshing::MeshResult& mesh) noexcept;

bool build_cpu_particle_visual(const gpu_meshing::ParticleJob& job,
                               float coarse_voxel_m,
                               gpu_meshing::MeshResult& result,
                               gpu_meshing::Error& error);

}  // namespace hydrology
