#pragma once
#include "raster_types.h"
#include "vk_draw_command.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace viewer {
// Borrowed image state. The owner retains allocation and layout bookkeeping.
struct RasterImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout* layout = nullptr;
    uint32_t mip_levels = 1, array_layers = 1;
};
struct RasterExtension {
    void* owner = nullptr;
    void (*record)(VkCommandBuffer, void*) = nullptr;
    explicit operator bool() const noexcept { return record != nullptr; }
    void operator()(VkCommandBuffer cmd) const { record(cmd, owner); }
};
// Complete production mesh pass. No world, renderer, generator or device owner.
// Callers retain every referenced allocation through GPU retirement.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4324)
#endif
struct MeshRasterRecord {
    RasterImage *albedo{}, *normal{}, *orm{}, *velocity{}, *material_instance{},
        *reactivity{}, *vt_feedback{}, *depth{}, *hdr{};
    VkExtent2D extent{};
    VkPipeline raster_pipeline = VK_NULL_HANDLE, skinned_raster_pipeline = VK_NULL_HANDLE;
    // Clockwise-front twin of raster_pipeline. When set, `indirect_buffer`
    // carries a second command table at mirror_command_base (same indexing,
    // same draw_ranges) holding the instances whose transform mirrors them.
    VkPipeline mirrored_raster_pipeline = VK_NULL_HANDLE;
    uint32_t mirror_command_base = 0;
    VkPipelineLayout raster_layout = VK_NULL_HANDLE;
    VkDescriptorSet raster_sets[2]{};
    VkBuffer vertex_buffer = VK_NULL_HANDLE, index_buffer = VK_NULL_HANDLE,
        indirect_buffer = VK_NULL_HANDLE;
    uint32_t static_command_count = 0, index_count = 0;
    const VkBuffer* skin_vertex_buffers = nullptr;
    const VkBuffer* skin_previous_vertex_buffers = nullptr;
    const uint32_t* skin_vertex_counts = nullptr;
    uint32_t skin_buffer_count = 0;
    const VkSkinRasterDraw* skin_draws = nullptr;
    uint32_t skin_draw_count = 0, draw_transform_slots = 0, skin_transform_base = 0;
    const PartCommandRange* draw_ranges = nullptr;
    uint32_t draw_range_count = 0, max_draw_indirect_count = 1;
    std::vector<PartCommandRange>* recorded_draw_ranges = nullptr;
    bool native_ray_tracing_available = false;
    VkQueryPool ts_pool = VK_NULL_HANDLE;
    uint8_t* ts_written = nullptr;
    uint32_t gbuffer_zone = 0, vt_zone = 0, vt_feedback_zone = 0, frame_slot = 0;
    RasterExtension vt_pre, vt_post, sparse_draw;
    VkBuffer water_vertices = VK_NULL_HANDLE, water_indices = VK_NULL_HANDLE;
    VkDeviceSize water_vertex_bytes = 0, water_index_bytes = 0;
    bool water_upload_required = false;
    VkQueryPool raster_stats_pool = VK_NULL_HANDLE;
    RasterDebugPushConstants raster_debug_push{};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
void record_mesh_gbuffer(VkCommandBuffer, const MeshRasterRecord&);
} // namespace viewer
