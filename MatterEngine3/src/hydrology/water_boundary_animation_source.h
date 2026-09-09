#pragma once

#include "hydrology/physx_fluid_types.h"
#include "hydrology/spillway_handoff.h"
#include "matter/gpu_visual_meshing.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hydrology {

struct WaterBoundaryCaptureFrameRecord {
    std::uint32_t simulation_step = 0;
    std::uint64_t position_byte_offset = 0;
    std::uint32_t particle_count = 0;
    std::uint64_t content_digest = 0;
};

struct WaterBoundaryAnimationSource {
    std::string section_id;
    std::uint64_t source_section_payload_digest = 0;
    std::uint64_t handoff_semantic_key = 0;
    gpu_meshing::ParticleSamplingLattice lattice{};
    std::uint32_t frames_per_second = 0;
    std::uint32_t phase_offset_frames = 0;
    float particle_radius_m = 0.0f;
    float blend_width_m = 0.0f;
    gpu_meshing::Aabb crop_bounds_m{};
    std::vector<WaterBoundaryCaptureFrameRecord> frames;
    std::vector<std::uint8_t> quantized_positions;
    std::uint64_t payload_digest = 0;
};

bool build_water_boundary_animation_source(
    const FluidParticleAnimationCapture& capture,
    std::string_view section_id,
    std::uint64_t source_section_payload_digest,
    const SpillwayHandoffRecord& handoff,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    float particle_radius_m,
    float blend_width_m,
    bool upstream_endpoint,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error);

bool decode_water_boundary_frame(
    const WaterBoundaryAnimationSource& source,
    std::uint32_t frame_index,
    std::vector<matter::Float3>& positions_m,
    gpu_meshing::Error& error);

bool water_boundary_source_contains(
    const WaterBoundaryAnimationSource& source,
    const matter::Float3& position_m) noexcept;

bool serialize_water_boundary_animation_source(
    const WaterBoundaryAnimationSource& source,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error);

bool deserialize_water_boundary_animation_source(
    const std::vector<std::uint8_t>& bytes,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error);

bool save_water_boundary_animation_source_immutable(
    const std::filesystem::path& path,
    const WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error);

bool load_water_boundary_animation_source(
    const std::filesystem::path& path,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error);

}  // namespace hydrology
