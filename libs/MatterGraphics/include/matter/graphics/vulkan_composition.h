#pragma once
#include <matter/graphics/composition.h>
#include <matter/graphics/vulkan_resources.h>

namespace matter::graphics {
// Synchronous offscreen render pass using the production composite/display
// shaders. Inputs are borrowed only through render(); output commits after GPU
// completion/readback. No device queue use may bypass the binding's mutex.
// Calls are thread confined. Owns pipelines, images, descriptors and submissions.
// No window or world; resources retire after fence completion.
// This is a composition pass, not a mesh rasterizer or a ray-tracing producer.
class VulkanComposition final {
public:
  explicit VulkanComposition(VulkanDeviceBinding, CompositionLimits = {});
  ~VulkanComposition();
  data::ValidationResult render(const CompositionInputs &, CompositionOutput &);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
