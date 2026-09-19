#include "vt_export.h"
#include "vk_resources.h"
#include <cstring>
#include <exception>
namespace vt {
namespace {
constexpr uint32_t batch_size = 8, page = VtCompositor::kPageStore,
                   pixels = page * page;
constexpr uint32_t pool_width = page * batch_size,
                   pool_pixels = pool_width * page;
struct Resources {
  std::shared_ptr<VtCompositor> compositor;
  std::shared_ptr<const VtPartSnapshot> inputs;
  matter::VkImageResource images[kVtChannelCount];
  matter::VkBufferResource readback, points;
  VtPoolBinding pool;
  std::array<VtFillRequest, batch_size> requests;
  std::array<bool, batch_size> filled{};
  std::array<VtPageHeight, batch_size> height{};
  uint32_t count = 0;
  bool initialized = false;
};
void record(VkCommandBuffer cmd, void *user) {
  auto &r = *static_cast<Resources *>(user);
  std::array<VkImageMemoryBarrier2, kVtChannelCount> transitions{};
  for (uint32_t c = 0; c < kVtChannelCount; ++c) {
    auto &b = transitions[c];
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = r.initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                   : VK_PIPELINE_STAGE_2_NONE;
    b.srcAccessMask = r.initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0;
    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout =
        r.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = r.images[c].image;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = kVtChannelCount;
  dep.pImageMemoryBarriers = transitions.data();
  vkCmdPipelineBarrier2(cmd, &dep);
  r.initialized = true;
  r.compositor->fill(cmd, r.requests.data(), r.count);
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
  for (uint32_t c = 0; c < kVtChannelCount; ++c) {
    VkBufferImageCopy region{};
    region.bufferOffset = VkDeviceSize(c) * pool_pixels * 4;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {pool_width, page, 1};
    vkCmdCopyImageToBuffer(cmd, r.images[c].image, VK_IMAGE_LAYOUT_GENERAL,
                           r.readback.buffer, 1, &region);
  }
  barrier.srcStageMask =
      VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.srcAccessMask =
      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace
bool vt_export_atlas(matter::VulkanDevice &device,
                     std::shared_ptr<VtCompositor> compositor,
                     std::shared_ptr<const VtPartSnapshot> inputs,
                     VtExportAtlas &out, std::string &error,
                     const std::function<bool()> &cancelled, uint32_t mip) {
  const auto fail = [&](const char *why) {
    error = why;
    return false;
  };
  try {
    if (!compositor || !inputs || !inputs->owns_context_inputs())
      return fail(
          "export requires an owned material snapshot and private compositor");
    const auto &atlas = inputs->geometry->atlas;
    if (mip > 7 || (!inputs->context.periodic.version && mip != 0))
      return fail("export mip is unsupported for this material");
    const uint32_t width = std::max(atlas.atlas_w >> mip, 1u),
                   height = std::max(atlas.atlas_h >> mip, 1u);
    const uint64_t count = uint64_t(width) * height;
    if (!atlas.atlas_w || !atlas.atlas_h || atlas.atlas_w > chart_atlas::kVtMaxAtlasDim ||
        atlas.atlas_h > chart_atlas::kVtMaxAtlasDim || count > 16u * 1024u * 1024u)
      return fail("export atlas exceeds 16 million texels or is empty");
    auto r = std::make_shared<Resources>();
    r->compositor = std::move(compositor);
    r->inputs = std::move(inputs);
    r->pool.layer_count = 1;
    r->pool.layer_edge_texels = pool_width;
    r->pool.transfer_dst_layout = false;
    r->pool.uncompressed = true;
    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
      const auto format = c == kVtChannelHeight ? VK_FORMAT_R16_UNORM
                                                : VK_FORMAT_R8G8B8A8_UNORM;
      if (!matter::create_image(
              device, VK_IMAGE_TYPE_2D, format, {pool_width, page, 1},
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
              r->images[c], error))
        return false;
      r->pool.image[c] = r->images[c].image;
      r->pool.format[c] = format;
    }
    if (!matter::create_buffer(device, VkDeviceSize(pool_pixels) * 18,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               r->readback, error) ||
        !matter::create_buffer(
            device, VkDeviceSize(batch_size) * pixels * sizeof(VtExportPoint),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, r->points, error) ||
        !matter::map_buffer(r->readback, error) ||
        !matter::map_buffer(r->points, error))
      return false;
    VtExportAtlas result;
    result.width = width;
    result.height = height;
    for (auto &c : result.channels)
      c.resize(size_t(count) * 4);
    result.heights.resize(size_t(count));
    result.points.resize(size_t(count));
    const uint32_t nx = (width + 127) / 128, ny = (height + 127) / 128;
    for (uint32_t start = 0; start < nx * ny; start += batch_size) {
      if (cancelled && cancelled())
        return fail("asset texture export cancelled");
      r->count = std::min(batch_size, nx * ny - start);
      r->filled.fill(false);
      for (uint32_t s = 0; s < r->count; ++s) {
        auto &q = r->requests[s];
        q = {};
        q.variant_hash = r->inputs->context.variant_hash;
        q.rung = uint16_t(r->inputs->context.rung);
        q.mip = uint16_t(mip);
        q.page_x = uint16_t((start + s) % nx);
        q.page_y = uint16_t((start + s) / nx);
        q.physical_slot = s;
        q.atlas = &atlas;
        q.part_context = &r->inputs->context;
        q.pool = &r->pool;
        q.out_filled = &r->filled[s];
        q.out_height = &r->height[s];
        q.export_points = r->points.address +
                          VkDeviceSize(s) * pixels * sizeof(VtExportPoint);
      }
      if (!matter::submit_immediate(
              device, record, r.get(), error,
              matter::ImmediateSubmitPhase::compute_dispatch, {r}))
        return false;
      if (!matter::invalidate_buffer(r->readback, 0, r->readback.size, error) ||
          !matter::invalidate_buffer(r->points, 0, r->points.size, error))
        return false;
      const auto *raw = static_cast<const uint8_t *>(r->readback.mapped);
      const auto *points = static_cast<const VtExportPoint *>(r->points.mapped);
      for (uint32_t s = 0; s < r->count; ++s) {
        if (!r->filled[s])
          return fail("material compositor refused an export page");
        if (start + s == 0)
          result.height_decode = r->height[s];
        else if (result.height_decode.min_m != r->height[s].min_m ||
                 result.height_decode.range_m != r->height[s].range_m ||
                 result.height_decode.version != r->height[s].version)
          return fail("export page height ranges disagree");
        for (uint32_t y = 0; y < 128; ++y)
          for (uint32_t x = 0; x < 128; ++x) {
            const uint32_t dx = r->requests[s].page_x * 128 + x,
                           dy = r->requests[s].page_y * 128 + y;
            if (dx >= result.width || dy >= result.height)
              continue;
            const size_t dst = size_t(dy) * result.width + dx,
                         src = size_t(y + 4) * pool_width + s * page + x + 4;
            for (uint32_t c = 0; c < 4; ++c)
              std::memcpy(result.channels[c].data() + dst * 4,
                          raw + size_t(c) * pool_pixels * 4 + src * 4, 4);
            std::memcpy(&result.heights[dst],
                        raw + size_t(4) * pool_pixels * 4 + src * 2, 2);
            result.points[dst] =
                points[size_t(s) * pixels + (y + 4) * page + x + 4];
          }
      }
    }
    if (cancelled && cancelled())
      return fail("asset texture export cancelled");
    out = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
} // namespace vt
