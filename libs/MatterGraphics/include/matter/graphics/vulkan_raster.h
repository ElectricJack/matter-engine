#pragma once
#include <matter/graphics/raster.h>
#include <matter/graphics/vulkan_resources.h>

namespace matter::graphics {
// Synchronous, thread-confined graphics producer. Owns all native allocations,
// retains them through submission, and publishes only completed readbacks.
// Inputs are immutable/borrowed through render; scene revisions need no global
// cache invalidation. Shared geometry uploads once per frame.
class VulkanRaster final {
public:
  explicit VulkanRaster(VulkanDeviceBinding, RasterLimits = {});
  ~VulkanRaster();
  data::ValidationResult render(const RasterInputs &, GbufferOutput &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
