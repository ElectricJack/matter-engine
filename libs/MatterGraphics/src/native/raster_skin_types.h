#pragma once
#include <cstdint>
namespace viewer {
// One accepted work item has exactly one immutable raster mapping.  The
// output offsets are vertex indices (not bytes); record_raster converts them
// to VkSkinVertex binding offsets.  Keeping this beside the work queue makes
// sorting and fallback selection transactional.
struct VkSkinRasterDraw {
    uint64_t asset_key = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t source_vertex = 0;
    // Part-LOCAL first vertex of this range. The shared index buffer stores
    // PART-LOCAL index values (matter_engine.cpp rebases each mesh by its
    // offset within the part and no further; vk_scene_renderer never rewrites
    // them), so a skinned draw must subtract exactly this to land on the
    // compute output. Subtracting the renderer-global source_vertex instead
    // over-rebases by the part's arena base -- invisible whenever that base is
    // 0, which is every fixture but not real content.
    uint32_t local_vertex_base = 0;
    uint32_t output_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    // Selects the fence-owned frame output buffer. Current work points at the
    // frame being built; retained fallbacks may point at an older, still
    // sealed frame slot.
    uint32_t output_frame_slot = 0;
    uint32_t lod = 0;
    uint32_t cluster = 0;
    uint32_t flags = 0;
};

// Compute output, consumed directly as the skinned raster vertex buffer.
// `previous_position` is the previous frame's skinned position of the SAME
// vertex, which is what makes deformation motion vectors possible; when
// history is invalid it ends up equal to `position` (see
// kVkSkinHistoryInvalid above). `tint`, `surface` and `material_index` are
// copied through from the source vertex unchanged; only position and normal
// are actually skinned.
struct alignas(16) VkSkinVertex {
    float position[4]{};
    float previous_position[4]{};
    float normal[4]{};
    float tint[4]{};
    float surface[4]{};
    uint32_t material_index = 0;
    uint32_t pad[3]{};
};
static_assert(sizeof(VkSkinVertex) == 96, "VkSkinVertex std430 ABI");

}
