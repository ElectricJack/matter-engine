#pragma once

// Engine-side graphics adapter: publishes the world's portable-authored part
// instances to MatterGraphics (Graphics -> SceneBackend on a VulkanResourceDevice
// leased from the engine's VulkanDevice). Each instance uploads the SAME retained
// AssetProduct its .part was baked from (authoring/engine/legacy_part_route.h),
// placed with the session's admitted object_to_world, live camera and sun.
//
// Swapchain presentation stays with the legacy renderer: it does not consume
// the SceneFrame. MATTER_PORTABLE_SCENE_CAPTURE=<prefix> rasterizes the frame
// with VulkanRaster once the scene has been stable for
// MATTER_PORTABLE_SCENE_CAPTURE_FRAMES frames (default 30), writing
// <prefix>.albedo.png, .normal.pfm, .depth.pfm, .identity.pfm and .json.

#include "matter/math_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace viewer { struct VkSceneInstance; }

namespace matter {
class VulkanDevice;

namespace graphics_adapter {

struct PortableView {
    Mat4f world_to_view{}, view_to_clip{};
    Float3 sun_direction{}, sun_color{};
    uint32_t width = 0, height = 0;
};

class PortableScene {
public:
    explicit PortableScene(VulkanDevice& device);
    ~PortableScene();
    PortableScene(const PortableScene&) = delete;
    PortableScene& operator=(const PortableScene&) = delete;

    // Mirrors `instances` (re-read only when `instances_changed`) into the
    // scene and advances camera/lights; a product retained later republishes
    // the unchanged list. Parts without a retained product are legacy content
    // and are skipped; a portable hash whose product could not be built, or a
    // product the adapter cannot consume, is refused once, logged, and left
    // to the legacy renderer. False only when the device cannot be leased or
    // the graphics service rejects a frame.
    bool update(const std::vector<viewer::VkSceneInstance>& instances,
                bool instances_changed, const PortableView& view,
                std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace graphics_adapter
} // namespace matter
