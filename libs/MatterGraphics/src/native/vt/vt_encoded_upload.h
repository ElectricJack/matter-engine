#pragma once
#include "vt_encoded_pages.h"
#include "vt_types.h"

namespace vt::encoded {
// A slice of an already allocated, HOST_VISIBLE|HOST_COHERENT transfer buffer.
// Recorder owns the slice exclusively until its submission retires. The caller
// retains/recycles its bank lease; this function allocates/submits/waits nothing.
struct UploadSpan {
    VkBuffer buffer = VK_NULL_HANDLE;
    void* mapped = nullptr; // base of buffer, not base of slice
    size_t buffer_bytes = 0, offset = 0;
};
inline bool record_upload(VkCommandBuffer cmd, const Bundle& bundle, const Key& expected,
    const VtFillRequest& request, UploadSpan staging, bool requires_draw_geometry,
    VtDrawGeometry geometry = {}) {
    static_assert(kExtent == kVtPageStride && kVtChannelCount == 5, "cached VT channel ABI");
    const auto* page = bundle.find(expected);
    if (!cmd || !bundle.lease || !page || !request.pool || request.pool->uncompressed ||
        request.export_points || expected.rung != request.rung || expected.mip != request.mip ||
        expected.x != request.page_x || expected.y != request.page_y ||
        !staging.buffer || !staging.mapped || staging.offset%16 || staging.offset > staging.buffer_bytes ||
        kPixelBytes > staging.buffer_bytes-staging.offset) return false;
    // Cached pixels cannot reconstruct POM addresses. That separate immutable
    // geometry lease must be ready when this rendering path requires it.
    if (requires_draw_geometry && page->height.version &&
        (!geometry.lifetime || !geometry.gpu.charts || !geometry.gpu.triangles ||
         !geometry.gpu.chart_count || !geometry.gpu.triangle_count)) return false;
    constexpr VkFormat formats[] = {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM};
    for (size_t channel = 0; channel < kChannelBytes.size(); ++channel)
        if (!request.pool->image[channel] || request.pool->format[channel] != formats[channel]) return false;
    uint32_t layer, x, y;
    vt_slot_origin(request.physical_slot, layer, x, y);
    if (layer >= request.pool->layer_count || x+kExtent > request.pool->layer_edge_texels ||
        y+kExtent > request.pool->layer_edge_texels) return false;
    // Validate everything before touching staging memory or publishing success.
    std::memcpy(static_cast<uint8_t*>(staging.mapped)+staging.offset, page->pixels, kPixelBytes);
    VkBufferImageCopy copy{};
    copy.bufferOffset = staging.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    copy.imageOffset = {int32_t(x), int32_t(y), 0};
    copy.imageExtent = {kExtent, kExtent, 1};
    const auto layout = request.pool->transfer_dst_layout
        ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    for (size_t channel = 0; channel < kChannelBytes.size(); ++channel) {
        vkCmdCopyBufferToImage(cmd, staging.buffer, request.pool->image[channel], layout, 1, &copy);
        copy.bufferOffset += kChannelBytes[channel];
    }
    request.mark_filled({page->height.minimum, page->height.range, page->height.version},
                       std::move(geometry));
    return true;
}
} // namespace vt::encoded
