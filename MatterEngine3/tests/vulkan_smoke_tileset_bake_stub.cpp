// The smoke suite links vk_scene_renderer.cpp, whose bake_tileset() entry
// reaches the full Vulkan .gtex bake orchestrator (tileset_bake_vk.cpp). That
// TU drags the BLAS/TLAS managers, the torus BVH assembler, and the material
// registry into what is otherwise a lean fault-injection harness — and no
// smoke scenario bakes a tileset. Resolve the symbol with an explicit failure
// so an accidental future call fails loudly instead of linking half a bake
// pipeline into every smoke build.
#include "render/tileset_bake_vk.h"

namespace tileset {

bool bake_tileset_vk(matter::VulkanDevice&, const SettledTorus&, uint64_t,
                     const std::string&, const BakeInputs&, bool, bool,
                     std::string& err) {
    err = "tileset bake is not linked into the Vulkan smoke suite";
    return false;
}

}  // namespace tileset
