#pragma once
#include "matter/vulkan_device.h"
#include "vt_compositor.h"
#include "vt_snapshot.h"
#include <array>
#include <functional>
namespace asset_export {
struct Mesh;
struct Material;
} // namespace asset_export
namespace part_surface {
struct Prepared;
}
namespace vt {
struct VtExportPoint {
  uint32_t triangle = UINT32_MAX;
  float barycentric[3]{};
};
static_assert(sizeof(VtExportPoint) == 16);
struct VtExportAtlas {
  uint32_t width = 0, height = 0;
  // Original uncompressed RGBA8 albedo, canonical normal XY, ORM and AUX.
  std::array<std::vector<uint8_t>, 4> channels;
  std::vector<uint16_t> heights;
  std::vector<VtExportPoint> points;
  VtPageHeight height_decode;
};
// Offline authoring call on the Vulkan app thread, outside frame recording.
// Uses a PRIVATE compositor and bounded eight-page scratch pool. No streaming
// residency, camera feedback, live page tables or screen visibility dependency.
// Inputs and GPU allocations remain owned through every submitted batch.
// Failure/cancellation preserves out; never call with the live frame
// compositor.
bool vt_export_atlas(matter::VulkanDevice &device,
                     std::shared_ptr<VtCompositor> compositor,
                     std::shared_ptr<const VtPartSnapshot> inputs,
                     VtExportAtlas &out, std::string &error,
                     const std::function<bool()> &cancelled = {},
                     uint32_t mip = 0);

// Produces standard UV tangent normals, sRGB color, linear ORM and signed
// height. The affine transform is applied to geometry and every texture normal.
bool vt_export_mesh(matter::VulkanDevice &, std::shared_ptr<VtCompositor>,
                    std::shared_ptr<const VtPartSnapshot>,
                    const part_surface::Prepared *, const float transform[16],
                    const std::vector<float> &baked_ao, asset_export::Mesh &,
                    asset_export::Material &, std::string &error,
                    const std::vector<std::array<float, 2>> &clearcoat = {});
} // namespace vt
