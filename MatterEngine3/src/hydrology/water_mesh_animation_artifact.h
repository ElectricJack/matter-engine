#pragma once

#include "hydrology/water_mesh_animation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hydrology {

struct PackedWaterAnimationVertex {
    std::uint32_t position_xy_unorm16 = 0;
    std::uint32_t position_z_unorm16_normal_x_snorm16 = 0;
    std::uint32_t normal_y_snorm16_reserved = 0;
};

struct WaterMeshAnimationArtifactMetadata {
    std::string identity;
    std::uint64_t semantic_key = 0;
    std::uint64_t source_primary_payload_digest = 0;
    std::uint64_t source_secondary_payload_digest = 0;
    float visual_voxel_m = 0.0f;
    gpu_meshing::ParticleSamplingLattice lattice{};
};

struct WaterMeshAnimationFrameRecord {
    std::uint64_t vertex_payload_offset = 0;
    std::uint64_t index_payload_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    gpu_meshing::Aabb bounds_m{};
    std::uint64_t content_digest = 0;
};

bool operator==(const WaterMeshAnimationFrameRecord& a,
                const WaterMeshAnimationFrameRecord& b) noexcept;

struct WaterMeshAnimationArtifact {
    std::string identity;
    std::uint64_t semantic_key = 0;
    std::uint64_t source_primary_payload_digest = 0;
    std::uint64_t source_secondary_payload_digest = 0;
    std::uint32_t frames_per_second = 0;
    std::uint32_t phase_offset_frames = 0;
    float duration_seconds = 0.0f;
    float visual_voxel_m = 0.0f;
    gpu_meshing::ParticleSamplingLattice lattice{};
    std::uint32_t material = 0;
    gpu_meshing::Aabb quantization_bounds_m{};
    std::vector<WaterMeshAnimationFrameRecord> frames;
    std::vector<std::uint8_t> frame_payload;
    std::uint64_t payload_digest = 0;
};

struct WaterMeshAnimationFrameSpan {
    const std::uint8_t* vertex_data = nullptr;
    const std::uint8_t* index_data = nullptr;
    std::size_t vertex_bytes = 0;
    std::size_t index_bytes = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
};

bool pack_water_mesh_animation_artifact(
    const WaterMeshAnimationArtifactMetadata& metadata,
    const WaterMeshAnimation& animation,
    WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error);

bool serialize_water_mesh_animation_artifact(
    const WaterMeshAnimationArtifact& artifact,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error);

bool deserialize_water_mesh_animation_artifact(
    const std::vector<std::uint8_t>& bytes,
    WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error);

bool water_mesh_animation_frame_span(
    const WaterMeshAnimationArtifact& artifact,
    std::uint32_t frame_index,
    WaterMeshAnimationFrameSpan& span,
    gpu_meshing::Error& error);

// Diagnostic count used by playback/performance regression tests. Loading and
// serialization are expected to validate; steady-state frame selection is not.
std::uint64_t water_mesh_animation_validation_count() noexcept;

bool decode_water_mesh_animation_frame(
    const WaterMeshAnimationArtifact& artifact,
    std::uint32_t frame_index,
    gpu_meshing::MeshResult& mesh,
    gpu_meshing::Error& error);

bool save_water_mesh_animation_artifact_immutable(
    const std::filesystem::path& path,
    const WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error);

bool load_water_mesh_animation_artifact(
    const std::filesystem::path& path,
    WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error);

}  // namespace hydrology
