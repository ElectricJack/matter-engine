#pragma once
#include <matter/graphics/vulkan_resources.h>
#include <matter/graphics/composition.h>

namespace matter::graphics {
struct RasterLimits {
  uint64_t max_gpu_bytes = 256ull * 1024 * 1024;
  uint32_t max_dimension = 4096;
  size_t max_triangles = 1000000;
};
struct RasterIdentity {
  data::SemanticId instance;
  std::vector<data::SemanticId> path;
};
struct RasterOutput {
  FloatImage albedo, normal, orm, depth, velocity, reactivity;
  IdentityImage identity;
  std::vector<ShadingMaterial> materials;
  // identity.y is an index here; identity.x is an index in materials.
  // UINT32_MAX in both channels denotes cleared background.
  std::vector<RasterIdentity> instances;
};
// Offscreen SceneFrame mesh producer using the native host's complete raster
// pass and shaders. Requires an injected Vulkan 1.3 graphics device with
// dynamicRendering, synchronization2, bufferDeviceAddress,
// shaderSampledImageArrayNonUniformIndexing, shaderDrawParameters,
// shaderDemoteToHelperInvocation and drawIndirectFirstInstance
// enabled. Uses reversed-Z and the host's negative-height viewport; the
// coordinator supplies the canonical Vulkan zero-to-one projection without
// an additional Y flip, matching native frame_matrices.cpp.
//
// Uniform opaque PBR is supported. Textured appearances fail explicitly.
// Immutable facade geometry is packed per visible placement, applying its
// inverse-transpose normal matrix and winding parity. No world, generator,
// window or renderer singleton is consulted. Calls are thread confined.
class VulkanRaster final {
public:
  explicit VulkanRaster(VulkanDeviceBinding, RasterLimits = {});
  ~VulkanRaster();
  // Synchronous render/readback; all allocations survive fence completion.
  // Output is unchanged on failure. Empty visibility produces a cleared frame.
  data::ValidationResult render(const SceneFrame &, uint32_t width, uint32_t height, RasterOutput &);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
