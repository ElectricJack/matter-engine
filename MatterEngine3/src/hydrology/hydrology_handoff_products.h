#pragma once

#include "hydrology/hydrology_artifact.h"
#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/physx_fluid_bake.h"
#include "hydrology/spillway_handoff.h"

#include <cstdint>
#include <filesystem>

namespace hydrology {

struct HydrologyHandoffArtifact {
    std::string id;
    SpillwayHandoffRecord handoff{};
    std::uint64_t semantic_key = 0;
    std::uint64_t upstream_payload_digest = 0;
    std::uint64_t downstream_payload_digest = 0;
    gpu_meshing::MeshResult visual_mesh;
    std::uint64_t payload_digest = 0;
};

struct HydrologyNetworkProducts {
    gpu_meshing::MeshResult visual_mesh;
    gpu_meshing::MeshResult coarse_cpu_mesh;
    GameplayFieldLayout gameplay_layout{};
    std::vector<GameplaySample> gameplay_field;
    std::vector<PresentationSample> presentation_field;
};

std::uint64_t hydrology_runtime_field_digest(
    const GameplayFieldLayout& layout,
    const std::vector<GameplaySample>& field) noexcept;

std::uint64_t hydrology_presentation_field_digest(
    const GameplayFieldLayout& layout,
    const std::vector<PresentationSample>& field) noexcept;

struct HydrologySectionTimings {
    std::string id;
    bool cache_hit = false;
    double setup_ms = 0.0;
    double physx_init_ms = 0.0;
    double simulate_ms = 0.0;
    double gpu_mesh_ms = 0.0;
    double cpu_mesh_ms = 0.0;
};

struct HydrologyNetworkTimings {
    std::vector<HydrologySectionTimings> sections;
    double handoff_mesh_ms = 0.0;
    double serialize_ms = 0.0;
    double total_wall_ms = 0.0;
};

struct HydrologyNetworkBakeResult {
    HydrologyNetworkArtifact manifest{};
    std::vector<HydrologyArtifact> sections;
    std::vector<HydrologyHandoffArtifact> handoffs;
    HydrologyNetworkProducts products{};
    gpu_meshing::MeshResult failed_debug_visual;
    HydrologyNetworkTimings timings{};
};

struct HandoffProductSettings {
    gpu_meshing::ParticleJob visual_job{};
    float particle_radius_m = 0.0f;
    GameplayFieldLayout gameplay_layout{};
};

bool build_handoff_artifact(
    const HydrologyArtifact& upstream,
    const HydrologyArtifact& downstream,
    const SpillwayHandoffRecord& handoff,
    const HandoffProductSettings& settings,
    const PhysxFluidBake::VisualMesher& visual_mesher,
    HydrologyHandoffArtifact& artifact,
    HydrologyNetworkProducts& products,
    FluidBakeError& error);

bool validate_handoff_products(
    const HydrologyHandoffArtifact& artifact,
    const HydrologyNetworkProducts& products,
    const SpillwayHandoffRecord& handoff,
    FluidBakeError& error);

bool serialize_handoff_artifact(
    const HydrologyHandoffArtifact& artifact,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error);

bool deserialize_handoff_artifact(
    const std::vector<std::uint8_t>& bytes,
    HydrologyHandoffArtifact& artifact,
    gpu_meshing::Error& error);

bool save_handoff_artifact_atomic(
    const std::filesystem::path& path,
    const HydrologyHandoffArtifact& artifact,
    gpu_meshing::Error& error);

bool load_handoff_artifact_validated(
    const std::filesystem::path& path,
    std::uint64_t expected_semantic_key,
    std::uint64_t expected_upstream_payload_digest,
    std::uint64_t expected_downstream_payload_digest,
    HydrologyHandoffArtifact& artifact,
    gpu_meshing::Error& error);

}  // namespace hydrology
