#pragma once
#include "vt_encoded_upload.h"

namespace vt::encoded {
// Capture a COMPLETE, successfully produced scratch page into a preallocated
// coherent readback slice. The caller may read mapped bytes only after the
// submission fence retires. Pool layouts are restored before returning.
inline bool record_capture(VkCommandBuffer cmd, const VtFillRequest& request, UploadSpan target) {
    if (!cmd || !request.pool || request.pool->uncompressed || request.coverage_only ||
        request.export_points || !request.out_filled || !*request.out_filled || !target.buffer ||
        !target.mapped || target.offset%16 || target.offset > target.buffer_bytes ||
        kPixelBytes > target.buffer_bytes-target.offset) return false;
    constexpr VkFormat formats[] = {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM};
    uint32_t layer, x, y; vt_slot_origin(request.physical_slot, layer, x, y);
    if (layer >= request.pool->layer_count || x+kExtent > request.pool->layer_edge_texels ||
        y+kExtent > request.pool->layer_edge_texels) return false;
    const auto original = request.pool->transfer_dst_layout ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImageMemoryBarrier2 barriers[5]{};
    for (size_t channel = 0; channel < 5; ++channel) {
        if (!request.pool->image[channel] || request.pool->format[channel] != formats[channel]) return false;
        auto& b = barriers[channel]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = original; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = request.pool->image[channel]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 5; dependency.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dependency);
    VkBufferImageCopy copy{}; copy.bufferOffset = target.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    copy.imageOffset = {int32_t(x), int32_t(y), 0}; copy.imageExtent = {kExtent, kExtent, 1};
    for (size_t channel = 0; channel < 5; ++channel) {
        vkCmdCopyImageToBuffer(cmd, request.pool->image[channel], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              target.buffer, 1, &copy);
        copy.bufferOffset += kChannelBytes[channel];
        auto& b = barriers[channel];
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = original;
    }
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = target.buffer; host.offset = target.offset; host.size = kPixelBytes;
    dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &dependency);
    return true;
}
} // namespace vt::encoded
