#pragma once
#include "raster_types.h"
#include <vulkan/vulkan.h>
#include <functional>
#include <string>
namespace viewer {
struct MeshRasterPipelines {
    VkPipeline raster = VK_NULL_HANDLE, skin = VK_NULL_HANDLE, water = VK_NULL_HANDLE,
        visibility = VK_NULL_HANDLE, wireframe = VK_NULL_HANDLE, wireframe_skin = VK_NULL_HANDLE;
    // `raster` with a clockwise front face, for instances whose transform
    // has a negative determinant (a mirror reverses screen-space winding).
    VkPipeline raster_mirrored = VK_NULL_HANDLE;
};
using RasterShaderLoader = std::function<bool(VkDevice, const char*, VkShaderModule&, std::string&)>;
using RasterPipelineCreator = std::function<VkResult(const VkGraphicsPipelineCreateInfo&, VkPipeline*)>;
// The owner destroys output handles after their last submitted use, including
// partially created outputs on failure. Shader modules retire inside this call.
// uniform_pbr_only requires chartless, non-impostor, non-water meshes with no
// texture or surface-detail material flags. Native hosts use the full default.
bool create_mesh_raster_pipelines(VkDevice, VkPipelineLayout, bool wireframe_available,
    const RasterShaderLoader&, const RasterPipelineCreator&, MeshRasterPipelines&, std::string&,
    bool uniform_pbr_only = false);
} // namespace viewer
