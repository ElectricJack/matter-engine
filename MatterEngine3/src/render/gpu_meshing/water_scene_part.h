#pragma once

#include "matter/gpu_visual_meshing.h"
#include "render/vk_scene_renderer.h"

#include <cstdint>
#include <memory>

namespace gpu_meshing {

bool build_water_scene_part(
    const MeshResult& mesh, std::uint64_t artifact_digest,
    std::uint32_t material_id,
    std::shared_ptr<const viewer::VkScenePart>& part,
    std::uint64_t& instance_id, Error& error,
    viewer::WaterFieldBinding water_field_binding = {});

// Animation suppresses only the accepted immutable raster proxy while the
// direct animated raster owns visibility. Active and fallback water remain
// raster-only; stable identity, geometry, and field binding stay reversible.
void set_water_scene_animation_active(
    viewer::VkSceneInstance& proxy, bool active) noexcept;

}  // namespace gpu_meshing
