#pragma once

#include "hydrology/hydrology_artifact.h"
#include "hydrology/hydrology_field_artifact.h"
#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/physx_fluid_bake.h"
#include "hydrology/spillway_handoff.h"
#include "hydrology/water_mesh_animation_artifact.h"
#include "hydrology/water_mesh_continuity.h"

#include <array>
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

struct HydrologySectionTimings {
    std::string id;
    bool cache_hit = false;
    double setup_ms = 0.0;
    double physx_init_ms = 0.0;
    double simulate_ms = 0.0;
    double gpu_mesh_ms = 0.0;
    double animation_capture_ms = 0.0;
    double animation_mesh_ms = 0.0;
    double animation_serialize_ms = 0.0;
    double cpu_mesh_ms = 0.0;
    std::uint64_t animation_bytes = 0u;
    std::uint64_t animation_device_capture_bytes = 0u;
    std::uint64_t animation_semantic_key = 0u;
    std::uint64_t animation_payload_digest = 0u;
    std::uint32_t animation_capture_first_step = 0u;
    std::uint32_t animation_capture_last_step = 0u;
    std::vector<std::uint32_t> animation_capture_particle_counts;
    std::vector<std::uint32_t> animation_frame_vertex_counts;
    std::vector<std::uint32_t> animation_frame_triangle_counts;
    std::vector<std::uint64_t> boundary_source_semantic_keys;
    std::vector<std::uint64_t> boundary_source_payload_digests;
    bool animation_cache_hit = false;
};

struct HandoffFieldContinuityMetrics {
    std::uint32_t sample_pairs = 0;
    float maximum_height_delta_m = 0.0f;
    float minimum_normal_dot = 1.0f;
    float maximum_turbulence_delta = 0.0f;
    float maximum_aeration_delta = 0.0f;
    float maximum_foam_delta = 0.0f;
    bool feature_labels_deterministic = false;
};

struct MeshIndexRange {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct HandoffFrameProducts {
    gpu_meshing::MeshResult replacement_strip;
    MeshIndexRange upstream_band{};
    MeshIndexRange collar{};
    MeshIndexRange downstream_band{};
};

struct HandoffAnimationBuildInput {
    const WaterBoundaryAnimationSource* upstream = nullptr;
    const WaterBoundaryAnimationSource* downstream = nullptr;
    const WaterMeshAnimationArtifact* upstream_bulk = nullptr;
    const WaterMeshAnimationArtifact* downstream_bulk = nullptr;
    SpillwayHandoffRecord handoff{};
    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::ParticleJob visual_template{};
};

struct HandoffAnimationBuildDiagnostics {
    std::array<double, 30> frame_mesh_ms{};
    std::array<WaterCutContourMetrics, 30> upstream_cut{};
    std::array<WaterCutContourMetrics, 30> downstream_cut{};
    std::array<MeshIndexRange, 30> upstream_band{};
    std::array<MeshIndexRange, 30> collar{};
    std::array<MeshIndexRange, 30> downstream_band{};
    std::uint64_t artifact_file_bytes = 0;
    std::uint64_t peak_build_cpu_payload_bytes = 0;
    std::uint32_t peak_decoded_boundary_frames = 0;
    std::uint32_t retained_temporary_dam_support_contributors = 0;
    bool source_blend_required = false;
};

struct HydrologyHandoffTimings {
    std::string id;
    std::uint64_t semantic_key = 0;
    std::uint64_t payload_digest = 0;
    std::uint64_t animation_semantic_key = 0;
    std::uint64_t animation_payload_digest = 0;
    bool static_cache_hit = false;
    bool animation_cache_hit = false;
    std::array<double, 30> animation_frame_ms{};
    std::uint64_t animation_file_bytes = 0;
    std::uint64_t boundary_source_bytes = 0;
    std::uint64_t peak_build_cpu_payload_bytes = 0;
    std::array<WaterCutContourMetrics, 30> upstream_cut{};
    std::array<WaterCutContourMetrics, 30> downstream_cut{};
    HandoffFieldContinuityMetrics upstream_field{};
    HandoffFieldContinuityMetrics downstream_field{};
    std::uint32_t retained_temporary_dam_support_contributors = 0;
    bool loop_frame_29_to_0_synchronized = false;
    bool source_blend_required = false;
};

struct HydrologyNetworkTimings {
    std::vector<HydrologySectionTimings> sections;
    std::vector<HydrologyHandoffTimings> handoffs;
    double handoff_mesh_ms = 0.0;
    double handoff_animation_mesh_ms = 0.0;
    double serialize_ms = 0.0;
    double total_wall_ms = 0.0;
    std::uint64_t peak_build_cpu_payload_bytes = 0;
};

struct HydrologyNetworkBakeResult {
    HydrologyNetworkArtifact manifest{};
    std::vector<HydrologyArtifact> sections;
    std::vector<HydrologyHandoffArtifact> handoffs;
    std::vector<WaterMeshAnimationArtifact> section_animations;
    std::vector<WaterMeshAnimationArtifact> handoff_animations;
    HydrologyNetworkProducts products{};
    gpu_meshing::MeshResult failed_debug_visual;
    HydrologyNetworkTimings timings{};
};

std::string hydrology_network_timing_trace_json(
    const HydrologyNetworkBakeResult& result);

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

bool measure_handoff_field_continuity(
    const HydrologyNetworkProducts& products,
    const SpillwayHandoffRecord& handoff,
    HandoffFieldContinuityMetrics& upstream_cut,
    HandoffFieldContinuityMetrics& downstream_cut,
    FluidBakeError& error);

bool build_handoff_water_animation_artifact(
    const HandoffAnimationBuildInput& input,
    const PhysxFluidBake::VisualMesher& mesher,
    WaterMeshAnimationArtifact& artifact,
    HandoffAnimationBuildDiagnostics& diagnostics,
    FluidBakeError& error);

bool build_handoff_animation_frames(
    const HandoffAnimationBuildInput& input,
    const PhysxFluidBake::VisualMesher& mesher,
    WaterMeshAnimation& output,
    HandoffAnimationBuildDiagnostics& diagnostics,
    FluidBakeError& error);

bool checked_handoff_animation_workset_bytes(
    const std::array<std::uint64_t, 4>& decoded_position_counts,
    std::uint64_t particle_sample_capacity,
    std::uint64_t& bytes) noexcept;

std::uint64_t derive_handoff_animation_semantic_key(
    const HandoffAnimationBuildInput& input,
    std::uint64_t upstream_animation_payload_digest,
    std::uint64_t downstream_animation_payload_digest);

bool clip_section_water_mesh_animation(
    const WaterMeshAnimation& source,
    const std::string& section_id,
    const std::vector<SpillwayHandoffRecord>& handoffs,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    WaterMeshAnimation& owned,
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
