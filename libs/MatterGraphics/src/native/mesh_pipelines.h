#pragma once
#include "raster_types.h"
#include <vulkan/vulkan.h>
#include <functional>
#include <string>
namespace viewer {
struct MeshRasterPipelines {
    VkPipeline raster = VK_NULL_HANDLE, skin = VK_NULL_HANDLE, water = VK_NULL_HANDLE,
        visibility = VK_NULL_HANDLE, wireframe = VK_NULL_HANDLE, wireframe_skin = VK_NULL_HANDLE;
};
using RasterShaderLoader = std::function<bool(VkDevice, const char*, VkShaderModule&, std::string&)>;
using RasterPipelineCreator = std::function<VkResult(const VkGraphicsPipelineCreateInfo&, VkPipeline*)>;
// The owner destroys output handles after their last submitted use, including
// partially created outputs on failure. Shader modules retire inside this call.
bool create_mesh_raster_pipelines(VkDevice, VkPipelineLayout, bool wireframe_available,
    const RasterShaderLoader&, const RasterPipelineCreator&, MeshRasterPipelines&, std::string&);
} // namespace viewer
