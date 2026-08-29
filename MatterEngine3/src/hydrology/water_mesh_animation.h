#pragma once

#include "hydrology/physx_fluid_types.h"
#include "hydrology/water_boundary_animation_source.h"
#include "matter/gpu_visual_meshing.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hydrology {

inline constexpr char kSectionWaterAnimationCacheDomain[] =
    "water-mesh-animation-v3-canonical-cell-ownership";

struct WaterMeshAnimationPhase {
    std::uint32_t primary_capture = 0;
    std::uint32_t secondary_capture = 0;
    float primary_weight = 1.0f;
    float secondary_weight = 0.0f;
};

WaterMeshAnimationPhase water_mesh_animation_phase(
    std::uint32_t frame_index,
    std::uint32_t frame_count,
    std::uint32_t phase_offset_frames) noexcept;

struct WaterMeshAnimation {
    std::uint32_t frames_per_second = 0;
    std::uint32_t phase_offset_frames = 0;
    float duration_seconds = 0.0f;
    std::vector<gpu_meshing::MeshResult> frames;
};

using WaterMeshAnimationMesher = std::function<bool(
    const gpu_meshing::ParticleJob& job,
    gpu_meshing::MeshResult& mesh,
    gpu_meshing::Stats& stats,
    gpu_meshing::Error& error)>;

// C++17-compatible non-owning span. The canonical Windows test graph remains
// C++17, so this is the pointer/count spelling of the plan's endpoint span.
struct WaterBoundaryAnimationSourceSpan {
    const WaterBoundaryAnimationSource* data = nullptr;
    std::size_t size = 0u;
};

bool build_water_mesh_animation(
    const FluidParticleAnimationCapture& capture,
    float particle_radius_m,
    const gpu_meshing::ParticleJob& template_job,
    const WaterMeshAnimationMesher& mesher,
    WaterMeshAnimation& animation,
    gpu_meshing::Error& error,
    WaterBoundaryAnimationSourceSpan endpoint_sources = {});

}  // namespace hydrology
